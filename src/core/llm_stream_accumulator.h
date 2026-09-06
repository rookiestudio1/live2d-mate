#pragma once

// SSE 事件 → LLM 串流增量的累積器。
//
// 兩家的事件形狀完全不同，各一個累積器；輸出統一成 LlmStreamUpdate，
// 引擎那側（src/llm/）只負責把它轉發進 LlmStreamSink，不含任何解析邏輯 ——
// 這一層是整條串流鏈最容易錯的地方（工具呼叫的 arguments 是跨多個 delta 的
// 碎片 JSON），放 core 才測得到（tests/test_llm_stream_accumulator.cpp）。
//
// OpenAI（chat.completions chunk）：
//   data: {"choices":[{"delta":{"content":"…","tool_calls":[{"index":0,
//          "id":"…","function":{"name":"…","arguments":"碎片"}}]},
//          "finish_reason":null|"stop"|…}]}
//   data: [DONE]                      ← 應用層哨兵，結束
// 工具呼叫以 delta 裡的 index 對齊、arguments 逐段串接，
// 在 finish_reason（或 [DONE]）時才吐出完整的 LlmToolCall。
//
// Anthropic（Messages API stream）：
//   event: message_start / content_block_start / content_block_delta
//          (text_delta | input_json_delta) / content_block_stop /
//          message_delta (stop_reason) / message_stop / ping / error
// tool_use 區塊在 content_block_start 拿到 id 與 name、input_json_delta
// 串 arguments、content_block_stop 吐出完整呼叫。
// stop_reason 對映到 OpenAI 字彙（end_turn→stop、tool_use→tool_calls、
// max_tokens→length），呼叫端只需要認一套。

#include <string>
#include <vector>

#include "llm_types.h"
#include "sse_parser.h"

namespace l2m {

// 一次 feed 產出的增量。欄位彼此獨立：同一個更新可能同時帶文字與 done。
struct LlmStreamUpdate {
  std::string textDelta;               // 非空＝一段文字增量
  std::vector<LlmToolCall> toolCalls;  // 已拼完整的工具呼叫
  std::string finishReason;            // 非空＝拿到結束原因（OpenAI 字彙）
  std::string model;                   // 非空＝第一次知道實際模型名
  bool done = false;                   // 串流正常結束
  std::string error;                   // 非空＝伺服器在串流裡回報錯誤
};

class OpenAiStreamAccumulator {
public:
  std::vector<LlmStreamUpdate> feed(const SseEvent& event);

private:
  struct PendingTool {
    std::string id;
    std::string name;
    std::string arguments;
  };
  // 以 delta 的 index 為下標；缺項補空
  std::vector<PendingTool> tools_;
  bool toolsFlushed_ = false;

  void flushTools(LlmStreamUpdate& update);
};

class AnthropicStreamAccumulator {
public:
  std::vector<LlmStreamUpdate> feed(const SseEvent& event);

private:
  struct PendingTool {
    int index = -1;
    std::string id;
    std::string name;
    std::string arguments;
  };
  std::vector<PendingTool> tools_;
};

}  // namespace l2m
