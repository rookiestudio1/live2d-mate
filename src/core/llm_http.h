#pragma once

// LLM 兩家協定的請求組裝與回應解析。
//
// 與 core/tts_http.h 同一個設計：刻意只放純函式，「每個欄位有沒有送對」
// 變成可測的斷言（tests/test_llm_http.cpp），不必真的起一台 Ollama 或
// 對計費 API 發請求才驗得到。錯一個欄位的症狀是整條管線靜默失敗或
// 401/400，離現場很遠。
//
// OpenAI-compatible（POST {baseUrl}/chat/completions）一個協定同時覆蓋
// local（Ollama / LM Studio / llama.cpp server）與多數雲端 —— 差別只是
// baseUrl 與 apiKey。Anthropic Messages API（POST /v1/messages）的 system
// 是獨立欄位、工具與 SSE 形狀都不同，硬塞 adapter 比第二組函式髒，
// 所以各組各的。

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "config_schema.h"
#include "llm_types.h"

namespace l2m {

// Anthropic 的固定端點與版本（不開放自訂：要自訂端點的服務一律走 openai 相容協定）
inline constexpr const char* kAnthropicMessagesUrl = "https://api.anthropic.com/v1/messages";
inline constexpr const char* kAnthropicModelsUrl = "https://api.anthropic.com/v1/models";
inline constexpr const char* kAnthropicVersion = "2023-06-01";

// 真正要送出去的東西。error 非空代表設定不完整，呼叫端不該發請求。
// 面向使用者／AI 的英文訊息，一定附「怎麼修」；設定頁的紅字提示與
// 呼叫失敗的錯誤是**同一句**，只有一個真相來源（tts_http.h 的既定慣例）。
struct LlmHttpRequest {
  std::string url;
  std::string body;
  std::string contentType;  // 一律 application/json
  std::vector<std::pair<std::string, std::string>> headers;
  std::string error;
};

// 設定是否完整（依 provider 檢查對應欄位）。非空＝紅字／錯誤訊息。
// enabled 不在這裡管 —— 開關是呼叫端的事，這裡只看「填好了沒」。
std::string llmConfigIssue(const LlmConfig& config);

// OpenAI-compatible chat.completions。stream=true 時帶 "stream":true。
// options 的欄位蓋過 config（nullopt／空字串＝用 config 的值）。
LlmHttpRequest buildChatCompletionsRequest(const LlmConfig& config, const std::vector<LlmMessage>& messages, const LlmChatOptions& options, bool stream);

// Anthropic Messages。system 訊息抽成頂層 system 欄位；toolsJson（OpenAI 形狀）
// 轉成 input_schema 形狀；tool 結果訊息轉成 user 的 tool_result 區塊。
LlmHttpRequest buildAnthropicMessagesRequest(const LlmConfig& config, const std::vector<LlmMessage>& messages, const LlmChatOptions& options, bool stream);

// 依 config.provider 分派到上面兩個
LlmHttpRequest buildLlmChatRequest(const LlmConfig& config, const std::vector<LlmMessage>& messages, const LlmChatOptions& options, bool stream);

// 模型清單（GET /models；兩家的回應恰好同形：{"data":[{"id":…}]}）
LlmHttpRequest buildListModelsRequest(const LlmConfig& config);
std::vector<std::string> parseModelListJson(const std::string& json);

// 把失敗回應轉成看得懂的錯誤訊息（兩家的 {"error":{"message":…}} 都撈得出來，
// 純文字 body 截短附上）
std::string describeLlmErrorResponse(int status, const std::string& body);

// 這個失敗是暫時性的嗎（值得稍後重試）？
// 5xx（Ollama 模型冷載入中會回 500 "llm server loading model"）、429 與
// 連線層失敗算；4xx（金鑰錯、模型名錯 —— 重試一百次也一樣）不算。
// 吃 describeLlmErrorResponse／引擎組出來的錯誤字串，行為大腦靠它決定
// 「15 秒後再試一次」還是「直接退回規則版」。
bool llmErrorLooksTransient(const std::string& error);

}  // namespace l2m
