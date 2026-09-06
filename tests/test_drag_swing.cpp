// 拖曳搖晃濾波的單元測試（core/drag_swing.h）
#include <QtTest>

#include <cmath>

#include "core/drag_swing.h"

using namespace l2m;

namespace {

// 以固定間隔連續餵等速位移，回傳最後一筆樣本的時間
double feedSteady(DragSwing& swing, double dx, double dy, double startMs, int count, double stepMs = 16) {
  double t = startMs;
  for (int i = 0; i < count; ++i) {
    t = startMs + i * stepMs;
    swing.addSample(dx, dy, t);
  }
  return t;
}

}  // namespace

class TestDragSwing : public QObject {
  Q_OBJECT

private slots:
  // 沒餵過樣本就取樣：inactive、全零
  void noSamplesStaysInactive() {
    DragSwing swing;
    const auto out = swing.sample(1000);
    QVERIFY(!out.active);
    QCOMPARE(out.angleXDeg, 0.0);
    QCOMPARE(out.angleYDeg, 0.0);
    QCOMPARE(out.angleZDeg, 0.0);
    QCOMPARE(out.bodyXDeg, 0.0);
    QCOMPARE(out.windX, 0.0);
    QCOMPARE(out.windY, 0.0);
  }

  // 以 16ms 間隔等速 1000px/s 往右餵 500ms，angleX 收斂到 -1000 * 0.012 = -12 度。
  // 風力期望值由調校常數導出 —— windPerPxPerSec 是實機反覆調的值，
  // 硬編死數字會在每一輪調參後把測試弄壞（踩過一次）
  void steadyDragConvergesToExpectedAngle() {
    DragSwing swing;
    const double last = feedSteady(swing, 16, 0, 0, 32);
    const auto out = swing.sample(last);
    QVERIFY(out.active);
    QVERIFY(qAbs(out.angleXDeg - (-12.0)) < 0.5);
    const double expectedWind = 1000 * DragSwingTuning{}.windPerPxPerSec;
    QVERIFY(qAbs(out.windX - expectedWind) < expectedWind * 0.05);
  }

  // 停止餵樣本後衰減：200ms 時剩約 1/3，800ms 時降到死區以下且 inactive
  //（放開滑鼠沒有任何事件也會歸零 —— 不需要拖曳結束通知的核心保證）
  void outputDecaysToZeroAfterRelease() {
    DragSwing swing;
    const double last = feedSteady(swing, 16, 0, 0, 32);
    const auto mid = swing.sample(last + 200);
    QVERIFY(mid.active);
    QVERIFY(mid.angleXDeg > -5.0 && mid.angleXDeg < -3.0);
    const auto end = swing.sample(last + 800);
    QVERIFY(!end.active);
    QCOMPARE(end.angleXDeg, 0.0);
  }

  // 單發巨量位移：速度模長夾在 maxSpeedPxPerSec 後，角度停在自己的貢獻上限；
  // 風力在目前的調校下由速度上限決定（2500 × windPerPxPerSec 遠小於 windMax，
  // windMax 只是保險絲），期望值同樣由常數導出
  void hugeVelocityIsClamped() {
    DragSwing swing;
    swing.addSample(500, 0, 0);
    const auto out = swing.sample(0);
    QVERIFY(out.active);
    QCOMPARE(out.angleXDeg, -20.0);
    const DragSwingTuning tuning;
    QCOMPARE(out.windX, std::min(tuning.maxSpeedPxPerSec * tuning.windPerPxPerSec, tuning.windMax));
  }

  // 貼螢幕邊情境：視窗被 clamp 住時餵進來的是 (0,0)，輸出要快速歸零
  void zeroDisplacementSamplesKillSwing() {
    DragSwing swing;
    const double last = feedSteady(swing, 16, 0, 0, 32);
    const double stopped = feedSteady(swing, 0, 0, last + 16, 16);
    const auto out = swing.sample(stopped);
    QVERIFY(!out.active);
  }

  // 同一 nowMs 重複呼叫：不產生 NaN／爆值，sample 冪等
  void duplicateTimestampIsSafe() {
    DragSwing swing;
    swing.addSample(10, 0, 100);
    swing.addSample(10, 0, 100);
    const auto first = swing.sample(100);
    const auto second = swing.sample(100);
    QVERIFY(std::isfinite(first.angleXDeg));
    QVERIFY(std::isfinite(first.windX));
    QCOMPARE(first.angleXDeg, second.angleXDeg);
    QCOMPARE(first.windX, second.windX);
  }

  // 間隔 1 秒後的新樣本是新一輪拖曳：舊的向右高速不會混進新的向左低速
  void longGapStartsFresh() {
    DragSwing swing;
    feedSteady(swing, 32, 0, 0, 32);  // 約 2000px/s 往右
    swing.addSample(-8, 0, 1500);     // 停 1 秒後往左一小步
    const auto out = swing.sample(1500);
    QVERIFY(out.active);
    QVERIFY(out.angleXDeg > 0.0);
    QVERIFY(out.angleXDeg < 3.0);
  }

  // 釘住符號約定（實機調校定案）：頭身是慣性滯後 —— 往右甩 angleX/bodyX 為負、
  // angleZ 為正；風力（頭髮）與拖曳同向 —— 往右甩 windX 為正。
  // 往下甩（螢幕 y 向下）angleY 為正；windY 為負 —— rig 空間 Y 朝上，
  // 「同向」在垂直方向要翻軸
  void signConventions() {
    DragSwing right;
    const double lastRight = feedSteady(right, 16, 0, 0, 32);
    const auto r = right.sample(lastRight);
    QVERIFY(r.angleXDeg < 0.0);
    QVERIFY(r.bodyXDeg < 0.0);
    QVERIFY(r.windX > 0.0);
    QVERIFY(r.angleZDeg > 0.0);

    DragSwing down;
    const double lastDown = feedSteady(down, 0, 16, 0, 32);
    const auto d = down.sample(lastDown);
    QVERIFY(d.angleYDeg > 0.0);
    QVERIFY(d.windY < 0.0);
  }

  // 1px / 50ms 的緩慢挪動（20px/s）低於死區，完全不干擾模型
  void deadZoneSuppressesJitter() {
    DragSwing swing;
    const double last = feedSteady(swing, 1, 0, 0, 20, 50);
    const auto out = swing.sample(last);
    QVERIFY(!out.active);
  }

  // reset 之後回到初始狀態
  void resetClearsState() {
    DragSwing swing;
    const double last = feedSteady(swing, 16, 0, 0, 32);
    QVERIFY(swing.sample(last).active);
    swing.reset();
    QVERIFY(!swing.sample(last).active);
  }
};

QTEST_APPLESS_MAIN(TestDragSwing)
#include "test_drag_swing.moc"
