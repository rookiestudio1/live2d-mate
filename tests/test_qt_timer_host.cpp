// QtTimerHost：TimerHost 的 production 實作。
//
// 這一層自己管 QTimer 的生命週期與重入，必須驗。
// 重點在「回呼裡取消自己 / 排下一輪」不能互相踩到 —— IdleReset 與
// IdlePerformer 兩邊都會這樣用。
#include <QtTest>

#include <functional>
#include <vector>

#include "core/idle.h"
#include "core/qt_timer_host.h"

using namespace l2m;

class TestQtTimerHost : public QObject {
  Q_OBJECT

private slots:
  // 依到期時間先後觸發
  void firesInOrder() {
    QtTimerHost host;
    std::vector<int> fired;
    host.setTimeout([&fired] { fired.push_back(2); }, 60);
    host.setTimeout([&fired] { fired.push_back(1); }, 20);

    QTRY_COMPARE_WITH_TIMEOUT(fired.size(), size_t(2), 2000);
    QCOMPARE(fired, (std::vector<int>{1, 2}));
    QCOMPARE(host.pendingCount(), size_t(0));
  }

  // clearTimeout 之後不再觸發
  void clearedTimerNeverFires() {
    QtTimerHost host;
    bool fired = false;
    const int id = host.setTimeout([&fired] { fired = true; }, 20);
    host.clearTimeout(id);
    QCOMPARE(host.pendingCount(), size_t(0));

    QTest::qWait(80);
    QVERIFY(!fired);
  }

  // 在回呼裡取消自己：不能當掉，也不能影響同時排的其他計時器
  void clearSelfInsideCallback() {
    QtTimerHost host;
    int selfId = -1;
    bool selfFired = false;
    bool otherFired = false;
    selfId = host.setTimeout(
      [&host, &selfId, &selfFired] {
        selfFired = true;
        host.clearTimeout(selfId);  // IdleReset::fire → stop() 就是這個形狀
      },
      20);
    host.setTimeout([&otherFired] { otherFired = true; }, 40);

    QTRY_VERIFY_WITH_TIMEOUT(selfFired && otherFired, 2000);
    QCOMPARE(host.pendingCount(), size_t(0));
  }

  // 在回呼裡排下一輪：新排的計時器不能被自己的清理連坐刪掉
  void rescheduleInsideCallback() {
    QtTimerHost host;
    int rounds = 0;
    std::function<void()> tick = [&] {
      if (++rounds < 3) host.setTimeout(tick, 10);
    };
    host.setTimeout(tick, 10);

    QTRY_COMPARE_WITH_TIMEOUT(rounds, 3, 2000);
  }

  // IdleReset 接上真計時器後的行為：touch 會重新倒數，時間到才復原
  void idleResetTouchRestartsCountdown() {
    QtTimerHost host;
    int resets = 0;
    IdleReset::Deps deps;
    deps.delayMs = [] { return 60.0; };
    deps.isBusy = [] { return false; };
    deps.reset = [&resets] { ++resets; };
    IdleReset idle(host, std::move(deps));

    idle.touch();
    QVERIFY(idle.pending());
    QTest::qWait(30);
    idle.touch();  // 還沒到期就重新倒數
    QTest::qWait(40);
    QCOMPARE(resets, 0);

    QTRY_COMPARE_WITH_TIMEOUT(resets, 1, 2000);
    QVERIFY(!idle.pending());
  }
};

QTEST_GUILESS_MAIN(TestQtTimerHost)
#include "test_qt_timer_host.moc"
