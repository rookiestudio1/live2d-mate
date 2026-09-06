#include "idle.h"

#include <cmath>

namespace l2m {

namespace {

bool isUsableDelay(double delay) { return std::isfinite(delay) && delay > 0; }

}  // namespace

// ── IdleReset ────────────────────────────────────────────────

void IdleReset::touch() {
  stop();
  const double delay = deps_.delayMs();
  if (!isUsableDelay(delay)) return;
  timerId_ = timers_.setTimeout([this] { fire(); }, delay);
}

void IdleReset::stop() {
  if (timerId_ == -1) return;
  timers_.clearTimeout(timerId_);
  timerId_ = -1;
}

void IdleReset::fire() {
  timerId_ = -1;
  // 還在忙就不清，重新倒數 —— 不然話講到一半表情會被抽掉
  if (deps_.isBusy()) {
    touch();
    return;
  }
  // reset 失敗不往外丟：自動復原炸掉不該連累整個 app
  try {
    deps_.reset();
  } catch (...) {
  }
}

// ── McpQuietReset ────────────────────────────────────────────

// 沒有 MCP 驅動時 delay 回 0 —— IdleReset 把 0 當停用，所以連 touch() 都不會排程
McpQuietReset::McpQuietReset(TimerHost& timers, Deps deps)
  : reset_(std::move(deps.reset)),
    inner_(timers, IdleReset::Deps{
                     [this] { return driving_ ? kMcpQuietResetMs : 0.0; },
                     std::move(deps.isBusy),
                     [this] { fire(); },
                   }) {}

void McpQuietReset::onCommand() {
  driving_ = true;
  inner_.touch();
}

void McpQuietReset::onActivity() {
  // 沒有 MCP 驅動就什麼都不做：使用者自己玩桌寵不該被這條 5 秒的規則清掉
  if (!driving_) return;
  inner_.touch();
}

void McpQuietReset::stop() {
  driving_ = false;
  inner_.stop();
}

void McpQuietReset::fire() {
  // 先熄火再交棒：reset 途中若又冒出活動，也不該把這一輪接回去
  driving_ = false;
  if (reset_) reset_();
}

// ── IdlePerformer ────────────────────────────────────────────

void IdlePerformer::touch() {
  stop();
  const double delay = deps_.idleMs();
  if (!isUsableDelay(delay)) return;
  mode_ = State::Waiting;
  timerId_ = timers_.setTimeout([this] { cycle(); }, delay);
}

void IdlePerformer::stop() {
  mode_ = State::Stopped;
  if (timerId_ == -1) return;
  timers_.clearTimeout(timerId_);
  timerId_ = -1;
}

void IdlePerformer::refresh() {
  const double delay = deps_.idleMs();
  if (!isUsableDelay(delay)) {
    stop();
    return;
  }
  touch();
}

// 每輪：忙碌就只排下一輪（跳過不退出）；否則表演一次再排下一輪
void IdlePerformer::cycle() {
  timerId_ = -1;
  mode_ = State::Performing;
  if (!deps_.isBusy()) {
    try {
      deps_.perform();
    } catch (...) {
      // 表演失敗不往外丟，下一輪照跑
    }
  }
  scheduleNext();
}

void IdlePerformer::scheduleNext() {
  const double interval = deps_.intervalMs();
  if (!isUsableDelay(interval)) {
    stop();
    return;
  }
  timerId_ = timers_.setTimeout([this] { cycle(); }, interval);
}

}  // namespace l2m
