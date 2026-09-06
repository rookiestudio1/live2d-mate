#include "echo_filter.h"

#include <algorithm>
#include <cmath>

namespace l2m {

namespace {

// 殘響衰到這個振幅就當它結束了。-60 dB（0.001）要多等兩輪，在 115 ms 的延遲
// 下就是多 230 ms 的無聲 —— 那段時間 speak(wait=true) 還沒回覆，AI 純粹在乾等。
// -46 dB 已經在任何桌面音量下都聽不到了。
constexpr double kTailFloor = 0.005;
// 尾巴的硬上限：feedback 調高時輪數會爆增，不設限一句話收尾要拖一秒以上
constexpr double kMaxTailSeconds = 1.2;
// 回授迴圈裡的值會一路衰減到 denormal，x86 上那是每個樣本數十個 cycle 的懲罰
constexpr float kDenormalFloor = 1e-20f;

double clampd(double value, double lo, double hi) { return std::min(std::max(value, lo), hi); }

// ── think 的音色，五顆可以各自調的常數 ──
// 115 ms：再短會糊進乾聲變成金屬味的 comb 音染，再長就聽成「講兩次」
constexpr double kThinkingDelayMs = 115;
// 每一輪重複保留多少 ＝「回音要響幾次」。也決定句尾要沖多長的尾巴（tailFrames）
constexpr double kThinkingFeedback = 0.26;
// 每一輪變多悶。越大越遠、越像在腦子裡
constexpr double kThinkingDamping = 0.40;
// **回音強弱就是這一顆**：0 ＝ 完全沒有回音，越大越濕
constexpr double kThinkingWet = 0.28;
// 乾聲保留多少。wet 疊上去會推高峰值，這裡先讓出一點空間免得削頂
constexpr double kThinkingDry = 0.85;

}  // namespace

EchoFilter::Params EchoFilter::thinkingVoice() {
  Params params;
  params.delayMs = kThinkingDelayMs;
  params.feedback = kThinkingFeedback;
  params.damping = kThinkingDamping;
  params.dry = kThinkingDry;
  params.wet = kThinkingWet;
  return params;
}

void EchoFilter::clear() {
  line_.clear();
  store_.clear();
  delayFrames_ = 0;
  cursor_ = 0;
  tailFrames_ = 0;
  channels_ = 0;
  feedback_ = 0;
  damping_ = 0;
  dry_ = 1;
  wet_ = 0;
  active_ = false;
}

void EchoFilter::reset(uint32_t sampleRate, uint32_t channels, const Params& params) {
  clear();
  if (sampleRate == 0 || channels == 0 || params.delayMs <= 0 || params.wet <= 0) return;

  delayFrames_ = std::max<size_t>(1, static_cast<size_t>(std::llround(params.delayMs * sampleRate / 1000.0)));
  channels_ = channels;
  line_.assign(delayFrames_ * channels, 0.0f);
  store_.assign(channels, 0.0f);
  feedback_ = static_cast<float>(clampd(params.feedback, 0.0, 0.9));
  damping_ = static_cast<float>(clampd(params.damping, 0.0, 0.95));
  dry_ = static_cast<float>(params.dry);
  wet_ = static_cast<float>(params.wet);
  active_ = true;

  // 衰到 kTailFloor 要幾輪重複。feedback 為 0 時只剩第一次回音，一輪就夠。
  const double repeats = feedback_ > 0 ? std::ceil(std::log(kTailFloor) / std::log(static_cast<double>(feedback_))) : 1.0;
  const size_t cap = static_cast<size_t>(kMaxTailSeconds * sampleRate);
  tailFrames_ = std::min(delayFrames_ * static_cast<size_t>(std::max(1.0, repeats)), cap);
}

void EchoFilter::process(float* samples, size_t frames) {
  if (!active_ || samples == nullptr || line_.empty()) return;

  const size_t channels = channels_;
  for (size_t frame = 0; frame < frames; ++frame) {
    float* slots = line_.data() + cursor_ * channels;
    float* input = samples + frame * channels;
    for (size_t channel = 0; channel < channels; ++channel) {
      const float echo = slots[channel];
      float store = echo * (1.0f - damping_) + store_[channel] * damping_;
      if (std::abs(store) < kDenormalFloor) store = 0.0f;
      store_[channel] = store;
      // 進延遲線的是乾聲，不是混完的輸出 —— 混音只影響送出去的那一份
      slots[channel] = input[channel] + store * feedback_;
      input[channel] = input[channel] * dry_ + echo * wet_;
    }
    cursor_ = cursor_ + 1 < delayFrames_ ? cursor_ + 1 : 0;
  }
}

}  // namespace l2m
