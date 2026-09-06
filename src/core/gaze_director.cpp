#include "gaze_director.h"

#include <algorithm>
#include <cmath>

namespace l2m {

namespace {

// 同 core/idle.cpp 的慣例：0、負值或 NaN 一律當「不過渡」，直接到位
bool isUsableSpan(double span) { return std::isfinite(span) && span > 0; }

double smoothstep(double t) {
  return t * t * (3.0 - 2.0 * t);  // 兩端慢、中間快，同 fade_timing.cpp / splash_timing.cpp
}

}  // namespace

GazeOutput GazeDirector::tick(const GazeInput& in) {
  // 第一輪只記時間（lastTickMs_ < 0），dt 為 0 —— 同 CubismTargetPoint::Update
  // 的作法，避免拿一個沒有意義的 nowMs 差值當第一步。
  const double dt = (lastTickMs_ < 0) ? 0.0 : std::clamp(in.nowMs - lastTickMs_, 0.0, tuning_.maxStepMs);
  lastTickMs_ = in.nowMs;

  // 刻意排在 pinned／suppressed 判定之前：說話期間滑鼠有沒有在動，決定的是
  // 「說完之後要不要追回去」，那段時間的位移不能漏記。
  // （pinned 分支底下會再清掉 —— 理由見該處。）
  if (in.cursorMoved) lastMoveMs_ = in.nowMs;

  if (in.pinned) {
    // 釘選期間 focus 由 look_at 全權掌控，一個位元組都不能覆寫：
    // 25 Hz 的輪詢只要寫一次就把釘點抹掉了。
    // lastMoveMs_ 一併清掉 —— 解除釘選時若滑鼠正好靜止，應該停在正前方，
    // 而不是拿釘選期間的位移當「使用者剛剛在動」而立刻追出去。
    weight_ = 0;
    lastMoveMs_ = -1;
    settledFront_ = false;
    return {};
  }

  const bool wantTrack = in.enabled && !in.suppressed && lastMoveMs_ >= 0 && (in.nowMs - lastMoveMs_) < tuning_.holdMs;
  const double target = wantTrack ? 1.0 : 0.0;

  const double span = (target > weight_) ? tuning_.engageMs : tuning_.releaseMs;
  if (!isUsableSpan(span)) {
    weight_ = target;
  } else {
    const double step = dt / span;
    weight_ = (target > weight_) ? std::min(target, weight_ + step) : std::max(target, weight_ - step);
  }
  weight_ = std::clamp(weight_, 0.0, 1.0);

  if (weight_ <= 0 && target <= 0) {
    // 已經停在正前方：只寫收尾那一次，之後閉嘴。
    // 那一次不能省 —— 否則 focus 停在歸零前的殘值上，頭永遠歪一點點。
    if (settledFront_) return {};
    settledFront_ = true;
    return {true, 0.0};
  }

  settledFront_ = false;
  return {true, smoothstep(weight_)};
}

void GazeDirector::reset() {
  weight_ = 0;
  lastTickMs_ = -1;
  lastMoveMs_ = -1;
  settledFront_ = false;
}

}  // namespace l2m
