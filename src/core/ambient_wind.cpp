#include "ambient_wind.h"

#include <algorithm>
#include <cmath>

namespace l2m {

WindVector ambientWind(const AmbientWindTuning& tuning, bool enabled, WindDirection direction, double strength, double nowMs) {
  if (!enabled || strength <= 0) return {};

  constexpr double kTau = 6.283185307179586;  // 2π
  // 兩個正弦疊出的陣風訊號 g ∈ [-1, 1]（兩波峰／兩波谷對齊時才碰到端點）
  const double a = std::sin(kTau * nowMs / tuning.gustPeriodMsA);
  const double b = std::sin(kTau * nowMs / tuning.gustPeriodMsB);
  const double g = tuning.gustWeightA * a + (1 - tuning.gustWeightA) * b;
  // 包絡 ∈ [1 - gustDepth, 1]：恆為正，所以風向不會在週期中途反轉
  const double envelope = 1 - tuning.gustDepth * (1 - g) / 2;

  const double magnitude = tuning.maxWind * std::clamp(strength, 0.0, 1.0) * envelope;
  WindVector out;
  out.x = direction == WindDirection::Left ? -magnitude : magnitude;
  return out;
}

}  // namespace l2m
