#pragma once

// 把「一段編碼位元組」變成 f32 / 立體聲 / 48 kHz 的 PCM，餵進 AudioPlayer 的環形緩衝。
//
// 為什麼要有這一層：舊版直接 ma_decoder_init_memory(bytes_.data(), bytes_.size(), …)，
// 需要一整塊完整、不會搬家的位元組。串流要邊收邊解，就得把「怎麼解」與
// 「什麼時候解得動」抽出來 —— 不同容器的答案完全不同：WAV 可以從任意 frame
// 邊界續接、MP3 要湊滿一個 frame、FLAC 只能整段解。
//
// 解碼一律在 GUI 執行緒進行（由 AudioPlayer::pump() 呼叫），即時音訊執行緒只從
// 環形緩衝讀已經解好的 PCM。這條線不能跨 —— 見 audio_player.h 開頭的執行緒契約。

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

typedef struct ma_decoder ma_decoder;

namespace l2m {

// stream_decoders.cpp 內部型別：包住 ma_data_converter（s16 → f32、
// 來源取樣率 → 48 kHz、單聲道 → 立體聲）。它在 miniaudio 裡是匿名 struct
// 的 typedef，沒辦法像 ma_decoder 那樣前置宣告，所以走 pimpl。
class Resampler;

// 播放格式。統一成 f32 立體聲 48 kHz：RMS 直接對浮點取，不必再依格式分支。
inline constexpr uint32_t kPlaybackChannels = 2;
inline constexpr uint32_t kPlaybackSampleRate = 48000;

// 一「段」編碼位元組：句段管線的一句，或位元組串流的一整段輸出。
//
// **不變式：closed 為 true 之後，encoded 不會再被 append。**
// 解碼器可以（而且只有這時可以）持有指進 encoded 的長命指標 —— BlobDecoder 的
// ma_decoder 就是這樣做的。closed 之前一律每次從 encoded.data() + consumed
// 重新取，因為 append 會讓 std::vector 擴容換位址，舊指標當場失效。
struct AudioSegment {
  // 只作診斷；實際格式由解碼器自己嗅探（沿用舊版 audio_player.h 的契約）
  std::string mime;
  std::vector<char> encoded;
  // 已經交給解碼器的前綴長度
  size_t consumed = 0;
  // endSegment() 之後為 true：不會再有位元組進來
  bool closed = false;
};

class StreamDecoder {
public:
  virtual ~StreamDecoder() = default;

  // 解出至多 maxFrames 個 f32/2ch/48000 的 frame 寫進 out，回傳實際寫入的 frame 數。
  // 回 0 有兩種意思，由 seg.closed 區分：
  //   closed == false → 位元組還不夠，等下一塊
  //   closed == true  → 這一段解完了（或解不了，看 error()）
  virtual uint32_t decode(AudioSegment& seg, float* out, uint32_t maxFrames) = 0;

  // 非空代表這一段播不出來。面向使用者的英文（會一路傳到 AI 與錯誤對話框）
  virtual const std::string& error() const = 0;
};

// 整段解碼：位元組全到齊才動得了，內部就是舊版的 ma_decoder_init_memory。
// 存在理由有二：(1) FLAC 與任何未知格式沒有增量解碼路徑，只能整段解；
// (2) 播放器改成串流形狀的第一步用它，對外行為就能一位元組都不變。
class BlobDecoder : public StreamDecoder {
public:
  ~BlobDecoder() override;

  uint32_t decode(AudioSegment& seg, float* out, uint32_t maxFrames) override;
  const std::string& error() const override { return error_; }

private:
  ma_decoder* decoder_ = nullptr;
  bool done_ = false;
  std::string error_;
};

// 邊收邊解的 MP3（Edge TTS 的 wss 訊框）。
//
// 用 miniaudio 內建 dr_mp3 的 **push** API（經 mp3_push_decoder.h 的 shim），
// 不是高階的 ma_decoder —— 後者資料不足時會把 atEnd 設成 sticky 的 true，
// 而且補料門檻是 16 KB，在 Edge 的 48 kbps 上等於要先緩衝 2.7 秒。
// 完整理由寫在 miniaudio_impl.c。
class Mp3PushDecoder : public StreamDecoder {
public:
  ~Mp3PushDecoder() override;

  uint32_t decode(AudioSegment& seg, float* out, uint32_t maxFrames) override;
  const std::string& error() const override { return error_; }

private:
  // 把 s16 / 來源取樣率 / 來源聲道數轉成 f32 / 48 kHz / 立體聲
  bool ensureConverter(uint32_t channels, uint32_t sampleRate);
  uint32_t drain(float* out, uint32_t maxFrames);

  std::vector<unsigned char> state_;
  // 解出來還沒轉換完的 s16 樣本（交錯），以及已經吃掉多少 frame
  std::vector<short> pending_;
  size_t pendingFrames_ = 0;
  size_t pendingOffset_ = 0;
  uint32_t channels_ = 0;
  uint32_t sampleRate_ = 0;
  std::unique_ptr<Resampler> converter_;
  std::string error_;
};

// 依容器格式挑一個解碼器。
//
// 回 nullptr 代表「位元組還不夠，認不出來」—— 呼叫端要等下一塊再問一次
// （seg.closed 之後就不會再回 nullptr）。
std::unique_ptr<StreamDecoder> makeDecoder(const AudioSegment& seg);

}  // namespace l2m
