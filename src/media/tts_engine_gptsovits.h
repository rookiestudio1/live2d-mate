#pragma once

// GPT-SoVITS 服務（api_v2.py，預設 127.0.0.1:9880）。
// 位址在設定 → 語音 → 伺服器位址改得到，所以服務不一定跑在本機。
//
// 使用者自己安裝並啟動服務，這裡只當 HTTP 用戶端；不 spawn 子行程、
// 不打包 Python 與模型，所以「安裝時不需要 Visual Studio」仍然成立。
//
// 刻意的限制：mediaType 不是 wav 時直接在送出前擋掉。
// 內建播放器（miniaudio）解不了 ogg 與 aac，等 CPU 推論跑完一兩分鐘
// 才發現放不出來太慘，寧可當場給一句有 hint 的錯誤。

#include <QObject>

#include <functional>
#include <memory>
#include <string>

#include "core/config_schema.h"
#include "core/tts_types.h"
#include "tts_stream_util.h"

namespace l2m {

class HttpJson;

class GptSovitsEngine : public QObject, public TtsEngine {
  Q_OBJECT

public:
  GptSovitsEngine(HttpJson& http, std::function<GptSovitsConfig()> getConfig, QObject* parent = nullptr);

  const std::string& id() const override { return id_; }
  const std::string& name() const override { return name_; }

  void isAvailable(std::function<void(bool)> done) override;
  void listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) override;
  std::unique_ptr<TtsRequestHandle> synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) override;

private:
  std::string base() const;
  // 找不到指定的 preset 就退回第一個，總比整句話發不出來好
  const GptSovitsPreset* resolvePreset(const GptSovitsConfig& config, const std::optional<std::string>& voice) const;
  // 權重是全域狀態且切換要重載模型（很貴），同一組不重複切
  void applyWeights(const HttpCallChainPtr& chain, const GptSovitsConfig& config, const GptSovitsPreset& preset, std::function<void(std::string error)> done);
  void postTts(const HttpCallChainPtr& chain, const GptSovitsConfig& config, const GptSovitsPreset& preset, const std::string& text, double rate, TtsStreamSink sink);

  HttpJson& http_;
  std::function<GptSovitsConfig()> getConfig_;
  const std::string id_ = "gptsovits";
  const std::string name_ = "GPT-SoVITS";

  // 上次真的套進服務的權重路徑
  std::string appliedGptWeights_;
  std::string appliedSovitsWeights_;
};

}  // namespace l2m
