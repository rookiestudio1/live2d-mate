#include "splash_timing.h"

#include <algorithm>
#include <cmath>

namespace l2m {

double splashSweepOffset(double elapsedMs, double trackWidth, double segmentWidth, double cycleMs) {
  // 除零保護：時鐘還沒 start() 或常數被設成 0 時，停在起始位置而不是 NaN
  if (!(cycleMs > 0.0)) return -segmentWidth;

  double t = std::fmod(elapsedMs, cycleMs) / cycleMs;
  // fmod 對負數會回負值（elapsedMs 理論上不會是負的，但 QElapsedTimer
  // 沒 start() 過時 elapsed() 的回傳值未定義，這裡不賭）
  if (t < 0.0) t += 1.0;

  t = t * t * (3.0 - 2.0 * t);  // smoothstep：兩端慢、中間快
  return -segmentWidth + t * (trackWidth + segmentWidth);
}

double splashRemainingHoldMs(double elapsedMs, double minVisibleMs) { return std::max(0.0, minVisibleMs - elapsedMs); }

}  // namespace l2m
