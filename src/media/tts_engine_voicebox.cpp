#include "tts_engine_voicebox.h"

#include <utility>

#include "core/tts_http.h"
#include "http_json.h"
#include "tts_stream_util.h"

namespace l2m {

VoiceboxEngine::VoiceboxEngine(HttpJson& http, std::function<VoiceboxConfig()> getConfig, std::function<std::string()> getUiLocale, QObject* parent)
  : QObject(parent), http_(http), getConfig_(std::move(getConfig)), getUiLocale_(std::move(getUiLocale)) {}

std::string VoiceboxEngine::base() const { return normalizeBaseUrl(getConfig_().baseUrl); }

void VoiceboxEngine::isAvailable(std::function<void(bool)> done) {
  const std::string url = base();
  if (url.empty()) {
    done(false);
    return;
  }
  http_.get(url + "/health", kProbeTimeoutMs, [done](HttpJson::Reply reply) { done(reply.ok()); });
}

void VoiceboxEngine::listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) {
  const VoiceboxConfig config = getConfig_();
  const std::string engineId = id_;
  // 刻意不快取：使用者在 Voicebox 新增 profile 後按「重新偵測語音」就該看得到，
  // 而本機查詢很便宜。
  http_.get(base() + "/profiles", config.timeoutMs, [done, engineId](HttpJson::Reply reply) {
    if (!reply.ok()) {
      const std::string detail = reply.transportError.empty() ? describeErrorResponse(reply.status, reply.bodyText()) : reply.transportError;
      done({}, "Voicebox /profiles failed - " + detail);
      return;
    }
    done(parseVoiceboxProfiles(reply.bodyText(), engineId), "");
  });
}

void VoiceboxEngine::generate(const HttpCallChainPtr& chain, const VoiceboxConfig& config, const std::string& profileId, const std::string& text, TtsStreamSink sink) {
  // 沒設定語言時跟著 UI 語系走，使用者切到日文就該講日文
  const std::string language = config.language.value_or(localeToLangCode(getUiLocale_ ? getUiLocale_() : "en"));
  const std::string body = buildVoiceboxBody(config, profileId, text, language);

  chain->current = http_.postJson(base() + "/generate/stream", body, config.timeoutMs, [sink](HttpJson::Reply reply) {
    // 模型還沒下載時後端會回非 2xx，訊息要原樣透出去才有辦法排查
    if (!reply.ok()) {
      const std::string detail = reply.transportError.empty() ? describeErrorResponse(reply.status, reply.bodyText()) : reply.transportError;
      if (sink.onError) sink.onError("Voicebox /generate/stream failed - " + detail);
      return;
    }
    if (reply.body.empty()) {
      if (sink.onError) sink.onError("Voicebox returned empty audio");
      return;
    }
    emitWholeBody(sink, reply.body, "audio/wav");
  });
}

std::unique_ptr<TtsRequestHandle> VoiceboxEngine::synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) {
  const VoiceboxConfig config = getConfig_();
  auto chain = std::make_shared<HttpCallChain>();

  if (options.voice.has_value() && !options.voice->empty()) {
    generate(chain, config, *options.voice, text, std::move(sink));
    return chainHandle(chain);
  }

  // 沒指定就取第一個 profile。listVoices 自己不吃 chain（它也給設定頁用），
  // 所以取消是靠下面這個 cancelled 護欄擋住後續動作。
  listVoices([this, chain, config, text, sink](std::vector<VoiceInfo> voices, std::string error) {
    if (chain->cancelled) return;
    if (!error.empty()) {
      if (sink.onError) sink.onError(std::move(error));
      return;
    }
    if (voices.empty()) {
      if (sink.onError) {
        sink.onError("Voicebox has no voice profile - create one in the Voicebox app first");
      }
      return;
    }
    generate(chain, config, voices.front().id, text, sink);
  });
  return chainHandle(chain);
}

}  // namespace l2m
