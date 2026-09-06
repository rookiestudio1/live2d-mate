#pragma once

// 設定 patch 的 JSON 組裝。
//
// 為什麼要獨立成一個模組：`ConfigStore::patch()` 吃的是「兩層 JSON 物件字串」
// （`{"model":{"scale":1.5}}`），呼叫端過去都在就地拼字串，踩到兩個坑：
//
//  1. **沒有逃逸**。MCP 設定的 host 與 token 是使用者可以自由輸入的欄位，
//     舊的 `"host":"` + host + `"` 遇到一個雙引號就產生無效 JSON，
//     `patch()` 丟出的例外訊息跟使用者實際做的事完全對不上。
//  2. **`std::to_string(double)` 印成 `1.000000`**，和 `config_schema.cpp`
//     的 `putNum`（整數值不印小數點，設定檔才好讀）不一致。
//
// 抽到 l2m_core 之後這兩條規則就測得到（tests/test_config_patch.cpp）。
// 面向使用者的字串不在這裡，這個模組只產生 JSON。

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config_schema.h"

namespace l2m {

// ── JSON scalar 編碼 ────────────────────────────────────────

// 只逃逸不加引號。自訂語音的 JSON 參數樣板要把 ${TEXT} 換成使用者的句子，
// 需要的正是這一半（樣板自己已經寫了 "${TEXT}" 的引號），所以從 jsonString 拆出來共用。
std::string jsonEscape(const std::string& value);
// 字串值：含前後雙引號，並逃逸 " \ 與控制字元。UTF-8 位元組原樣保留。
std::string jsonString(const std::string& value);
// 整數值不印小數點，與 config_schema.cpp 的 putNum 一致
std::string jsonNumber(double value);
std::string jsonBool(bool value);

// ── 一層 JSON 物件 ────────────────────────────────────────

// {"<field>":<value>, …}，保持插入順序。
// ConfigPatch 的內層與巢狀欄位（tts.custom）共用同一份編碼規則，所以抽出來。
class JsonObject {
public:
  JsonObject& boolean(const std::string& field, bool value);
  JsonObject& number(const std::string& field, double value);
  JsonObject& text(const std::string& field, const std::string& value);
  JsonObject& nullValue(const std::string& field);
  JsonObject& nullableText(const std::string& field, const std::optional<std::string>& value);
  JsonObject& object(const std::string& field, const JsonObject& value);
  // 陣列欄位。目前只有 tts.gptsovits.presets 用得到 —— 內層整包送的時候
  // 少了它就等於把使用者手寫的音色預設整組刪掉（見 ttsLocalServerPatch）。
  JsonObject& array(const std::string& field, const std::vector<JsonObject>& items);

  std::string json() const;

private:
  // field → 已編碼的 JSON 值
  std::vector<std::pair<std::string, std::string>> fields_;
};

// ── 兩層 patch 物件 ────────────────────────────────────────

// {"<section>":{"<field>":<value>, …}}。
// 欄位名一律用不同的函式名而不是多載，`set("x", "abc")` 會悄悄選到 bool 多載。
class ConfigPatch {
public:
  explicit ConfigPatch(std::string section) : section_(std::move(section)) {}

  ConfigPatch& boolean(const std::string& field, bool value);
  ConfigPatch& number(const std::string& field, double value);
  ConfigPatch& text(const std::string& field, const std::string& value);
  ConfigPatch& nullValue(const std::string& field);
  // nullopt 寫成 JSON null（例如 tts.voice、mcp.token）
  ConfigPatch& nullableText(const std::string& field, const std::optional<std::string>& value);
  // 巢狀物件欄位。ConfigStore::patch() 的內層是**整個覆蓋**，見 ttsCustomPatch 的說明。
  ConfigPatch& object(const std::string& field, const JsonObject& value);

  std::string json() const;

private:
  std::string section_;
  JsonObject fields_;
};

// ── 單欄位捷徑 ─────────────────────────────────────────────

std::string boolPatch(const std::string& section, const std::string& field, bool value);
std::string numberPatch(const std::string& section, const std::string& field, double value);
std::string stringPatch(const std::string& section, const std::string& field, const std::string& value);
std::string nullPatch(const std::string& section, const std::string& field);

// ── 有規則的組合 ───────────────────────────────────────────

// 換 TTS 引擎一定要同時把 voice 清成 null：語音 id 是引擎專屬的，
// 留著舊 id 會讓新引擎合成失敗或退回預設，使用者只看到「怎麼沒聲音」。
std::string ttsEnginePatch(const std::string& engineId);

// tts.custom 一定要**整包**送。ConfigStore::patch() 的合併只有兩層，內層是
// `yyjson_val_mut_copy` 整個覆蓋 —— 少送的欄位不會保留原值，而是被 parseConfig
// 補回預設值。這個函式吃整個結構就是為了讓呼叫端不可能只送一半。
std::string ttsCustomPatch(const CustomTtsConfig& config);

// 兩份自訂端點設定的「使用者可見欄位」是否相同。
// 比較的欄位與 ttsCustomPatch 送出的是**同一組**，只少了 timeoutMs（沒有 UI，
// 使用者改不到）。設定頁的「套用」按鈕就是靠這個決定亮不亮 —— 少比一個欄位
// 就會變成「明明改了卻按不下去」，所以刻意與 ttsCustomPatch 放在一起，
// 兩者必須同步異動。
bool customTtsFieldsEqual(const CustomTtsConfig& a, const CustomTtsConfig& b);

// GPT-SoVITS 與 Voicebox 的伺服器位址（設定 → 語音 → 伺服器位址）。
// 兩個位址共用一顆「套用」按鈕，所以組成同一份 tts patch 一次送出 ——
// 分兩包送會讓 ConfigStore 寫兩輪、發兩次 changed，中間那一輪還是半套設定。
//
// 內層一律**整包**送，理由與 ttsCustomPatch 相同，而這裡漏送的後果更嚴重：
// 少送 gptsovits.presets 就是把使用者手寫的音色預設整組刪掉（parseConfig 會把
// 沒送的欄位補回預設值，而 presets 的預設值是空陣列，症狀是「改個位址，
// 聲音全沒了」）。所以這個函式吃整個 TtsConfig，呼叫端不可能只送一半。
std::string ttsLocalServerPatch(const TtsConfig& config);

// 兩份設定的「有輸入框的欄位」（兩個 baseUrl）是否相同。
// 設定頁的「套用」按鈕靠這個決定亮不亮，所以與 ttsLocalServerPatch 放在一起，
// 兩者必須同步異動 —— 取捨同 customTtsFieldsEqual。
bool ttsLocalServerFieldsEqual(const TtsConfig& a, const TtsConfig& b);

// MCP 的四個欄位是一組。分開送會讓伺服器在中途以「新 port ＋ 舊 token」
// 之類的半套設定重啟一次。
std::string mcpPatch(bool enabled, const std::string& host, int port, const std::optional<std::string>& token);

// llm 的「套用」按鈕欄位一定要**整包**送 —— 理由與 ttsCustomPatch 相同：
// ConfigStore::patch() 的內層是整個覆蓋，少送的欄位會被 parseConfig 補回預設值。
// enabled / driveIdle / temperature 等即時套用的欄位也一併帶上（帶目前值），
// 呼叫端不可能只送一半。
std::string llmPatch(const LlmConfig& config);

// 兩份 LLM 設定的「套用按鈕管的欄位」是否相同（provider、兩組連線欄位）。
// 即時套用的欄位（enabled / driveIdle / temperature / maxTokens…）刻意不比 ——
// 它們改了就直接寫進 config，不經過套用按鈕。與 llmPatch 放在一起，同步異動。
bool llmFieldsEqual(const LlmConfig& a, const LlmConfig& b);

// 天氣的地點是三個欄位一組（座標兩個 ＋ 顯示名）。
// 分開送會讓 ConfigStore 在中途以「新座標配舊城市名」重跑一輪：設定頁閃一下
// 對不上的名字，WeatherService 還會用那半套設定真的去抓一次預報。
// 其餘欄位（enabled / unit / 門檻…）都是單一欄位即時套用，走 boolPatch 就好。
std::string weatherLocationPatch(double latitude, double longitude, const std::string& name);

}  // namespace l2m
