#include "stream_decoders.h"

#include <QDebug>
#include <QString>

#include <algorithm>

#include "core/audio_stream_format.h"
#include "miniaudio.h"
#include "mp3_push_decoder.h"

namespace l2m {

namespace {

// MA_DR_MP3_MAX_SAMPLES_PER_FRAME（miniaudio.h:61076）。shim 的 pcmOut 至少要這麼大。
constexpr size_t kMaxMp3SamplesPerFrame = 2304;

}  // namespace

// ---------------------------------------------------------------------------
// ma_data_converter 的薄包裝：s16 →f32、來源取樣率 → 48 kHz、單聲道 → 立體聲。
// 有狀態且支援分批餵入，所以每一塊 chunk 解出來多少就轉多少。
// ---------------------------------------------------------------------------
class Resampler {
public:
  ~Resampler() {
    if (ok_) ma_data_converter_uninit(&converter_, nullptr);
  }

  bool init(ma_uint32 channels, ma_uint32 sampleRate) {
    ma_data_converter_config config = ma_data_converter_config_init(ma_format_s16, ma_format_f32, channels, kPlaybackChannels, sampleRate, kPlaybackSampleRate);
    if (ma_data_converter_init(&config, nullptr, &converter_) != MA_SUCCESS) return false;
    ok_ = true;
    return true;
  }

  // 回傳 (吃掉的輸入 frame 數, 產出的輸出 frame 數)
  std::pair<uint64_t, uint64_t> process(const short* in, uint64_t inFrames, float* out, uint64_t outFrames) {
    if (!ok_) return {0, 0};
    ma_uint64 inCount = inFrames;
    ma_uint64 outCount = outFrames;
    if (ma_data_converter_process_pcm_frames(&converter_, in, &inCount, out, &outCount) != MA_SUCCESS) {
      return {0, 0};
    }
    return {inCount, outCount};
  }

private:
  ma_data_converter converter_{};
  bool ok_ = false;
};

// ---------------------------------------------------------------------------
// BlobDecoder：整段解碼
// ---------------------------------------------------------------------------

BlobDecoder::~BlobDecoder() {
  if (decoder_) {
    ma_decoder_uninit(decoder_);
    delete decoder_;
    decoder_ = nullptr;
  }
}

uint32_t BlobDecoder::decode(AudioSegment& seg, float* out, uint32_t maxFrames) {
  if (done_ || maxFrames == 0) return 0;
  // 整段解：位元組全到齊才動得了。ma_decoder_init_memory 會嗅探標頭，
  // FLAC 還會回頭 seek，兩件事都需要整塊資料在手上。
  if (!seg.closed) return 0;

  if (!decoder_) {
    if (seg.encoded.empty()) {
      done_ = true;
      return 0;
    }
    decoder_ = new ma_decoder();
    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, kPlaybackChannels, kPlaybackSampleRate);
    // seg.closed 為真，所以 encoded 不會再擴容 —— 解碼器可以直接讀這塊記憶體
    if (ma_decoder_init_memory(seg.encoded.data(), seg.encoded.size(), &config, decoder_) != MA_SUCCESS) {
      delete decoder_;
      decoder_ = nullptr;
      done_ = true;
      error_ = "This audio format cannot be decoded (only wav, mp3 and flac are supported)";
      qWarning() << "[tts] 音訊解碼失敗（格式:" << QString::fromStdString(seg.mime) << "）";
      return 0;
    }
    seg.consumed = seg.encoded.size();
  }

  ma_uint64 framesRead = 0;
  ma_decoder_read_pcm_frames(decoder_, out, maxFrames, &framesRead);
  if (framesRead == 0) done_ = true;
  return static_cast<uint32_t>(framesRead);
}

// ---------------------------------------------------------------------------
// Mp3PushDecoder：邊收邊解
// ---------------------------------------------------------------------------

Mp3PushDecoder::~Mp3PushDecoder() = default;

bool Mp3PushDecoder::ensureConverter(uint32_t channels, uint32_t sampleRate) {
  if (converter_ && channels == channels_ && sampleRate == sampleRate_) return true;
  if (channels == 0 || sampleRate == 0) return false;

  if (converter_) {
    // 串流中途換格式：MP3 允許逐 frame 改，但 Edge 不會這樣做。
    // 真的發生就重建轉換器，並留個記錄 —— 接縫處可能有一聲喀噠。
    qWarning() << "[tts] MP3 串流中途換了格式：" << channels_ << "ch/" << sampleRate_ << "Hz →" << channels << "ch/" << sampleRate << "Hz";
  }
  converter_ = std::make_unique<Resampler>();
  if (!converter_->init(channels, sampleRate)) {
    converter_.reset();
    error_ = "This MP3 stream uses an audio format the player cannot convert";
    return false;
  }
  channels_ = channels;
  sampleRate_ = sampleRate;
  return true;
}

uint32_t Mp3PushDecoder::drain(float* out, uint32_t maxFrames) {
  if (!converter_ || pendingOffset_ >= pendingFrames_) return 0;
  const uint64_t available = pendingFrames_ - pendingOffset_;
  const auto [consumed, produced] = converter_->process(pending_.data() + pendingOffset_ * channels_, available, out, maxFrames);
  pendingOffset_ += static_cast<size_t>(consumed);
  if (pendingOffset_ >= pendingFrames_) {
    pendingFrames_ = 0;
    pendingOffset_ = 0;
  }
  return static_cast<uint32_t>(produced);
}

uint32_t Mp3PushDecoder::decode(AudioSegment& seg, float* out, uint32_t maxFrames) {
  if (!error_.empty() || maxFrames == 0) return 0;

  if (state_.empty()) {
    state_.resize(l2mMp3DecoderSize());
    l2mMp3DecoderInit(state_.data());
    pending_.resize(kMaxMp3SamplesPerFrame);
  }

  uint32_t written = 0;
  while (written < maxFrames) {
    // 先把上一輪沒轉完的樣本吐乾淨，再解新的 frame
    const uint32_t drained = drain(out + static_cast<size_t>(written) * kPlaybackChannels, maxFrames - written);
    written += drained;
    if (written >= maxFrames) break;
    if (pendingFrames_ > pendingOffset_) {
      // 轉換器一口都吃不下（輸出滿了），下次再來
      if (drained == 0) break;
      continue;
    }

    const size_t available = seg.encoded.size() - seg.consumed;
    if (available == 0) break;

    int frameBytes = 0;
    int channels = 0;
    int sampleRate = 0;
    const int frames = l2mMp3DecoderDecode(state_.data(), reinterpret_cast<const unsigned char*>(seg.encoded.data()) + seg.consumed, static_cast<int>(std::min<size_t>(available, 0x7FFFFFFF)),
                                           pending_.data(), &frameBytes, &channels, &sampleRate);

    if (frameBytes <= 0) {
      // 湊不出一個完整 frame。這在串流下是常態（下一個 wss 訊框還沒到），
      // **不是**錯誤，也沒有留下任何壞狀態 —— 補上位元組再叫一次就好。
      break;
    }
    seg.consumed += static_cast<size_t>(frameBytes);
    if (frames <= 0) continue;  // 跳過了 ID3 之類的垃圾，前進即可

    if (!ensureConverter(static_cast<uint32_t>(channels), static_cast<uint32_t>(sampleRate))) {
      return written;
    }
    pendingFrames_ = static_cast<size_t>(frames);
    pendingOffset_ = 0;
  }

  // 消耗掉的前綴定期丟掉：一句長台詞的 MP3 有好幾百 KB，一直留著沒有意義。
  // seg.encoded 只有這裡會縮，而且 Mp3PushDecoder 不持有指進去的長命指標。
  if (seg.consumed > 64 * 1024) {
    seg.encoded.erase(seg.encoded.begin(), seg.encoded.begin() + static_cast<long>(seg.consumed));
    seg.consumed = 0;
  }
  return written;
}

// ---------------------------------------------------------------------------

std::unique_ptr<StreamDecoder> makeDecoder(const AudioSegment& seg) {
  const char* data = seg.encoded.data() + seg.consumed;
  const size_t size = seg.encoded.size() - seg.consumed;
  const AudioContainer container = sniffContainer(data, size);

  // 位元組還不夠認：段還沒收完就再等一塊，收完了就死馬當活馬醫交給整段解碼器
  if (container == AudioContainer::NeedMore && !seg.closed) return nullptr;

  if (container == AudioContainer::Mp3) return std::make_unique<Mp3PushDecoder>();
  // WAV 走整段解碼就夠了：目前唯一會分塊送 WAV 的來源是句段管線，
  // 而它的每一段本來就是一個完整的檔案（closed 之後才解）。
  return std::make_unique<BlobDecoder>();
}

}  // namespace l2m
