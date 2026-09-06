// IdlePerformer：閒置多久後開始自動表演、間隔重複、忙碌時跳過、設定熱更新
#include <QtTest>

#include <stdexcept>

#include "core/idle.h"
#include "fake_timers.h"

using namespace l2m;

namespace {

constexpr double kIdle = 180000;
constexpr double kInterval = 60000;

struct Harness {
  FakeTimers timers;
  int performCalls = 0;
  bool busy = false;
  double idleMs = kIdle;
  double intervalMs = kInterval;
  bool throwOnPerform = false;
  IdlePerformer performer;

  Harness()
    : performer(timers, IdlePerformer::Deps{
                          [this] { return idleMs; },
                          [this] { return intervalMs; },
                          [this] { return busy; },
                          [this] {
                            performCalls++;
                            if (throwOnPerform) throw std::runtime_error("boom");
                          },
                        }) {}
};

}  // namespace

class TestIdlePerformer : public QObject {
  Q_OBJECT

private slots:
  // touch 之後閒置滿 idleMs 就表演第一次
  void firstPerformAfterIdle() {
    Harness h;
    h.performer.touch();
    h.timers.advance(kIdle - 1);
    QCOMPARE(h.performCalls, 0);
    h.timers.advance(1);
    QCOMPARE(h.performCalls, 1);
    QCOMPARE(h.performer.state(), IdlePerformer::State::Performing);
  }

  // 進入表演後每隔 intervalMs 重複，不會只演一次
  void repeatsAtInterval() {
    Harness h;
    h.performer.touch();
    h.timers.advance(kIdle + kInterval * 2);
    QCOMPARE(h.performCalls, 3);
  }

  // 沒有 touch 過就不會自己跑起來
  void doesNotStartByItself() {
    Harness h;
    h.timers.advance(kIdle * 3);
    QCOMPARE(h.performCalls, 0);
  }

  // 表演中 touch 就離開表演，要重新閒置滿 idleMs 才再開始
  void touchLeavesPerforming() {
    Harness h;
    h.performer.touch();
    h.timers.advance(kIdle);
    QCOMPARE(h.performCalls, 1);

    h.performer.touch();
    QCOMPARE(h.performer.state(), IdlePerformer::State::Waiting);
    h.timers.advance(kInterval * 2);
    QCOMPARE(h.performCalls, 1);
    h.timers.advance(kIdle - kInterval * 2);
    QCOMPARE(h.performCalls, 2);
  }

  // 忙碌時跳過該輪但不退出表演，下一輪照演（與 IdleReset 的整段重排不同）
  void busySkipsRoundButStaysPerforming() {
    Harness h;
    h.performer.touch();
    h.busy = true;
    h.timers.advance(kIdle);
    QCOMPARE(h.performCalls, 0);
    QCOMPARE(h.performer.state(), IdlePerformer::State::Performing);

    h.busy = false;
    h.timers.advance(kInterval);
    QCOMPARE(h.performCalls, 1);
  }

  // idleMs 回 0 代表停用，touch 也不會排程
  void zeroIdleDisables() {
    Harness h;
    h.idleMs = 0;
    h.performer.touch();
    QCOMPARE(h.performer.state(), IdlePerformer::State::Stopped);
    h.timers.advance(kIdle * 3);
    QCOMPARE(h.performCalls, 0);
  }

  // stop() 在倒數中與表演中都能取消
  void stopWorksInBothStates() {
    Harness waiting;
    waiting.performer.touch();
    waiting.performer.stop();
    waiting.timers.advance(kIdle * 2);
    QCOMPARE(waiting.performCalls, 0);

    Harness performing;
    performing.performer.touch();
    performing.timers.advance(kIdle);
    QCOMPARE(performing.performCalls, 1);
    performing.performer.stop();
    QCOMPARE(performing.performer.state(), IdlePerformer::State::Stopped);
    performing.timers.advance(kInterval * 3);
    QCOMPARE(performing.performCalls, 1);
  }

  // perform 丟例外不會炸掉整個 app，下一輪照跑
  void performThrowIsContained() {
    Harness h;
    h.throwOnPerform = true;
    h.performer.touch();
    h.timers.advance(kIdle);
    QCOMPARE(h.performCalls, 1);
    h.timers.advance(kInterval);
    QCOMPARE(h.performCalls, 2);
  }

  // intervalMs 每輪重讀，中途改設定下一輪就用新間隔
  void intervalHotReloaded() {
    Harness h;
    h.performer.touch();
    h.timers.advance(kIdle);
    QCOMPARE(h.performCalls, 1);

    h.intervalMs = kInterval * 2;
    h.timers.advance(kInterval);
    QCOMPARE(h.performCalls, 2);
    // 這一輪起用新間隔：舊間隔到點不觸發，滿新間隔才觸發
    h.timers.advance(kInterval);
    QCOMPARE(h.performCalls, 2);
    h.timers.advance(kInterval);
    QCOMPARE(h.performCalls, 3);
  }

  // refresh()：倒數中重排新秒數，設定停用時立即停（含表演中）
  void refreshAppliesNewSettings() {
    Harness h;
    h.performer.touch();
    h.timers.advance(kIdle - 1);
    h.idleMs = kIdle * 2;
    h.performer.refresh();
    h.timers.advance(kIdle * 2 - 1);
    QCOMPARE(h.performCalls, 0);
    h.timers.advance(1);
    QCOMPARE(h.performCalls, 1);

    // 表演中被停用
    h.idleMs = 0;
    h.performer.refresh();
    QCOMPARE(h.performer.state(), IdlePerformer::State::Stopped);
    h.timers.advance(kInterval * 3);
    QCOMPARE(h.performCalls, 1);
  }
};

QTEST_APPLESS_MAIN(TestIdlePerformer)
#include "test_idle_performer.moc"
