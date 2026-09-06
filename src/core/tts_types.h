#pragma once

// TTS 引擎介面與共用型別。
//
// 契約：**引擎一律回傳音訊位元組，絕不自己發聲**。
// 口型同步需要拿到波形自己算音量包絡，讓引擎直接播就什麼都拿不到了。
//
// 非同步一律是回呼式而不是 future/promise ——
// Qt 的網路與行程都是事件迴圈驅動，包成 future 反而要多開執行緒。
// 回呼一律在 GUI 執行緒觸發。
//
// 「整段」與「串流」共用同一個 synthesize()，差別只在 onChunk 被呼叫幾次、
// 以及 streams() 回什麼 —— 兩條各自維護的分支遲早會走鐘，所以刻意合成一條。

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace l2m {

struct VoiceInfo {
  // 引擎內部的語音識別字串
  std::string id;
  std::string name;
  // 帶連字號的地區字串（zh-TW、ja-JP…），系統匣靠它分組
  std::string locale;
  std::string gender;
  // 來源引擎 id，fallback 之後才知道實際用了誰
  std::string engine;
};

struct SpeakOptions {
  // nullopt 代表用引擎預設語音
  std::optional<std::string> voice;
  // 語速倍率，1 為正常（0.5 ~ 2）
  std::optional<double> rate;
};

struct TtsEngineInfo {
  std::string id;
  std::string name;
  bool available = false;
};

// 一次合成實際用到的引擎與語音（fallback 之後可能與請求不同）
struct SynthesisInfo {
  std::string engine;
  std::optional<std::string> voice;
  // 例如 audio/mpeg、audio/wav；只作診斷用，實際格式由解碼器嗅探
  std::string mime;
};

// 整段緩衝完成的結果，給不串流的呼叫端用（設定頁的「測試語音」之類）
struct SynthesizeOutcome {
  std::vector<char> audio;
  std::string mime;
  std::string engine;
  std::optional<std::string> voice;
};

// 一次合成的控制代碼。
//
// 舊介面沒有 cancel，所以 stopSpeaking() 只停得了播放器，進行中的 wss 訊框與
// HTTP body 還是會一路收完 —— 邊收邊播之後這變成必修：已經拆掉的播放器不該
// 再收到遲到的 chunk。
class TtsRequestHandle {
public:
  virtual ~TtsRequestHandle() = default;
  // 呼叫之後**保證不會再有任何 sink 回呼**。重複呼叫是安全的。
  virtual void cancel() = 0;
};

// 大多數引擎的取消長得一樣：立一個共享旗標讓還在飛的回呼自己閉嘴，
// 再跑一個中止動作（abort socket、kill 行程）。
// abort 的 lambda 要用 QPointer 護住 QObject —— 取消時它可能已經走了。
class FunctionTtsHandle : public TtsRequestHandle {
public:
  explicit FunctionTtsHandle(std::shared_ptr<bool> cancelled, std::function<void()> abort = {}) : cancelled_(std::move(cancelled)), abort_(std::move(abort)) {}

  void cancel() override {
    if (!cancelled_ || *cancelled_) return;
    *cancelled_ = true;
    if (abort_) abort_();
  }

private:
  std::shared_ptr<bool> cancelled_;
  std::function<void()> abort_;
};

// 串流合成的回呼組。全部在 GUI 執行緒觸發，順序保證是：
//   onOpen? → onChunk* → (onSegmentEnd → onChunk*)* → (onDone | onError)
// onDone / onError 之後絕不再有任何回呼，cancel() 之後也是。
struct TtsStreamSink {
  // 知道格式了（來自 Content-Type，或引擎自己協商的輸出格式）。可能不被呼叫。
  std::function<void(std::string mime)> onOpen;
  // 一塊編碼位元組。data 只在回呼期間有效，呼叫端要自己複製走。
  std::function<void(const char* data, size_t size)> onChunk;
  // 句段管線：這一段的位元組完了，下一段還會來（**不是**整句結束）
  std::function<void()> onSegmentEnd;
  std::function<void()> onDone;
  std::function<void(std::string error)> onError;
};

class TtsEngine {
public:
  virtual ~TtsEngine() = default;

  virtual const std::string& id() const = 0;
  virtual const std::string& name() const = 0;

  // 引擎是否可用（需要網路、需要本機服務、或只在某個平台上存在）
  virtual void isAvailable(std::function<void(bool)> done) = 0;

  // 取得語音清單；失敗時 error 非空、清單為空
  virtual void listVoices(std::function<void(std::vector<VoiceInfo> voices, std::string error)> done) = 0;

  // 這個引擎是不是「邊產生邊送」。false 代表 onChunk 只會在最後被呼叫一次
  //（整段），上層據此決定要不要幫它套一層文字切句的管線。
  virtual bool streams() const { return false; }

  // 合成。回傳的 handle 永遠不是 nullptr —— 即使已經同步失敗（sink.onError
  // 在 synthesize() 返回前就呼叫過了），也要回一個 cancel() 是 no-op 的 handle，
  // 呼叫端才不必到處判空。
  virtual std::unique_ptr<TtsRequestHandle> synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) = 0;
};

}  // namespace l2m
