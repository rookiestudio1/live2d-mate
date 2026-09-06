// LLM 串流累積器：OpenAI 的 tool-call 碎片重組與 [DONE] 哨兵、
// Anthropic 的事件形狀與 stop_reason 對映。
#include <QtTest>

#include <string>
#include <vector>

#include "core/llm_stream_accumulator.h"

using namespace l2m;

namespace {

SseEvent dataEvent(const std::string& data) { return {"", data}; }

}  // namespace

class TestLlmStreamAccumulator : public QObject {
  Q_OBJECT

private slots:
  // ── OpenAI ──

  void openaiTextDeltas() {
    OpenAiStreamAccumulator acc;
    auto updates = acc.feed(dataEvent(R"({"model":"llama3","choices":[{"delta":{"role":"assistant","content":"Hel"},"finish_reason":null}]})"));
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].model, std::string("llama3"));
    QCOMPARE(updates[0].textDelta, std::string("Hel"));

    updates = acc.feed(dataEvent(R"({"choices":[{"delta":{"content":"lo"},"finish_reason":null}]})"));
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].textDelta, std::string("lo"));
  }

  void openaiFinishReasonAndDone() {
    OpenAiStreamAccumulator acc;
    auto updates = acc.feed(dataEvent(R"({"choices":[{"delta":{},"finish_reason":"stop"}]})"));
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].finishReason, std::string("stop"));
    QCOMPARE(updates[0].done, false);

    updates = acc.feed(dataEvent("[DONE]"));
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].done, true);
  }

  // arguments 是跨多個 delta 的碎片 JSON，靠 index 對齊、逐段串接，
  // finish_reason 時才吐出完整呼叫。
  // 這幾條刻意用傳統字串常值：moc 的 lexer 解不動 raw string 裡的 \"，
  // 整個測試類別會被它靜默吃掉（症狀是 .moc 產出 0 位元組、連結時
  // metaObject 全數 unresolved）。
  void openaiToolCallFragments() {
    OpenAiStreamAccumulator acc;
    acc.feed(
      dataEvent("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call_1\","
                "\"function\":{\"name\":\"speak\",\"arguments\":\"{\\\"te\"}}]},"
                "\"finish_reason\":null}]}"));
    acc.feed(
      dataEvent("{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,"
                "\"function\":{\"arguments\":\"xt\\\":\\\"hi\\\"}\"}}]},"
                "\"finish_reason\":null}]}"));
    const auto updates = acc.feed(dataEvent(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"));
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].finishReason, std::string("tool_calls"));
    QCOMPARE(updates[0].toolCalls.size(), size_t(1));
    QCOMPARE(updates[0].toolCalls[0].id, std::string("call_1"));
    QCOMPARE(updates[0].toolCalls[0].name, std::string("speak"));
    QCOMPARE(updates[0].toolCalls[0].argumentsJson, std::string(R"({"text":"hi"})"));
  }

  // 兩個並行工具呼叫各自靠 index 累積，不互相污染
  void openaiParallelToolCalls() {
    OpenAiStreamAccumulator acc;
    acc.feed(dataEvent(
      R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"a","function":{"name":"f1","arguments":"{}"}},{"index":1,"id":"b","function":{"name":"f2","arguments":"{}"}}]},"finish_reason":null}]})"));
    const auto updates = acc.feed(dataEvent("[DONE]"));
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].toolCalls.size(), size_t(2));
    QCOMPARE(updates[0].toolCalls[0].name, std::string("f1"));
    QCOMPARE(updates[0].toolCalls[1].name, std::string("f2"));
  }

  // 不送 finish_reason 只送 [DONE] 的相容服務：工具呼叫在 [DONE] 補 flush，
  // 而且只 flush 一次
  void openaiToolsFlushOnlyOnce() {
    OpenAiStreamAccumulator acc;
    acc.feed(dataEvent(R"({"choices":[{"delta":{"tool_calls":[{"index":0,"id":"a","function":{"name":"f","arguments":"{}"}}]},"finish_reason":null}]})"));
    auto updates = acc.feed(dataEvent(R"({"choices":[{"delta":{},"finish_reason":"tool_calls"}]})"));
    QCOMPARE(updates[0].toolCalls.size(), size_t(1));
    updates = acc.feed(dataEvent("[DONE]"));
    QCOMPARE(updates[0].toolCalls.size(), size_t(0));
    QCOMPARE(updates[0].done, true);
  }

  void openaiStreamError() {
    OpenAiStreamAccumulator acc;
    const auto updates = acc.feed(dataEvent(R"({"error":{"message":"insufficient quota","type":"quota"}})"));
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].error, std::string("insufficient quota"));
  }

  // 壞掉的 chunk 直接略過，不讓整條串流失敗
  void openaiIgnoresGarbageChunk() {
    OpenAiStreamAccumulator acc;
    QCOMPARE(acc.feed(dataEvent("not json at all")).size(), size_t(0));
  }

  // ── Anthropic ──

  void anthropicTextFlow() {
    AnthropicStreamAccumulator acc;
    auto updates = acc.feed({"message_start", R"({"type":"message_start","message":{"model":"claude-opus-5"}})"});
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].model, std::string("claude-opus-5"));

    QCOMPARE(acc.feed({"ping", R"({"type":"ping"})"}).size(), size_t(0));

    updates = acc.feed({"content_block_delta", R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"Hi"}})"});
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].textDelta, std::string("Hi"));

    // end_turn → stop（對映到 OpenAI 字彙）
    updates = acc.feed({"message_delta", R"({"type":"message_delta","delta":{"stop_reason":"end_turn"}})"});
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].finishReason, std::string("stop"));

    updates = acc.feed({"message_stop", R"({"type":"message_stop"})"});
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].done, true);
  }

  // tool_use：block_start 拿 id/name、input_json_delta 串碎片、block_stop 吐完整呼叫。
  // partial_json 兩條用傳統字串常值，理由同上（moc 與 raw string 裡的 \"）。
  void anthropicToolUse() {
    AnthropicStreamAccumulator acc;
    acc.feed({"content_block_start", R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_1","name":"speak"}})"});
    acc.feed({"content_block_delta",
              "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{"
              "\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"text\\\":\"}}"});
    acc.feed({"content_block_delta",
              "{\"type\":\"content_block_delta\",\"index\":1,\"delta\":{"
              "\"type\":\"input_json_delta\",\"partial_json\":\"\\\"hi\\\"}\"}}"});
    const auto updates = acc.feed({"content_block_stop", R"({"type":"content_block_stop","index":1})"});
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].toolCalls.size(), size_t(1));
    QCOMPARE(updates[0].toolCalls[0].id, std::string("toolu_1"));
    QCOMPARE(updates[0].toolCalls[0].name, std::string("speak"));
    QCOMPARE(updates[0].toolCalls[0].argumentsJson, std::string(R"({"text":"hi"})"));
  }

  // 空參數的工具呼叫一個 input_json_delta 都不來：補上空物件
  void anthropicEmptyToolInput() {
    AnthropicStreamAccumulator acc;
    acc.feed({"content_block_start", R"({"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"t","name":"reset"}})"});
    const auto updates = acc.feed({"content_block_stop", R"({"type":"content_block_stop","index":0})"});
    QCOMPARE(updates[0].toolCalls[0].argumentsJson, std::string("{}"));
  }

  void anthropicMaxTokensMapsToLength() {
    AnthropicStreamAccumulator acc;
    const auto updates = acc.feed({"message_delta", R"({"type":"message_delta","delta":{"stop_reason":"max_tokens"}})"});
    QCOMPARE(updates[0].finishReason, std::string("length"));
  }

  void anthropicErrorEvent() {
    AnthropicStreamAccumulator acc;
    const auto updates = acc.feed({"error", R"({"type":"error","error":{"type":"overloaded_error","message":"Overloaded"}})"});
    QCOMPARE(updates.size(), size_t(1));
    QCOMPARE(updates[0].error, std::string("Overloaded"));
  }
};

QTEST_GUILESS_MAIN(TestLlmStreamAccumulator)
#include "test_llm_stream_accumulator.moc"
