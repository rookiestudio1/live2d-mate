#pragma once

// LLM 的對外門面：依 config.llm.provider 路由到引擎、提供「整包收完」的
// 便利包裝、替 get_state 產出 llm 區塊。
//
// 與 TtsManager 的差異是刻意的：TTS 的引擎陣列順序＝fallback 順序，因為
// 「有聲音」比「哪個引擎的聲音」重要；LLM 不做引擎間 fallback ——
// 聊天回覆換一家重講會人格分裂，行為大腦的 fallback 又是「退回 IdleDirector」
// 這一層的事（src/llm/llm_behavior_planner.h），不是換一家再試。
// 目前只有一個 HTTP 引擎（兩家協定共用實作）；未來加內嵌推論引擎時
// 在這裡按 provider 分派。

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/config_schema.h"
#include "core/llm_types.h"
#include "llm_engine_http.h"

namespace l2m {

class LlmManager {
public:
  LlmManager(HttpJson& http, std::function<LlmConfig()> getConfig);

  // 設定齊全可以發請求了嗎（enabled 且 llmConfigIssue 為空）
  bool ready() const;

  // 串流對話（引擎的 chat 直通）。lastError 會在失敗時記下來給 stateJson。
  std::unique_ptr<LlmRequestHandle> chat(const std::vector<LlmMessage>& messages, const LlmChatOptions& options, LlmStreamSink sink);

  // 整包收完的便利包裝（行為大腦用：PerformStep JSON 要收完整才能解析）。
  // done 恰好被呼叫一次：成功時 error 為空。
  std::unique_ptr<LlmRequestHandle> chatBuffered(const std::vector<LlmMessage>& messages, const LlmChatOptions& options, std::function<void(std::string text, std::string error)> done);

  void listModels(std::function<void(std::vector<std::string> models, std::string error)> done);

  // get_state 的 llm 區塊（AppController::llmStateJson 注入用）
  std::string stateJson() const;

private:
  std::function<LlmConfig()> getConfig_;
  HttpLlmEngine engine_;
  std::string lastError_;  // 最近一次請求失敗的原因，純診斷（get_state 帶出）
};

}  // namespace l2m
