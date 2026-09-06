#pragma once

// Windows 內建 SAPI 語音，離線可用，是整條 fallback 鏈的最後一道保險。
//
// 用 SetOutputToWaveFile 而不是直接 Speak：口型同步需要音訊資料，
// 讓 SAPI 自己發聲就什麼都拿不到。
//
// 合成是非同步的（QProcess 的 finished 訊號），語音清單仍然同步 ——
// 前者會被句段管線呼叫很多次，後者只在開設定頁時查一次而且永久快取。
//
// 文字一律經由暫存檔傳給 PowerShell，不放進命令列 ——
// 使用者或 AI 給的文字可能含引號、換行、$ 等字元，字串拼接遲早會炸。
//
// 只在 WIN32 編譯（tts_engine_sapi_win.cpp）。macOS 的 say 引擎屬於 M7。

#include <QObject>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/tts_types.h"

namespace l2m {

class SapiEngine : public QObject, public TtsEngine {
  Q_OBJECT

public:
  explicit SapiEngine(QObject* parent = nullptr);

  const std::string& id() const override { return id_; }
  const std::string& name() const override { return name_; }

  void isAvailable(std::function<void(bool)> done) override;
  void listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) override;
  // 非同步：PowerShell 跑完才回呼。cancel() 會直接殺掉那個行程。
  // 同步版凍住 GUI 一兩秒本來還能忍，但句段管線會讓一段長台詞變成好幾次呼叫，
  // 累積起來使用者會以為程式當掉。
  std::unique_ptr<TtsRequestHandle> synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) override;

  // 純函式，供測試：SAPI 的 Rate 是 -10 ~ 10，把 0.5 ~ 2 的倍率線性映射過去
  static int sapiRate(double rate);
  // 純函式，供測試：解析 list-voices 腳本的 JSON 輸出
  static std::vector<VoiceInfo> parseVoiceList(const std::string& json, const std::string& engineId);

private:
  const std::string id_ = "sapi";
  const std::string name_ = "Windows Built-in Voices (offline)";

  // 語音清單很穩定，查一次就夠（TtsManager 的 resetCache 不會清它，
  // 使用者裝新語音的頻率遠低於「重新偵測」按鈕被按的頻率）
  std::optional<std::vector<VoiceInfo>> voicesCache_;
};

}  // namespace l2m
