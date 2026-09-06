#include "tts_engine_custom.h"

#include <QByteArray>
#include <QDebug>
#include <QElapsedTimer>
#include <QNetworkRequest>
#include <QString>
#include <QUrl>

#include <memory>
#include <utility>

#include "core/tts_http.h"
#include "http_json.h"
#include "tts_stream_util.h"

namespace l2m {

CustomTtsEngine::CustomTtsEngine(HttpJson& http, std::function<CustomTtsConfig()> getConfig, QObject* parent) : QObject(parent), http_(http), getConfig_(std::move(getConfig)) {}

void CustomTtsEngine::isAvailable(std::function<void(bool)> done) {
  // 永遠可用，理由見標頭註解（不發請求、也不看設定填完了沒）
  done(true);
}

void CustomTtsEngine::listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) {
  // 空清單但**不是錯誤**：語音是使用者寫在參數樣板裡的，設定頁另外顯示說明
  done({}, "");
}

std::unique_ptr<TtsRequestHandle> CustomTtsEngine::synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) {
  // 樣板裡沒有語速／語音的佔位符，這兩個參數對自訂端點無效（標頭註解有寫）
  (void)options;

  auto chain = std::make_shared<HttpCallChain>();
  const CustomTtsConfig config = getConfig_();
  const CustomTtsRequest request = buildCustomTtsRequest(config, text);
  if (!request.error.empty()) {
    // 設定不完整就不要送出去 —— 這一句與設定頁的紅字提示是同一份字串。
    // 這是同步 onError，最容易被誤讀成「引擎壞了」，所以要講明是設定問題
    qWarning() << "[tts] custom 設定不完整，這一句不會送出：" << QString::fromStdString(request.error);
    if (sink.onError) sink.onError(request.error);
    return chainHandle(chain);
  }

  QNetworkRequest netRequest{QUrl(QString::fromStdString(request.url))};
  if (!request.contentType.empty()) {
    netRequest.setHeader(QNetworkRequest::ContentTypeHeader, QString::fromStdString(request.contentType));
  }
  for (const auto& header : request.headers) {
    netRequest.setRawHeader(QByteArray::fromStdString(header.first), QByteArray::fromStdString(header.second));
  }

  // custom 是本專案唯一「一句話一個 HTTP 請求」的引擎（非串流，被 SegmentedTtsEngine
  // 包成逐句合成），所以每一句的往返時間與回應內容就是漏句診斷的第一手資料。
  // clock 用 shared_ptr 捕獲：回呼是非同步的，lambda 活得比這個 stack frame 久
  auto clock = std::make_shared<QElapsedTimer>();
  clock->start();
  const std::string method = customTtsMethod(config.method) == CustomTtsMethod::Get ? "GET" : "POST";
  qDebug().nospace() << "[tts] custom 請求 " << method.c_str() << " " << request.url.c_str() << "（" << text.size() << " bytes 文字，逾時 " << config.timeoutMs << " ms）";

  const auto handle = [sink, clock](HttpJson::Reply reply) {
    if (!reply.ok()) {
      const std::string detail = reply.transportError.empty() ? describeErrorResponse(reply.status, reply.bodyText()) : reply.transportError;
      // 逾時（預設 120 秒）也走這裡，而逾時是「唸到一半停住」的頭號來源 ——
      // 對照上面那行請求 log 的時間差就知道是不是撞到 timeoutMs
      qWarning().nospace() << "[tts] custom 失敗（HTTP " << reply.status << "，" << clock->elapsed() << " ms）：" << QString::fromStdString(detail);
      if (sink.onError) sink.onError("Custom voice request failed - " + detail);
      return;
    }
    // 狀態碼 200 不代表拿到的是音訊：包一層 JSON 或回 ogg 都會在 miniaudio
    // 那裡靜默失敗成「有氣泡沒聲音」，所以在這裡就攔下來講清楚
    const std::string audioError = customTtsAudioError(reply.contentType, reply.body.size());
    if (!audioError.empty()) {
      qWarning().nospace() << "[tts] custom 回應不是可解的音訊（" << reply.contentType.c_str() << "，" << reply.body.size() << " bytes）：" << QString::fromStdString(audioError);
      if (sink.onError) sink.onError(audioError);
      return;
    }
    qDebug().nospace() << "[tts] custom 回應 HTTP " << reply.status << "，" << reply.contentType.c_str() << "，" << reply.body.size() << " bytes，耗時 " << clock->elapsed() << " ms";
    emitWholeBody(sink, reply.body, reply.contentType.empty() ? "audio/wav" : reply.contentType);
  };

  if (customTtsMethod(config.method) == CustomTtsMethod::Get) {
    chain->current = http_.get(netRequest, config.timeoutMs, handle);
  } else {
    chain->current = http_.post(netRequest, request.body, config.timeoutMs, handle);
  }
  return chainHandle(chain);
}

}  // namespace l2m
