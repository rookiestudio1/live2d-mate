// 在席偵測：Active/Away 與回座／休眠喚醒／久用事件。
// 兩個時鐘與閒置訊號全由測試注入，休眠偵測不必真的讓機器睡著。
#include <QtTest>

#include "core/idle_signal.h"
#include "core/presence_tracker.h"

using namespace l2m;

namespace {

using Kind = PresenceEvent::Kind;

// 選項縮小到測試友善的量級。resumeGapMs 必須**遠大於測試的取樣間隔**：
// 它的語意是「牆上時鐘一次跳這麼多＝機器睡過」，取樣間隔逼近它就會把正常的
// 心跳誤判成休眠（production 是 5 秒心跳對 5 分鐘 gap，天生安全）。
PresenceTracker::Options smallOptions() {
  PresenceTracker::Options options;
  options.awayAfterMs = 60000;        // 1 分沒輸入 ＝ 離座
  options.greetAfterAwayMs = 120000;  // 離開 2 分鐘才打招呼
  options.resumeGapMs = 600000;       // 牆上時鐘跳 10 分鐘 ＝ 睡過
  options.longSessionMs = 300000;     // 連續 5 分鐘 ＝ 久用
  return options;
}

}  // namespace

class TestPresenceTracker : public QObject {
  Q_OBJECT

private slots:
  // 第一次取樣只建基準，不發事件
  void firstSampleEmitsNothing() {
    PresenceTracker tracker(smallOptions());
    QCOMPARE(tracker.sample(0, 1000000, 0.0).kind, Kind::None);
    QCOMPARE(tracker.state(), PresenceState::Active);
  }

  // 平台拿不到閒置訊號 → 永遠 Active、永不發事件（行為退回沒有這個功能的樣子）
  void nulloptIdleStaysActiveForever() {
    PresenceTracker tracker(smallOptions());
    double wall = 0;
    for (int i = 0; i < 100; ++i) {
      const auto event = tracker.sample(i * 5000.0, wall += 5000, std::nullopt);
      QCOMPARE(event.kind, Kind::None);
      QCOMPARE(tracker.state(), PresenceState::Active);
    }
  }

  // 閒置超過 awayAfterMs → WentAway 一次，之後維持 Away 不重複發
  void idleGrowsIntoAway() {
    PresenceTracker tracker(smallOptions());
    tracker.sample(0, 0, 0.0);
    QCOMPARE(tracker.sample(5000, 5000, 5000.0).kind, Kind::None);
    QCOMPARE(tracker.sample(65000, 65000, 65000.0).kind, Kind::WentAway);
    QCOMPARE(tracker.state(), PresenceState::Away);
    QCOMPARE(tracker.sample(70000, 70000, 70000.0).kind, Kind::None);
    QCOMPARE(tracker.state(), PresenceState::Away);
  }

  // 離開夠久才回座 → CameBack 帶離開時長
  void longAwayThenReturnGreets() {
    PresenceTracker tracker(smallOptions());
    tracker.sample(0, 0, 0.0);
    tracker.sample(65000, 65000, 65000.0);  // WentAway（離開起點回推到 0）
    const auto event = tracker.sample(180000, 180000, 1000.0);
    QCOMPARE(event.kind, Kind::CameBack);
    QVERIFY(event.awayForMs >= 120000);
    QCOMPARE(tracker.state(), PresenceState::Active);
  }

  // 離開得不夠久（倒杯水）→ 默默轉回 Active，不打招呼
  void shortAwayReturnsSilently() {
    PresenceTracker tracker(smallOptions());
    tracker.sample(0, 0, 0.0);
    tracker.sample(65000, 65000, 65000.0);                    // WentAway
    const auto event = tracker.sample(80000, 80000, 1000.0);  // 離開共 80 秒 < 120 秒
    QCOMPARE(event.kind, Kind::None);
    QCOMPARE(tracker.state(), PresenceState::Active);
  }

  // 牆上時鐘跳躍 → Resumed（休眠期間 GetTickCount 停止，閒置訊號小也照樣要抓到）
  void wallClockJumpMeansResumed() {
    PresenceTracker tracker(smallOptions());
    tracker.sample(0, 0, 0.0);
    tracker.sample(5000, 5000, 2000.0);
    // 機器睡了一小時：單調鐘照走（QPC），牆上時鐘跳一大段，idle 只有幾秒
    const auto event = tracker.sample(10000, 3605000, 3000.0);
    QCOMPARE(event.kind, Kind::Resumed);
    QVERIFY(event.awayForMs >= 3600000 - 5000);
    QCOMPARE(tracker.state(), PresenceState::Active);
  }

  // 連續使用滿 longSessionMs → LongSession，之後重新起算再發
  void longSessionFiresAndRearms() {
    PresenceTracker tracker(smallOptions());
    double wall = 0;
    tracker.sample(0, 0, 0.0);
    int fired = 0;
    for (double t = 5000; t <= 620000; t += 5000) {
      const auto event = tracker.sample(t, wall = t, 1000.0);
      if (event.kind == Kind::LongSession) {
        ++fired;
        QVERIFY(event.sessionForMs >= 300000);
      }
    }
    QCOMPARE(fired, 2);  // 620 秒 ≈ 兩個 5 分鐘週期
  }

  // continuousActiveMs：Away 時為 0，回座後從頭起算
  void continuousActiveResetsOnAway() {
    PresenceTracker tracker(smallOptions());
    tracker.sample(0, 0, 0.0);
    tracker.sample(30000, 30000, 1000.0);
    QVERIFY(tracker.continuousActiveMs(30000) >= 29000);
    tracker.sample(95000, 95000, 95000.0);  // Away
    QCOMPARE(tracker.continuousActiveMs(95000), 0.0);
  }

  // === effectiveIdleMs ===

  // 分級吃兩個訊號取小；平台拿不到就退回 app 閒置
  void effectiveIdleTakesTheSmaller() {
    QCOMPARE(effectiveIdleMs(100000, 5000.0), 5000.0);
    QCOMPARE(effectiveIdleMs(5000, 100000.0), 5000.0);
    QCOMPARE(effectiveIdleMs(100000, std::nullopt), 100000.0);
  }
};

QTEST_GUILESS_MAIN(TestPresenceTracker)
#include "test_presence_tracker.moc"
