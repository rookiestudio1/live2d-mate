// splash 畫面的兩個時間函式，見 core/splash_timing.h。
#include <QtTest>

#include <cmath>

#include "core/splash_timing.h"

using namespace l2m;

namespace {

// 一條 480px 寬的軌道、28% 的光條、1200ms 一圈 —— 與 splash_window.cpp 的預設一致
constexpr double kTrack = 480.0;
constexpr double kSegment = kTrack * 0.28;
constexpr double kCycle = 1200.0;

constexpr double kEps = 1e-9;

bool near(double a, double b) { return std::abs(a - b) < 1e-6; }

}  // namespace

class TestSplashTiming : public QObject {
  Q_OBJECT

private slots:
  // 一圈的起點：光條完全在軌道左側之外，看不到任何一格
  void sweepStartsFullyOffScreenLeft() { QVERIFY(near(splashSweepOffset(0.0, kTrack, kSegment, kCycle), -kSegment)); }

  // 一圈的終點：光條完全滑出軌道右側才回頭（左緣剛好等於軌道寬）
  void sweepEndsFullyOffScreenRight() {
    const double end = splashSweepOffset(kCycle - kEps, kTrack, kSegment, kCycle);
    QVERIFY(end > kTrack - 1e-6);
    QVERIFY(end <= kTrack + 1e-6);
  }

  // 每個 cycle 迴繞一次，所以相隔整數圈的時間點位置相同
  void sweepWrapsEveryCycle() {
    for (const double t : {0.0, 137.0, 600.0, 1199.0}) {
      const double a = splashSweepOffset(t, kTrack, kSegment, kCycle);
      const double b = splashSweepOffset(t + kCycle * 3, kTrack, kSegment, kCycle);
      QVERIFY(near(a, b));
    }
  }

  // 單一圈內單調遞增 —— 光條只會往前跑，不會抖回去
  void sweepIsMonotonicWithinOneCycle() {
    double previous = -1e9;
    for (int i = 0; i <= 100; ++i) {
      const double t = kCycle * i / 100.0 - (i == 100 ? kEps : 0.0);
      const double x = splashSweepOffset(t, kTrack, kSegment, kCycle);
      QVERIFY(x > previous);
      previous = x;
    }
  }

  // smoothstep：中點剛好在行程正中央，兩端比等速慢
  void sweepIsSmoothstepShaped() {
    const double span = kTrack + kSegment;
    QVERIFY(near(splashSweepOffset(kCycle / 2, kTrack, kSegment, kCycle), -kSegment + span / 2));
    // 四分之一圈時只走了 1/4 * 1/4 * (3 - 1/2) = 0.15625 的行程（等速會是 0.25）
    const double quarter = splashSweepOffset(kCycle / 4, kTrack, kSegment, kCycle);
    QVERIFY(quarter < -kSegment + span * 0.25);
  }

  // cycleMs 是 0（常數被設壞、或時鐘沒 start）時回起始位置，不是 NaN
  void sweepSurvivesZeroCycle() {
    const double x = splashSweepOffset(500.0, kTrack, kSegment, 0.0);
    QVERIFY(!std::isnan(x));
    QVERIFY(near(x, -kSegment));
    QVERIFY(near(splashSweepOffset(500.0, kTrack, kSegment, -1.0), -kSegment));
  }

  // elapsed 是負的（QElapsedTimer 沒 start 過）時仍落在合法區間內
  void sweepSurvivesNegativeElapsed() {
    const double x = splashSweepOffset(-300.0, kTrack, kSegment, kCycle);
    QVERIFY(!std::isnan(x));
    QVERIFY(x >= -kSegment - 1e-6);
    QVERIFY(x <= kTrack + 1e-6);
  }

  // 最短顯示時間過了就不必再等
  void holdIsZeroOnceMinVisiblePassed() {
    QVERIFY(near(splashRemainingHoldMs(800.0, 800.0), 0.0));
    QVERIFY(near(splashRemainingHoldMs(5000.0, 800.0), 0.0));
  }

  // 淡入的時間算在最短顯示時間裡：剛淡完（300ms）還要再撐 500ms
  void holdCountsFadeInTime() { QVERIFY(near(splashRemainingHoldMs(300.0, 800.0), 500.0)); }

  // 最短顯示時間設 0 時永遠不等
  void holdIsZeroWhenMinVisibleDisabled() { QVERIFY(near(splashRemainingHoldMs(0.0, 0.0), 0.0)); }
};

QTEST_APPLESS_MAIN(TestSplashTiming)
#include "test_splash_timing.moc"
