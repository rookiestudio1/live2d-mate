#pragma once

// HTTP 系 LLM 引擎：OpenAI-compatible 與 Anthropic 兩家協定共用這一個實作 ——
// 請求組裝（core/llm_http.h）與串流事件解析（core/llm_stream_accumulator.h）
// 都已依 provider 分好，這一層只剩「發請求、把位元組餵給解析器、
// 把增量轉發進 sink」的搬運，兩家共用一份搬運碼比兩個類別各抄一份可靠。
//
// LlmEngine 抽象仍然留著：未來若加內嵌推論引擎（llama.cpp），
// LlmManager 依 provider 路由到另一個實作，這一側不用動。
//
// 紀律與 TTS 引擎相同（core/tts_types.h）：回呼全在 GUI 執行緒、
// cancel() 之後絕不再回呼、chat() 永遠回非空 handle。
// 設定用 std::function 每次現取（tts_engine_custom.h 的規則）：
// 使用者在設定頁一改，下一次請求就生效。

#include <functional>

#include "../media/http_json.h"
#include "core/config_schema.h"
#include "core/llm_types.h"

namespace l2m {

class HttpLlmEngine : public LlmEngine {
public:
  HttpLlmEngine(HttpJson& http, std::function<LlmConfig()> getConfig);

  const std::string& id() const override;

  // 永遠 true 且不打網路：計費端點不能拿來探活，而且「不可用」會讓引擎
  // 在設定頁選不了（tts_engine_custom.h 的三規則）。
  void isAvailable(std::function<void(bool)> done) override;

  void listModels(std::function<void(std::vector<std::string> models, std::string error)> done) override;

  std::unique_ptr<LlmRequestHandle> chat(const std::vector<LlmMessage>& messages, const LlmChatOptions& options, LlmStreamSink sink) override;

private:
  std::string id_ = "http";
  HttpJson& http_;
  std::function<LlmConfig()> getConfig_;
};

}  // namespace l2m
