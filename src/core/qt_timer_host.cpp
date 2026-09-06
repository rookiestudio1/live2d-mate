#include "qt_timer_host.h"

#include <QTimer>

#include <algorithm>
#include <utility>

namespace l2m {

QtTimerHost::QtTimerHost(QObject* parent) : QObject(parent) {}

QtTimerHost::~QtTimerHost() = default;

int QtTimerHost::setTimeout(std::function<void()> fn, double delayMs) {
  const int id = nextId_++;

  Entry entry;
  entry.fn = std::move(fn);
  entry.timer = new QTimer(this);
  entry.timer->setSingleShot(true);
  // QTimer 只吃整數毫秒；負值在各平台行為不一致，先夾成 0（等同「下一輪事件迴圈」）
  entry.timer->setInterval(static_cast<int>(std::max(0.0, delayMs)));
  connect(entry.timer, &QTimer::timeout, this, [this, id] { onFired(id); });

  QTimer* raw = entry.timer;
  timers_.emplace(id, std::move(entry));
  raw->start();
  return id;
}

void QtTimerHost::clearTimeout(int id) { discard(id); }

void QtTimerHost::discard(int id) {
  const auto it = timers_.find(id);
  if (it == timers_.end()) return;
  QTimer* timer = it->second.timer;
  timers_.erase(it);
  if (timer) {
    timer->stop();
    // 不能在 timeout 的訊號發送過程中直接 delete，交給事件迴圈
    timer->deleteLater();
  }
}

void QtTimerHost::onFired(int id) {
  const auto it = timers_.find(id);
  if (it == timers_.end()) return;

  // 先把 callback 複製出來、把自己從表裡移除，才呼叫 ——
  // 回呼裡常常會排下一輪或取消自己，留在表裡會互相踩到。
  std::function<void()> fn = std::move(it->second.fn);
  discard(id);

  if (fn) fn();
}

}  // namespace l2m
