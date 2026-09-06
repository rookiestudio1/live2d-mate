#pragma once

// GPT-SoVITS 與 Voicebox 兩個「本機 HTTP 服務」引擎共用的純函式。
//
// 刻意只放純函式而不做抽象基底類別 —— 只有兩個實作，繼承只會讓兩邊的
// 參數差異藏在模板方法裡；純函式反而好測（tests/test_tts_http.cpp）。
//
// 請求主體的組裝也在這裡，是為了讓「14 個欄位有沒有送對」變成可測的斷言，
// 而不是要真的起一台 GPT-SoVITS 才驗得到。

#include <string>
#include <utility>
#include <vector>

#include "config_schema.h"
#include "tts_types.h"

namespace l2m {

// 探測服務活著只要一次往返，別讓系統匣為了一個沒開的服務卡住
inline constexpr int kProbeTimeoutMs = 2000;

// 去掉尾端斜線與前後空白，避免組出 http://host//tts 這種網址
std::string normalizeBaseUrl(const std::string& url);

// 'zh-TW' → 'zh'。後端要的是兩碼語言碼，不是完整地區字串。
std::string localeToLangCode(const std::string& locale);

// 'ja' → 'ja-JP'。已含連字號或不認識的語言原樣回傳，寧可多顯示也不要弄丟。
std::string langCodeToLocale(const std::string& code);

// 把失敗回應轉成看得懂的錯誤訊息。
// 後端可能回 JSON（GPT-SoVITS 的 {"message": …}）也可能回純文字，兩種都要撈得出來。
std::string describeErrorResponse(int status, const std::string& body);

// GPT-SoVITS 的 mediaType → mime
std::string gptSovitsMime(const std::string& mediaType);

// GPT-SoVITS /tts 的請求主體
std::string buildGptSovitsBody(const GptSovitsConfig& config, const GptSovitsPreset& preset, const std::string& text, double rate);

// GPT-SoVITS 切權重的 URL（weights_path 要做 URL 編碼）
std::string gptSovitsWeightUrl(const std::string& base, const std::string& path, const std::string& weights);

// Voicebox /generate/stream 的請求主體。
// engine / modelSize 留空時整個欄位不送，讓 profile 自己的預設生效。
std::string buildVoiceboxBody(const VoiceboxConfig& config, const std::string& profileId, const std::string& text, const std::string& language);

// Voicebox /profiles 的回應（裸陣列）
std::vector<VoiceInfo> parseVoiceboxProfiles(const std::string& json, const std::string& engineId);

// GPT-SoVITS 的 presets 直接就是語音清單，不必打網路
std::vector<VoiceInfo> gptSovitsVoices(const GptSovitsConfig& config, const std::string& engineId);

// ── 自訂 HTTP 語音端點 ─────────────────────────────────────
//
// 「使用者填什麼、我們就送什麼」的組裝規則。端點長什麼樣我們一無所知，
// 錯一個位元組就是整句唸不出來，所以每一條編碼規則都在
// tests/test_tts_http.cpp 裡釘死，而不是靠真的架一台服務去試。

enum class CustomTtsMethod { Get, PostForm, PostJson };

// config 的字串 → 列舉。未知值一律當 Get（schema 的 readEnum 已經擋過，這裡只是不要炸）。
CustomTtsMethod customTtsMethod(const std::string& value);

// ${TEXT} 的替換編碼。URL 一律 Percent，參數則依方法而定。
enum class TemplateEncoding { Percent, JsonString };

// 替換樣板裡的**每一處** ${TEXT}。
// JsonString 模式**不含前後引號** —— 樣板自己寫 "${TEXT}"，才同時支援
// {"text":"${TEXT}"} 與 {"segments":["${TEXT}"]} 這種形狀。
std::string expandTextPlaceholder(const std::string& tpl, const std::string& text, TemplateEncoding encoding);

// 自訂標頭：一行一個 "Name: Value"。
// 空行與 # 開頭略過；第一個冒號之後全部算值（Bearer 字串裡可能還有冒號）；
// 沒有冒號的行直接忽略而不是報錯 —— 使用者打到一半就被防抖存檔是常態。
std::vector<std::pair<std::string, std::string>> parseHeaderLines(const std::string& text);

// 真正要送出去的東西。error 非空代表設定不完整，呼叫端不該發請求。
struct CustomTtsRequest {
  std::string url;          // Get 時 params 已接成 query string
  std::string body;         // Get 時為空
  std::string contentType;  // Get 時為空
  std::vector<std::pair<std::string, std::string>> headers;
  // 面向使用者／AI 的英文訊息，一定附「怎麼修」（core/command_result.h 的產品規則）。
  // 設定頁的紅字提示與合成失敗的錯誤是**同一句**，只有一個真相來源。
  std::string error;
};
CustomTtsRequest buildCustomTtsRequest(const CustomTtsConfig& config, const std::string& text);

// 回應能不能播？回傳非空字串代表「別播，直接報這個錯」。
// miniaudio 只解得了 wav / mp3 / flac，其餘丟進去只會靜默失敗成「有氣泡沒聲音」，
// 使用者完全查不出原因（既有前例：tts_engine_gptsovits.cpp 也是這樣擋 JSON 回應的）。
std::string customTtsAudioError(const std::string& contentType, size_t bodySize);

}  // namespace l2m
