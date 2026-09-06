#include "presence_tracker.h"

#include <algorithm>

namespace l2m {

PresenceEvent PresenceTracker::sample(double monotonicMs, double wallMs, std::optional<double> idleMs) {
  PresenceEvent event;

  if (!hasSample_) {
    hasSample_ = true;
    lastWallMs_ = wallMs;
    activeSinceMs_ = monotonicMs;
    awaySinceMs_ = monotonicMs;
    return event;  // 第一次取樣只建基準，不發事件
  }

  // 休眠偵測：心跳約 5 秒一跳，牆上時鐘的增量遠大於此就是機器睡過
  //（GetTickCount 在 S3 期間停止，idleMs 靠不住 —— 見標頭）
  const double wallGap = wallMs - lastWallMs_;
  lastWallMs_ = wallMs;
  if (wallGap > options_.resumeGapMs) {
    state_ = PresenceState::Active;
    activeSinceMs_ = monotonicMs;
    event.kind = PresenceEvent::Kind::Resumed;
    event.awayForMs = wallGap;
    return event;
  }

  // 平台拿不到閒置訊號：永遠維持 Active（見標頭）
  if (!idleMs) {
    state_ = PresenceState::Active;
    return event;
  }

  if (state_ == PresenceState::Active) {
    if (*idleMs >= options_.awayAfterMs) {
      state_ = PresenceState::Away;
      // 離開其實從最後一次輸入就開始了，不是從這次取樣才開始
      awaySinceMs_ = monotonicMs - *idleMs;
      event.kind = PresenceEvent::Kind::WentAway;
      return event;
    }
    if (monotonicMs - activeSinceMs_ >= options_.longSessionMs) {
      event.kind = PresenceEvent::Kind::LongSession;
      event.sessionForMs = monotonicMs - activeSinceMs_;
      // 重新起算：下一次又滿了才再發，不會每 5 秒轟炸一次
      activeSinceMs_ = monotonicMs;
      return event;
    }
    return event;
  }

  // Away → 有輸入了
  if (*idleMs < options_.awayAfterMs) {
    state_ = PresenceState::Active;
    const double awayFor = monotonicMs - awaySinceMs_;
    activeSinceMs_ = monotonicMs;
    // 離開得不夠久就默默轉回 Active，不值得為了倒杯水打一次招呼
    if (awayFor >= options_.greetAfterAwayMs) {
      event.kind = PresenceEvent::Kind::CameBack;
      event.awayForMs = awayFor;
    }
  }
  return event;
}

double PresenceTracker::continuousActiveMs(double monotonicMs) const {
  if (state_ != PresenceState::Active) return 0;
  return std::max(0.0, monotonicMs - activeSinceMs_);
}

}  // namespace l2m
