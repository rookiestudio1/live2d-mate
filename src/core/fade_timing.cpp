#include "fade_timing.h"

#include <algorithm>

namespace l2m {

namespace {

double clamp01(double value) { return std::clamp(value, 0.0, 1.0); }

}  // namespace

void Fade::set(double alpha) {
  from_ = target_ = clamp01(alpha);
  startMs_ = 0.0;
  durationMs_ = 0.0;
}

void Fade::begin(double target, double nowMs) {
  from_ = alphaAt(nowMs);
  target_ = clamp01(target);
  startMs_ = nowMs;
  // 距離越短跑越快，每單位 alpha 的速度維持一致
  durationMs_ = kFadeDurationMs * std::abs(target_ - from_);
}

double Fade::alphaAt(double nowMs) const {
  if (durationMs_ <= 0.0) return target_;
  // 夾限而不是外推：nowMs 早於 startMs_（時鐘還沒 start）時停在起點，不會是 NaN
  double t = clamp01((nowMs - startMs_) / durationMs_);
  t = t * t * (3.0 - 2.0 * t);  // smoothstep：兩端慢、中間快，同 splash_timing.cpp
  return from_ + (target_ - from_) * t;
}

bool Fade::finished(double nowMs) const { return durationMs_ <= 0.0 || nowMs - startMs_ >= durationMs_; }

}  // namespace l2m
