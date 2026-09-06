#pragma once

// TimerHost 的正式實作，用 QTimer 撐起 setTimeout / clearTimeout 語意。
// core/idle.h 把計時器抽象成 TimerHost 是為了測試（tests/fake_timers.h 的假時鐘），
// 這裡是 production 用的那一份。
//
// 兩個地雷都在 onFired 裡：
//  1) 重入：IdleReset::fire() 會在回呼裡 clearTimeout(自己)，IdlePerformer::cycle()
//     會在回呼裡排下一輪。所以要「先把自己從表裡拿掉，再呼叫使用者的 callback」，
//     否則回呼中途註冊的新計時器會被接下來的 erase 連坐清掉。
//  2) 生命週期：不能在 QTimer 自己的 timeout 訊號發送過程中直接 delete 它，
//     一律走 deleteLater()。QTimer 掛在 host 底下當子物件，host 消失時一起收乾淨。

#include <QObject>

#include <map>

#include "idle.h"

class QTimer;

namespace l2m {

class QtTimerHost : public QObject, public TimerHost {
  Q_OBJECT

public:
  explicit QtTimerHost(QObject* parent = nullptr);
  ~QtTimerHost() override;

  int setTimeout(std::function<void()> fn, double delayMs) override;
  void clearTimeout(int id) override;

  // 診斷／測試用：目前還在倒數的計時器數量
  size_t pendingCount() const { return timers_.size(); }

private:
  void onFired(int id);
  // 停掉並排程刪除，同時把項目從表裡移除
  void discard(int id);

  struct Entry {
    QTimer* timer = nullptr;  // 由 this 持有（QObject 親子關係）
    std::function<void()> fn;
  };

  std::map<int, Entry> timers_;
  int nextId_ = 1;
};

}  // namespace l2m
