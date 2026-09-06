#pragma once

// LLM 引擎介面與共用型別。
//
// 形狀刻意逐項模仿 core/tts_types.h 的 TtsEngine 全套 —— 那一套的紀律
// （回呼一律在 GUI 執行緒、嚴格的順序保證、cancel 之後絕不再回呼、
// handle 永遠不是 nullptr）已經被五個 TTS 引擎驗證過，LLM 的請求生命週期
// 與 TTS 合成同構：發出 → 一連串增量回呼 → 結束或失敗，可取消。
//
// 引擎只交出 token 與工具呼叫，**絕不自己執行任何動作** —— 拿到工具呼叫
// 要做什麼是呼叫端（行為大腦、未來的聊天迴圈）的決定，與「TTS 引擎只交出
// 音訊位元組，絕不自己播放」是同一條產品規則。

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace l2m {

// 一次工具呼叫（OpenAI 的 tool_calls / Anthropic 的 tool_use）。
// argumentsJson 是完整的 JSON 物件文字 —— SSE 裡它是跨多個 delta 的碎片，
// 由引擎內部的累積器（core/llm_stream_accumulator.h）拼完整才交出來，
// 呼叫端永遠不必懂 delta 格式。
struct LlmToolCall {
  std::string id;
  std::string name;
  std::string argumentsJson;
};

// 對話裡的一則訊息。role 沿用 OpenAI 的字彙（system | user | assistant | tool），
// Anthropic 引擎在組請求時自行轉換（system 抽出、tool 轉 tool_result）。
struct LlmMessage {
  std::string role;
  std::string text;
  // role == "tool" 時：這是哪一次工具呼叫的結果
  std::string toolCallId;
  // role == "assistant" 時：這一輪發出的工具呼叫（第一期還用不到，
  // 聊天迴圈階段才會把上一輪的 assistant 訊息帶回去）
  std::vector<LlmToolCall> toolCalls;
};

// 單次請求的覆寫。nullopt / 空字串代表用 LlmConfig 的值。
struct LlmChatOptions {
  std::optional<double> temperature;
  std::optional<int> maxTokens;
  // OpenAI tools 陣列的 JSON 原文；空字串＝這一輪不帶工具。
  // Anthropic 引擎組請求時轉成它的 tools 形狀（input_schema）。
  std::string toolsJson;
};

// 一次請求的控制代碼。呼叫 cancel() 之後**保證不會再有任何 sink 回呼**，
// 重複呼叫是安全的。與 TtsRequestHandle 分開定義而不是共用：兩邊的語意
// 相同純屬巧合，綁在一起會讓其中一邊將來想加方法時動到另一邊。
class LlmRequestHandle {
public:
  virtual ~LlmRequestHandle() = default;
  virtual void cancel() = 0;
};

// FunctionTtsHandle 的翻版：共享旗標讓還在飛的回呼自己閉嘴，再跑中止動作。
// abort 的 lambda 要用 QPointer 護住 QObject —— 取消時它可能已經走了。
class FunctionLlmHandle : public LlmRequestHandle {
public:
  explicit FunctionLlmHandle(std::shared_ptr<bool> cancelled, std::function<void()> abort = {}) : cancelled_(std::move(cancelled)), abort_(std::move(abort)) {}

  void cancel() override {
    if (!cancelled_ || *cancelled_) return;
    *cancelled_ = true;
    if (abort_) abort_();
  }

private:
  std::shared_ptr<bool> cancelled_;
  std::function<void()> abort_;
};

// 串流回覆的回呼組。全部在 GUI 執行緒觸發，順序保證是：
//   onOpen? → (onTextDelta | onToolCall)* → (onDone | onError)
// onDone / onError 之後絕不再有任何回呼，cancel() 之後也是。
struct LlmStreamSink {
  // 知道實際回覆的模型了（伺服器回報的名稱）。可能不被呼叫。
  std::function<void(std::string model)> onOpen;
  // 一段文字增量。累積起來就是完整回覆。
  std::function<void(const std::string& delta)> onTextDelta;
  // 一次**完整的**工具呼叫（引擎已把碎片拼好）
  std::function<void(LlmToolCall call)> onToolCall;
  // finishReason 沿用 OpenAI 字彙（stop | length | tool_calls…）；拿不到時為空
  std::function<void(std::string finishReason)> onDone;
  std::function<void(std::string error)> onError;
};

class LlmEngine {
public:
  virtual ~LlmEngine() = default;

  virtual const std::string& id() const = 0;

  // custom TTS 引擎的三規則同樣適用（tts_engine_custom.h）：
  // 永遠回 true 且不打網路 —— 拿計費端點探活等於每次扣錢，而且「不可用」
  // 會讓引擎在設定頁選不了，使用者也就沒辦法試自己剛填的設定。
  // 設定不完整的回報一律留到 chat()。
  virtual void isAvailable(std::function<void(bool)> done) = 0;

  // 模型清單（GET /models）。失敗時 error 非空、清單為空 —— 設定頁的
  // 「測試連線」按鈕靠它一石二鳥：既驗連線又把清單填進下拉。
  virtual void listModels(std::function<void(std::vector<std::string> models, std::string error)> done) = 0;

  // 發出一輪對話。回傳的 handle 永遠不是 nullptr —— 即使已經同步失敗
  //（sink.onError 在 chat() 返回前就呼叫過了），也要回一個 cancel() 是
  // no-op 的 handle，呼叫端才不必到處判空。
  virtual std::unique_ptr<LlmRequestHandle> chat(const std::vector<LlmMessage>& messages, const LlmChatOptions& options, LlmStreamSink sink) = 0;
};

}  // namespace l2m
