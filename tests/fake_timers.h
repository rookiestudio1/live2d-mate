#pragma once

// 測試用假時鐘（對應 vitest 的 useFakeTimers / advanceTimersByTimeAsync）。
// advance() 會依到期時間依序執行，途中新排的計時器只要在窗內也會被執行。

#include <functional>
#include <map>

#include "core/idle.h"

class FakeTimers : public l2m::TimerHost {
public:
  int setTimeout(std::function<void()> fn, double delayMs) override {
    const int id = nextId_++;
    timers_[id] = {now_ + delayMs, std::move(fn)};
    return id;
  }

  void clearTimeout(int id) override { timers_.erase(id); }

  void advance(double ms) {
    const double target = now_ + ms;
    while (true) {
      // 找最早到期且在窗內的計時器
      int dueId = -1;
      double dueAt = 0;
      for (const auto& [id, timer] : timers_) {
        if (timer.at <= target && (dueId == -1 || timer.at < dueAt)) {
          dueId = id;
          dueAt = timer.at;
        }
      }
      if (dueId == -1) break;
      auto fn = timers_[dueId].fn;
      timers_.erase(dueId);
      now_ = dueAt;
      fn();
    }
    now_ = target;
  }

private:
  struct Timer {
    double at = 0;
    std::function<void()> fn;
  };

  double now_ = 0;
  int nextId_ = 1;
  std::map<int, Timer> timers_;
};
