#pragma once

// 把「只會整段吐出來」的引擎包成串流引擎：文字切句 → 逐句合成 →
// 第一句一回來就送出去，同時把下一句的請求先丟出去。
//
// 為什麼需要它：位元組串流要靠協定支援（Edge 的 wss 訊框、HTTP 的 chunked
// 回應），但 sapi 是寫檔、而多數自架端點是「推論跑完才回一整包」。這些來源
// 沒有位元組可以邊收邊播，卻仍然可以「邊播第 N 句、邊合成第 N+1 句」。
// 實測：本機 GPT-SoVITS 唸一段 70 字要 10 秒才開口，切句之後約 2 秒。
//
// 每一句是播放器眼中的一「段」（onSegmentEnd），段與段共用同一個環形緩衝與
// 同一個裝置，所以銜接處沒有間隙 —— 只要下一句在環清空之前備妥。
//
// **prefetch 深度刻意是 0**：第 N+1 句的請求在第 N 句「交給播放器」那一刻才送出，
// 所以合成與播放仍然完全重疊，只是同一時間只有一個請求在飛。
//
// 一開始寫成 1（同時兩個請求），實測反而更糟：本機 GPT-SoVITS 上
// 「32 字 + 30 字同時送」的第一句要 8152 ms，而同一句單獨送只要 3~4 秒 ——
// 兩個請求搶同一顆 GPU，先開口的時間被自己的預取拖慢了一倍。
// 開口延遲是這整件事的主要指標，所以寧可不預取。
// （sapi 那邊還有第二個理由：每一句是一個 PowerShell 行程，開太多很難看。）

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "text_segments.h"
#include "tts_types.h"

namespace l2m {

// 額外預取幾句。0 ＝ 只在「上一句交給播放器」之後才送下一句的請求
inline constexpr size_t kSegmentPrefetch = 0;

class SegmentedTtsEngine : public TtsEngine {
public:
  // prefetch 是建構參數而不是寫死的常數：不同引擎的最佳值不一樣
  //（雲端 API 併發沒有代價，本機推論則是互相搶資源），而且測試要試得到
  // 「後面的句子先合成完」那條順序保證 —— 預取 0 的時候它根本不會發生。
  explicit SegmentedTtsEngine(std::unique_ptr<TtsEngine> inner, SegmentOptions options = {}, size_t prefetch = kSegmentPrefetch);
  ~SegmentedTtsEngine() override;

  // id 與 name 透傳內層：對 TtsManager、設定頁與 get_state 來說，
  // 包不包管線是實作細節，不該變成另一個引擎
  const std::string& id() const override;
  const std::string& name() const override;

  void isAvailable(std::function<void(bool)> done) override;
  void listVoices(std::function<void(std::vector<VoiceInfo>, std::string)> done) override;

  bool streams() const override { return true; }

  std::unique_ptr<TtsRequestHandle> synthesize(const std::string& text, const SpeakOptions& options, TtsStreamSink sink) override;

private:
  struct Job;

  void pump(const std::shared_ptr<Job>& job);

  std::unique_ptr<TtsEngine> inner_;
  SegmentOptions options_;
  size_t prefetch_ = kSegmentPrefetch;
};

}  // namespace l2m
