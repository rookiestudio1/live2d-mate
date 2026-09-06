#include "gesture_detector.h"

#include <cmath>

namespace l2m {

Gesture GestureDetector::feed(const PointerSample& sample) {
  Gesture gesture;

  if (sample.pressed) {
    // 按著：撫摸中斷（按著拖是 dragMove 的地盤）
    resetPet();

    if (!pressing_) {
      pressing_ = true;
      longPressFired_ = false;
      pressStartMs_ = sample.nowMs;
      pressX_ = sample.screenX;
      pressY_ = sample.screenY;
      pressMovedPx_ = 0;
      pressPart_ = bodyPartFor(sample.areas);
      pressAreas_ = sample.areas;
      pressLocalX_ = sample.x;
      pressLocalY_ = sample.y;
    } else {
      pressMovedPx_ = std::max(pressMovedPx_, std::hypot(sample.screenX - pressX_, sample.screenY - pressY_));
    }
    return gesture;
  }

  // 放開的瞬間：短按小位移＝一次 tap。單擊**不在這裡發** ——
  // 掛起等 multiTapWindowMs（tick 收割），時窗內連滿 multiTapCount 下
  // 才立刻發 MultiTap 並吞掉掛起的擊數（見檔頭）。
  if (pressing_) {
    // 位移補上放開點：拖曳期間若沒有中途樣本（游標輪詢最密也才 10 Hz），
    // 只靠按著時的樣本會漏掉最後一段
    pressMovedPx_ = std::max(pressMovedPx_, std::hypot(sample.screenX - pressX_, sample.screenY - pressY_));
    const bool wasTap = !longPressFired_ && sample.nowMs - pressStartMs_ < options_.longPressMs && pressMovedPx_ <= options_.tapMaxMovePx;
    const BodyPart part = pressPart_;
    std::vector<std::string> areas = std::move(pressAreas_);
    const double localX = pressLocalX_;
    const double localY = pressLocalY_;
    resetPress();
    if (wasTap) {
      // 超出時窗的新擊數重新起算 —— 掛起的舊單擊理論上早被 tick 收割了
      //（40ms 一次 ≪ 時窗），真的撞上競態也只是兩擊併成一次反應
      tapCount_ = (lastTapMs_ >= 0 && sample.nowMs - lastTapMs_ <= options_.multiTapWindowMs) ? tapCount_ + 1 : 1;
      lastTapMs_ = sample.nowMs;
      pendingTapPart_ = part;
      pendingTapAreas_ = std::move(areas);
      pendingTapX_ = localX;
      pendingTapY_ = localY;
      if (tapCount_ >= options_.multiTapCount) {
        gesture.kind = GestureKind::MultiTap;
        gesture.part = part;
        gesture.taps = options_.multiTapCount;
        gesture.areas = std::move(pendingTapAreas_);
        gesture.x = localX;
        gesture.y = localY;
        resetPendingTap();
        return gesture;
      }
    }
    return gesture;
  }

  // 放開狀態的 hover：撫摸偵測
  return petFrom(sample);
}

Gesture GestureDetector::petFrom(const PointerSample& sample) {
  Gesture gesture;

  // 離開角色（或 hitTest 沒結果）就重新來過
  if (!sample.inside) {
    resetPet();
    return gesture;
  }

  const BodyPart part = bodyPartFor(sample.areas);
  const bool stale = petActive_ && sample.nowMs - petLastMoveMs_ > options_.petIdleMs;
  // 換部位或停太久：這一段撫摸結束，重新累計（petFired_ 一併歸零，
  // 停一下再摸才會再觸發一次）
  if (!petActive_ || part != petPart_ || stale) {
    petActive_ = true;
    petFired_ = false;
    petPart_ = part;
    petLastX_ = sample.x;
    petLastMoveMs_ = sample.nowMs;
    petTravelPx_ = 0;
    petDirection_ = 0;
    petReversals_ = 0;
    return gesture;
  }

  const double dx = sample.x - petLastX_;
  if (std::abs(dx) >= 1) {
    const int direction = dx > 0 ? 1 : -1;
    if (petDirection_ != 0 && direction != petDirection_) ++petReversals_;
    petDirection_ = direction;
    petTravelPx_ += std::abs(dx);
    petLastX_ = sample.x;
    petLastMoveMs_ = sample.nowMs;
  }

  if (!petFired_ && petReversals_ >= options_.petMinReversals && petTravelPx_ >= options_.petMinTravelPx) {
    petFired_ = true;  // 這一段撫摸只觸發一次，停頓（petIdleMs）之後才能再來
    gesture.kind = GestureKind::Pet;
    gesture.part = petPart_;
  }
  return gesture;
}

Gesture GestureDetector::tick(double nowMs) {
  Gesture gesture;

  // 遞延單擊收割：時窗過了、沒有新的按壓在進行 → 這（一兩）下確定是單擊。
  // 按著時不收割 —— 拖曳中冒出點擊動作看起來像抽搐，等放開再說
  if (!pressing_ && tapCount_ > 0 && lastTapMs_ >= 0 && nowMs - lastTapMs_ > options_.multiTapWindowMs) {
    gesture.kind = GestureKind::Tap;
    gesture.part = pendingTapPart_;
    gesture.taps = tapCount_;
    gesture.areas = std::move(pendingTapAreas_);
    gesture.x = pendingTapX_;
    gesture.y = pendingTapY_;
    resetPendingTap();
    return gesture;
  }

  // 長按：按著不放、沒怎麼動、時間到 —— 只在這裡成立（放開就變 tap 判定）
  if (pressing_ && !longPressFired_ && nowMs - pressStartMs_ >= options_.longPressMs && pressMovedPx_ <= options_.tapMaxMovePx) {
    longPressFired_ = true;
    gesture.kind = GestureKind::LongPress;
    gesture.part = pressPart_;
    return gesture;
  }

  // 撫摸逾時：這一段結束，下次移動重新累計
  if (petActive_ && nowMs - petLastMoveMs_ > options_.petIdleMs) resetPet();
  return gesture;
}

void GestureDetector::reset() {
  resetPress();
  resetPet();
  resetPendingTap();
}

void GestureDetector::resetPress() {
  pressing_ = false;
  longPressFired_ = false;
  pressMovedPx_ = 0;
  pressPart_ = BodyPart::Unknown;
  pressAreas_.clear();
  pressLocalX_ = 0;
  pressLocalY_ = 0;
}

void GestureDetector::resetPendingTap() {
  tapCount_ = 0;
  lastTapMs_ = -1;
  pendingTapPart_ = BodyPart::Unknown;
  pendingTapAreas_.clear();
  pendingTapX_ = 0;
  pendingTapY_ = 0;
}

void GestureDetector::resetPet() {
  petActive_ = false;
  petFired_ = false;
  petPart_ = BodyPart::Unknown;
  petTravelPx_ = 0;
  petDirection_ = 0;
  petReversals_ = 0;
}

}  // namespace l2m
