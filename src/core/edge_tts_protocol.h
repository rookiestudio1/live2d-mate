#pragma once

// Edge 線上語音的協定細節（純函式，不碰網路）。
// 協定從 msedge-tts@2.0.7 原始碼（dist/MsEdgeTTS.js）逆推而來。
//
// 這一整組常數都會隨微軟升 Edge 而漂移，所以集中放一處：
// 握手被回 403 幾乎都是 kSecMsGecVersion 過期，改這裡一行就好。
//
// 抽到 l2m_core 的理由是可測性：簽章、SSML 模板、音訊訊框切割都是純算，
// 對應 tests/test_edge_ssml.cpp；真正的 wss 連線在 l2m_media 那側。

#include <cstdint>
#include <string>
#include <vector>

#include "tts_types.h"

namespace l2m::edge {

// ── 易漂移常數（升級時只改這一區）──
inline constexpr const char* kTrustedClientToken = "6A5AA1D4EAFF4E9FB37E23D68491D6F4";
inline constexpr const char* kWssUrl = "wss://speech.platform.bing.com/consumer/speech/synthesize/readaloud/edge/v1";
inline constexpr const char* kVoicesUrl =
  "https://speech.platform.bing.com/consumer/speech/synthesize/readaloud/voices/"
  "list?trustedclienttoken=6A5AA1D4EAFF4E9FB37E23D68491D6F4";
inline constexpr const char* kSecMsGecVersion = "1-143.0.3650.96";
inline constexpr const char* kUserAgent =
  "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
  "Chrome/143.0.0.0 Safari/537.36 Edg/143.0.0.0";
inline constexpr const char* kOrigin = "chrome-extension://jdiccldimpdaibmpdkjnbmckianbfold";
inline constexpr const char* kOutputFormat = "audio-24khz-48kbitrate-mono-mp3";

// 訊框分隔：標頭與內容之間永遠是 CRLF CRLF
inline constexpr const char* kHeaderDelimiter = "\r\n\r\n";
// 二進位訊框裡音訊資料的起點標記
inline constexpr const char* kAudioDelimiter = "Path:audio\r\n";

// 沒有指定語音時，依序尋找第一個符合的地區
const std::vector<std::string>& preferredLocales();

// Sec-MS-GEC 簽章：每 5 分鐘換一個值。
//   ticks        = unix 秒 + 11644473600   （Unix epoch → Windows FILETIME epoch）
//   rounded      = ticks - ticks % 300     （對齊到 300 秒）
//   windowsTicks = rounded * 10^7          （100ns 單位）
//   gec          = 大寫十六進位(SHA256(windowsTicks 的十進位字串 + trustedClientToken))
std::string secMsGec(int64_t unixSeconds, const std::string& trustedClientToken);

// 合成連線的完整 URL
std::string buildSynthUrl(const std::string& gec, const std::string& connectionId);

// 連線後要立刻送出的第一則文字訊框（輸出格式協商）
std::string buildSpeechConfig(const std::string& outputFormat);

// SSML 本體。rate 用相對百分比：1 → +0%，1.5 → +50%
std::string buildSsml(const std::string& voice, const std::string& locale, const std::string& text, int ratePercent);

// 帶上 X-RequestId 與 Path 標頭的合成請求訊框
std::string buildSsmlRequest(const std::string& requestId, const std::string& ssml);

// 從語音名稱推地區（zh-TW-HsiaoChenNeural → zh-TW）；推不出來回空字串
std::string localeFromVoiceName(const std::string& voice);

// 二進位訊框裡音訊資料的起始位移；找不到 "Path:audio\r\n" 回 npos
size_t findAudioPayload(const char* data, size_t size);

// 文字訊框的 Path 標頭值（turn.start / turn.end / response / audio.metadata）
std::string framePath(const std::string& frame);

// 語音清單 JSON → VoiceInfo，並依 preferredLocales 排序
std::vector<VoiceInfo> parseVoiceList(const std::string& json, const std::string& engineId);

}  // namespace l2m::edge
