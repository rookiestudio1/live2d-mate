#include "llm_engine_http.h"

#include <QNetworkRequest>
#include <QUrl>

#include <memory>

#include "core/llm_http.h"
#include "core/llm_stream_accumulator.h"
#include "core/sse_parser.h"

namespace l2m {

namespace {

// 模型清單只是一次輕量 GET，不必吃 config.timeoutMs 那種推論等級的逾時
constexpr int kListModelsTimeoutMs = 10000;

QNetworkRequest makeRequest(const LlmHttpRequest& request) {
  QNetworkRequest out{QUrl(QString::fromStdString(request.url))};
  if (!request.contentType.empty()) {
    out.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromStdString(request.contentType));
  }
  for (const auto& [name, value] : request.headers) {
    out.setRawHeader(QByteArray::fromStdString(name), QByteArray::fromStdString(value));
  }
  return out;
}

// 一次 chat 請求的存活狀態。sink 回呼、解析器與取消旗標都掛在這裡，
// 由 postStream 的兩個 lambda 共享持有 —— 引擎本身不記任何請求狀態，
// 多個請求並行互不干擾。
struct ChatSession {
  SseParser parser;
  OpenAiStreamAccumulator openai;
  AnthropicStreamAccumulator anthropic;
  bool anthropicMode = false;
  LlmStreamSink sink;
  std::shared_ptr<bool> cancelled;
  HttpJson::CallPtr call;
  bool openSent = false;
  bool settled = false;  // onDone / onError 已經發過，之後一律閉嘴
  std::string finishReason;

  void dispatch(const std::vector<SseEvent>& events) {
    for (const auto& event : events) {
      const std::vector<LlmStreamUpdate> updates = anthropicMode ? anthropic.feed(event) : openai.feed(event);
      for (const auto& update : updates) {
        if (settled || (cancelled && *cancelled)) return;
        if (!update.error.empty()) {
          settled = true;
          if (sink.onError) sink.onError(update.error);
          if (call) call->cancel();
          return;
        }
        if (!update.model.empty() && !openSent) {
          openSent = true;
          if (sink.onOpen) sink.onOpen(update.model);
        }
        if (!update.textDelta.empty() && sink.onTextDelta) sink.onTextDelta(update.textDelta);
        for (const auto& tool : update.toolCalls) {
          if (sink.onToolCall) sink.onToolCall(tool);
        }
        if (!update.finishReason.empty()) finishReason = update.finishReason;
        if (update.done) {
          settled = true;
          if (sink.onDone) sink.onDone(finishReason);
          // 伺服器已經說完了，把連線收掉，不等它自己關
          if (call) call->cancel();
          return;
        }
      }
    }
  }
};

}  // namespace

HttpLlmEngine::HttpLlmEngine(HttpJson& http, std::function<LlmConfig()> getConfig) : http_(http), getConfig_(std::move(getConfig)) {}

const std::string& HttpLlmEngine::id() const { return id_; }

void HttpLlmEngine::isAvailable(std::function<void(bool)> done) {
  if (done) done(true);
}

void HttpLlmEngine::listModels(std::function<void(std::vector<std::string>, std::string)> done) {
  const LlmConfig config = getConfig_ ? getConfig_() : LlmConfig{};
  const LlmHttpRequest request = buildListModelsRequest(config);
  if (!request.error.empty()) {
    if (done) done({}, request.error);
    return;
  }
  http_.get(makeRequest(request), kListModelsTimeoutMs, [done](HttpJson::Reply reply) {
    if (!done) return;
    if (!reply.transportError.empty()) {
      done({}, "Could not reach the LLM endpoint: " + reply.transportError);
      return;
    }
    if (!reply.ok()) {
      done({}, describeLlmErrorResponse(reply.status, reply.bodyText()));
      return;
    }
    std::vector<std::string> models = parseModelListJson(reply.bodyText());
    if (models.empty()) {
      done({},
           "The endpoint answered but returned no models. Check the base URL points at "
           "an OpenAI-compatible /v1 root.");
      return;
    }
    done(std::move(models), "");
  });
}

std::unique_ptr<LlmRequestHandle> HttpLlmEngine::chat(const std::vector<LlmMessage>& messages, const LlmChatOptions& options, LlmStreamSink sink) {
  const LlmConfig config = getConfig_ ? getConfig_() : LlmConfig{};
  const LlmHttpRequest request = buildLlmChatRequest(config, messages, options, /*stream=*/true);

  auto cancelled = std::make_shared<bool>(false);
  if (!request.error.empty()) {
    // 同步失敗也要回 no-op handle，呼叫端才不必判空（tts_types.h 的慣例）
    if (sink.onError) sink.onError(request.error);
    return std::make_unique<FunctionLlmHandle>(cancelled);
  }

  auto session = std::make_shared<ChatSession>();
  session->anthropicMode = config.provider == "anthropic";
  session->sink = std::move(sink);
  session->cancelled = cancelled;

  QNetworkRequest qreq = makeRequest(request);
  qreq.setRawHeader("Accept", "text/event-stream");

  session->call = http_.postStream(
    qreq, request.body, config.timeoutMs,
    [session](const char* data, size_t size) {
      if (session->settled || *session->cancelled) return;
      session->dispatch(session->parser.feed(data, size));
    },
    [session](HttpJson::Reply reply) {
      if (session->settled || *session->cancelled) return;
      if (!reply.transportError.empty()) {
        session->settled = true;
        if (session->sink.onError) {
          session->sink.onError("LLM request failed: " + reply.transportError);
        }
        return;
      }
      if (!(reply.status >= 200 && reply.status < 300)) {
        session->settled = true;
        if (session->sink.onError) {
          session->sink.onError(describeLlmErrorResponse(reply.status, reply.bodyText()));
        }
        return;
      }
      // 連線正常關閉但沒收到結束事件：把殘尾派發完，直接視為完成 ——
      // 有些相容服務不送 [DONE] 就關連線
      session->dispatch(session->parser.finish());
      if (!session->settled) {
        session->settled = true;
        if (session->sink.onDone) session->sink.onDone(session->finishReason);
      }
    });

  // cancel 只要立旗標＋中止傳輸：postStream 保證 cancel 後不再回呼
  return std::make_unique<FunctionLlmHandle>(cancelled, [call = session->call] { call->cancel(); });
}

}  // namespace l2m
