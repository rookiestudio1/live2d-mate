#include "tts_engine_gptsovits.h"

#include <memory>
#include <utility>

#include "core/tts_http.h"
#include "http_json.h"
#include "tts_stream_util.h"

namespace l2m {

GptSovitsEngine::GptSovitsEngine(HttpJson& http, std::function<GptSovitsConfig()> getConfig, QObject* parent) : QObject(parent), http_(http), getConfig_(std::move(getConfig)) {}

std::string GptSovitsEngine::base() const { return normalizeBaseUrl(getConfig_().baseUrl); }

void GptSovitsEngine::isAvailable(std::function<void(bool)> done) {
  const std::string url = base();
  if (url.empty()) {
    done(false);
    return;
  }
  // 不帶參數打 /tts，api_v2.py 會回 400 —— 但那代表服務活著。
  // 只有連線被拒或逾時才算不可用。
  http_.get(url + "/tts", kProbeTimeoutMs, [done](HttpJson::Reply reply) { done(reply.transportError.empty()); });
}

void GptSovitsEngine::listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) {
  // 音色來自設定檔的 presets，不打網路
  done(gptSovitsVoices(getConfig_(), id_), "");
}

const GptSovitsPreset* GptSovitsEngine::resolvePreset(const GptSovitsConfig& config, const std::optional<std::string>& voice) const {
  if (config.presets.empty()) return nullptr;
  if (voice.has_value()) {
    for (const auto& preset : config.presets) {
      if (preset.id == *voice) return &preset;
    }
  }
  return &config.presets.front();
}

void GptSovitsEngine::applyWeights(const HttpCallChainPtr& chain, const GptSovitsConfig& config, const GptSovitsPreset& preset, std::function<void(std::string)> done) {
  struct Target {
    const char* path;
    std::optional<std::string> wanted;
    std::string* applied;
  };

  auto targets =
    std::make_shared<std::vector<Target>>(std::vector<Target>{{"set_gpt_weights", preset.gptWeights, &appliedGptWeights_}, {"set_sovits_weights", preset.sovitsWeights, &appliedSovitsWeights_}});

  const std::string url = base();
  const int timeout = config.timeoutMs;
  auto step = std::make_shared<std::function<void(size_t)>>();

  *step = [this, targets, step, done, url, timeout, chain](size_t index) {
    if (chain->cancelled) return;
    if (index >= targets->size()) {
      done("");
      return;
    }
    const Target& target = (*targets)[index];
    if (!target.wanted.has_value() || target.wanted->empty() || *target.wanted == *target.applied) {
      (*step)(index + 1);
      return;
    }

    const std::string path = target.path;
    const std::string wanted = *target.wanted;
    std::string* applied = target.applied;
    chain->current = http_.get(gptSovitsWeightUrl(url, path, wanted), timeout, [step, done, index, path, wanted, applied](HttpJson::Reply reply) {
      if (!reply.ok()) {
        const std::string detail = reply.transportError.empty() ? describeErrorResponse(reply.status, reply.bodyText()) : reply.transportError;
        done("GPT-SoVITS " + path + " failed - " + detail);
        return;
      }
      *applied = wanted;
      (*step)(index + 1);
    });
  };
  (*step)(0);
}

void GptSovitsEngine::postTts(const HttpCallChainPtr& chain, const GptSovitsConfig& config, const GptSovitsPreset& preset, const std::string& text, double rate, TtsStreamSink sink) {
  const std::string body = buildGptSovitsBody(config, preset, text, rate);
  const std::string mime = gptSovitsMime(config.mediaType);

  chain->current = http_.postJson(base() + "/tts", body, config.timeoutMs, [sink, mime](HttpJson::Reply reply) {
    if (!reply.ok()) {
      const std::string detail = reply.transportError.empty() ? describeErrorResponse(reply.status, reply.bodyText()) : reply.transportError;
      if (sink.onError) sink.onError("GPT-SoVITS /tts failed - " + detail);
      return;
    }
    // 成功時是音訊串流；還回 JSON 就代表其實是錯誤，別讓它變成一段雜訊
    if (reply.contentType.find("application/json") != std::string::npos) {
      if (sink.onError) {
        sink.onError("GPT-SoVITS /tts returned an error - " + describeErrorResponse(reply.status, reply.bodyText()));
      }
      return;
    }
    if (reply.body.empty()) {
      if (sink.onError) sink.onError("GPT-SoVITS returned empty audio");
      return;
    }
    emitWholeBody(sink, reply.body, mime);
  });
}

std::unique_ptr<TtsRequestHandle> GptSovitsEngine::synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) {
  auto chain = std::make_shared<HttpCallChain>();
  const GptSovitsConfig config = getConfig_();

  // 內建播放器只解得了 wav / mp3 / flac。等一兩分鐘的 CPU 推論跑完
  // 才發現放不出來太慘，在送出前就擋掉並講清楚怎麼改。
  if (config.mediaType != "wav") {
    if (sink.onError) {
      sink.onError("GPT-SoVITS media_type \"" + config.mediaType +
                   "\" cannot be decoded by the built-in audio player. "
                   "Set tts.gptsovits.mediaType to \"wav\" in config.json "
                   "(tray: General > Open Config File).");
    }
    return chainHandle(chain);
  }

  const GptSovitsPreset* preset = resolvePreset(config, options.voice);
  if (!preset) {
    if (sink.onError) {
      sink.onError(
        "GPT-SoVITS has no voice presets - add tts.gptsovits.presets to config.json "
        "(tray: General > Open Config File)");
    }
    return chainHandle(chain);
  }

  const GptSovitsPreset chosen = *preset;
  const double rate = options.rate.value_or(1);
  applyWeights(chain, config, chosen, [this, chain, config, chosen, text, rate, sink](std::string error) {
    if (chain->cancelled) return;
    if (!error.empty()) {
      if (sink.onError) sink.onError(std::move(error));
      return;
    }
    postTts(chain, config, chosen, text, rate, sink);
  });
  return chainHandle(chain);
}

}  // namespace l2m
