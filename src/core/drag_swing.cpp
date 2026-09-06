#include "drag_swing.h"

#include <cmath>

namespace l2m {

namespace {

// 對稱夾限：|value| 不超過 limit
double clampAbs(double value, double limit) { return std::clamp(value, -limit, limit); }

}  // namespace

void DragSwing::addSample(double dxPx, double dyPx, double nowMs) {
  double dt = tuning_.assumedDtMs;
  if (lastInputMs_ >= 0) {
    const double gap = nowMs - lastInputMs_;
    if (gap > tuning_.newRoundGapMs) {
      // 新一輪拖曳：舊速度先按閒置時間衰減到位，才不會混進新的瞬時速度
      const double decay = std::exp(-gap / tuning_.releaseTauMs);
      vx_ *= decay;
      vy_ *= decay;
    } else {
      // 夾下限：同一毫秒的重複樣本等同一次大位移，不做除以零；
      // 夾上限：偶發的長間隔不至於把速度稀釋到近乎零
      dt = std::clamp(gap, 4.0, 50.0);
    }
  }
  const double ivx = dxPx / dt * 1000.0;
  const double ivy = dyPx / dt * 1000.0;
  // 時間感知 EMA：事件間隔不固定，權重要跟著 dt 走才有一致的時間常數
  const double alpha = 1.0 - std::exp(-dt / tuning_.emaTauMs);
  vx_ += alpha * (ivx - vx_);
  vy_ += alpha * (ivy - vy_);
  lastInputMs_ = nowMs;
}

DragSwing::Output DragSwing::sample(double nowMs) {
  Output out;
  if (lastInputMs_ < 0) return out;

  // 衰減是「距最後一次輸入的閒置時間」的純函數，不寫回速度狀態
  const double idle = std::max(0.0, nowMs - lastInputMs_);
  const double decay = std::exp(-idle / tuning_.releaseTauMs);
  double sx = vx_ * decay;
  double sy = vy_ * decay;

  // 速度模長夾上限，再檢查死區
  const double speed = std::hypot(sx, sy);
  if (speed < tuning_.deadZonePxPerSec) {
    // 已經低於死區就整個歸零，避免次法線值的長尾一直留在狀態裡
    reset();
    return out;
  }
  if (speed > tuning_.maxSpeedPxPerSec) {
    const double scale = tuning_.maxSpeedPxPerSec / speed;
    sx *= scale;
    sy *= scale;
  }

  // 符號約定（實機調校定案）：頭與身體是慣性滯後 —— 視窗往右甩，頭相對往左仰
  //（螢幕 y 向下為正，往下甩則頭相對上仰）；風力則與拖曳「同向」——
  // 頭髮往拖曳方向甩才自然（相對風的 -v 在實際模型上方向相反）。
  // 風只作用在物理粒子上，這就是頭髮方向能與頭身角度分開調的原因。
  out.angleXDeg = clampAbs(-sx * tuning_.angleXPerPxPerSec, tuning_.angleXMaxDeg);
  out.angleYDeg = clampAbs(sy * tuning_.angleYPerPxPerSec, tuning_.angleYMaxDeg);
  out.angleZDeg = clampAbs(sx * tuning_.angleZPerPxPerSec, tuning_.angleZMaxDeg);
  out.bodyXDeg = clampAbs(-sx * tuning_.bodyXPerPxPerSec, tuning_.bodyXMaxDeg);
  out.windX = clampAbs(sx * tuning_.windPerPxPerSec, tuning_.windMax);
  // 垂直要多翻一次軸：rig 空間 Y 朝上（重力預設 (0,-1)），螢幕 Y 朝下，
  // 「與拖曳同向」在垂直方向等於 rig 空間的 -sy
  out.windY = clampAbs(-sy * tuning_.windPerPxPerSec, tuning_.windMax);
  out.active = true;
  return out;
}

void DragSwing::reset() {
  vx_ = 0;
  vy_ = 0;
  lastInputMs_ = -1;
}

}  // namespace l2m
