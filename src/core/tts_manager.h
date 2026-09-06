#pragma once

// 管理所有 TTS 引擎：可用性偵測、語音清單、合成與 fallback。
//
// 引擎陣列的順序**就是** fallback 順序：線上品質優先，本機服務次之，
// 系統內建離線語音墊底。指定的引擎失敗時依序往下試，
// 但**不沿用原本的 voice** —— 那是別的引擎的識別字串。
//
// fallback 用回呼的連續傳遞（tryNext）串起來，不用 future/promise。
// 播放與口型同步不在這裡，這一層只負責產出音訊 buffer。

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tts_types.h"

namespace l2m {

// 「不可用」偵測結果的信任期限。啟動時網路還沒好、線上服務短暫失聯都會
// 探成不可用，永久快取會讓引擎再也回不來（每次說話都直接 fallback 到
// 墊底引擎，只能手動「重新偵測」）；「可用」則沿用永久快取的既有語意
inline constexpr int64_t kUnavailableRetryMs = 30'000;

class TtsManager {
public:
  // nowMs 供測試注入假時鐘；預設用單調時鐘（不受系統時間調整影響）
  explicit TtsManager(std::vector<std::unique_ptr<TtsEngine>> engines, std::function<int64_t()> nowMs = {});
  ~TtsManager();

  // 第一個引擎即預設引擎
  std::string defaultEngineId() const;

  TtsEngine* find(const std::optional<std::string>& id) const;

  // 清掉可用性快取（例如網路恢復後、或使用者按「重新偵測語音」）
  void resetCache() { availability_.clear(); }
  // 單一引擎失敗時只清它自己，下次重新偵測
  void forgetEngine(const std::string& id) { availability_.erase(id); }

  void listEngines(std::function<void(std::vector<TtsEngineInfo>)> done);

  // engineId 為 nullopt 代表彙整所有可用引擎的語音
  void listVoices(const std::optional<std::string>& engineId, std::function<void(std::vector<VoiceInfo>)> done);

  struct SynthesizeRequest {
    std::optional<std::string> engine;
    std::optional<std::string> voice;
    std::optional<double> rate;
  };

  // 串流合成的接收端。
  struct StreamSink {
    // 第一塊音訊要交出去了。**回傳值就是承諾點**：
    //   true  = 這一塊會真的出聲，之後任何失敗都不再 fallback
    //   false = 呼叫端只是在緩衝（例如下面的 synthesize()），還可以換引擎重試
    // 承諾點存在的理由：串流一旦吐出第一塊給播放器，換下一家就會把已經唸出去
    // 的前半句再唸一次。
    std::function<bool(const std::string& mime)> onOpen;
    std::function<void(const char* data, size_t size)> onChunk;
    std::function<void()> onSegmentEnd;
    std::function<void(SynthesisInfo info)> onDone;
    // committed == true 代表已經出過聲：呼叫端要「播完已收到的部分、收氣泡、
    // 回報失敗」，而不是假裝這次沒發生過
    std::function<void(std::string error, bool committed)> onError;
  };

  // 串流合成。回傳的 handle 永遠不是 nullptr；cancel() 會一併中止目前的引擎
  // 與還沒跑到的 fallback 鏈。
  // 注意：失敗（例如空字串）可能在這個函式返回**之前**就同步回呼 onError。
  std::unique_ptr<TtsRequestHandle> stream(const std::string& text, const SynthesizeRequest& request, StreamSink sink);

  // 整段緩衝的舊介面，內部就是 stream() 配一個「永不承諾」的 sink ——
  // 呼叫端還沒出聲，所以 fallback 語意與串流化之前完全一致。
  // 全部引擎都失敗時 outcome 為 nullopt，error 會列出每個引擎各自的失敗原因。
  void synthesize(const std::string& text, const SynthesizeRequest& request, std::function<void(std::optional<SynthesizeOutcome>, std::string error)> done);

private:
  struct Availability {
    bool ok = false;
    // 偵測完成當下的 nowMs_()，「不可用」超過 kUnavailableRetryMs 就重探
    int64_t atMs = 0;
  };

  // 帶快取的可用性查詢。同一個引擎的偵測進行中時，後到的查詢排隊共用結果
  //（啟動時系統匣的引擎清單與語音清單會同時打進來，沒有去重會對線上服務
  // 同時發好幾個一樣的請求）
  void checkAvailable(TtsEngine* engine, std::function<void(bool)> done);

  std::vector<std::unique_ptr<TtsEngine>> engines_;
  std::map<std::string, Availability> availability_;
  std::map<std::string, std::vector<std::function<void(bool)>>> probing_;
  std::function<int64_t()> nowMs_;
};

}  // namespace l2m
