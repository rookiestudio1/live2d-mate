#pragma once

// Voicebox 服務（預設 127.0.0.1:17493）。
// 位址在設定 → 語音 → 伺服器位址改得到，所以服務不一定跑在本機。
//
// 用 /generate/stream 而不是 /generate —— 後者是非同步排隊，要輪詢或接 SSE
// 才拿得到音訊；前者直接把 WAV 以 chunked 回應吐回來。
//
// 已知限制：Voicebox 的 GenerationRequest 沒有語速欄位，所以 rate 無效。

#include <QObject>

#include <functional>
#include <memory>
#include <string>

#include "core/config_schema.h"
#include "core/tts_types.h"
#include "tts_stream_util.h"

namespace l2m {

class HttpJson;

class VoiceboxEngine : public QObject, public TtsEngine {
  Q_OBJECT

public:
  VoiceboxEngine(HttpJson& http, std::function<VoiceboxConfig()> getConfig, std::function<std::string()> getUiLocale, QObject* parent = nullptr);

  const std::string& id() const override { return id_; }
  const std::string& name() const override { return name_; }

  void isAvailable(std::function<void(bool)> done) override;
  void listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) override;
  std::unique_ptr<TtsRequestHandle> synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) override;

private:
  std::string base() const;
  void generate(const HttpCallChainPtr& chain, const VoiceboxConfig& config, const std::string& profileId, const std::string& text, TtsStreamSink sink);

  HttpJson& http_;
  std::function<VoiceboxConfig()> getConfig_;
  std::function<std::string()> getUiLocale_;
  const std::string id_ = "voicebox";
  const std::string name_ = "Voicebox";
};

}  // namespace l2m
