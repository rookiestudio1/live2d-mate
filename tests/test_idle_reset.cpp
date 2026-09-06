// IdleReset：閒置倒數的重新計時、忙碌時延後、觸發一次即停
#include <QtTest>

#include <stdexcept>

#include "core/idle.h"
#include "fake_timers.h"

using namespace l2m;

namespace {

constexpr double kDelay = 120000;

struct Harness {
  FakeTimers timers;
  int resetCalls = 0;
  bool busy = false;
  double delayMs = kDelay;
  bool throwOnReset = false;
  IdleReset idle;

  Harness()
    : idle(timers, IdleReset::Deps{
                     [this] { return delayMs; },
                     [this] { return busy; },
                     [this] {
                       resetCalls++;
                       if (throwOnReset) throw std::runtime_error("boom");
                     },
                   }) {}
};

}  // namespace

class TestIdleReset : public QObject {
  Q_OBJECT

private slots:
  // touch 之後過了設定的時間就復原
  void firesAfterDelay() {
    Harness h;
    h.idle.touch();
    h.timers.advance(kDelay);
    QCOMPARE(h.resetCalls, 1);
  }

  // 沒有 touch 過就不會自己跑起來
  void doesNotStartByItself() {
    Harness h;
    h.timers.advance(kDelay * 3);
    QCOMPARE(h.resetCalls, 0);
  }

  // 期間再 touch 一次就重新倒數，不會提早清掉
  void retouchRestartsCountdown() {
    Harness h;
    h.idle.touch();
    h.timers.advance(kDelay - 1);
    h.idle.touch();
    h.timers.advance(kDelay - 1);
    QCOMPARE(h.resetCalls, 0);
    h.timers.advance(1);
    QCOMPARE(h.resetCalls, 1);
  }

  // 時間到但還在忙（說話中／排程未跑）就延後，不會把話講到一半的表情抽掉
  void deferredWhileBusy() {
    Harness h;
    h.busy = true;
    h.idle.touch();
    h.timers.advance(kDelay * 3);
    QCOMPARE(h.resetCalls, 0);

    h.busy = false;
    h.timers.advance(kDelay);
    QCOMPARE(h.resetCalls, 1);
  }

  // delayMs 回 0 代表停用，touch 也不會排程
  void zeroDelayDisables() {
    Harness h;
    h.delayMs = 0;
    h.idle.touch();
    QCOMPARE(h.idle.pending(), false);
    h.timers.advance(kDelay * 3);
    QCOMPARE(h.resetCalls, 0);
  }

  // stop() 取消倒數
  void stopCancels() {
    Harness h;
    h.idle.touch();
    h.idle.stop();
    QCOMPARE(h.idle.pending(), false);
    h.timers.advance(kDelay * 2);
    QCOMPARE(h.resetCalls, 0);
  }

  // 復原之後不再自己重排，要等下一次活動
  void doesNotRescheduleAfterFiring() {
    Harness h;
    h.idle.touch();
    h.timers.advance(kDelay);
    QCOMPARE(h.idle.pending(), false);
    h.timers.advance(kDelay * 3);
    QCOMPARE(h.resetCalls, 1);
  }

  // reset 丟例外不會炸掉整個 app，也不會卡住下一輪
  void resetThrowIsContained() {
    Harness h;
    h.throwOnReset = true;
    h.idle.touch();
    h.timers.advance(kDelay);
    QCOMPARE(h.resetCalls, 1);
    h.idle.touch();
    h.timers.advance(kDelay);
    QCOMPARE(h.resetCalls, 2);
  }
};

QTEST_APPLESS_MAIN(TestIdleReset)
#include "test_idle_reset.moc"
