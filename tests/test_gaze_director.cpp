// 視線追蹤權重的狀態機（core/gaze_director.h）。
// 重點有三塊：① holdMs 的邊界（游標靜止多久才開始回正）；② 四個理由
// （總開關、釘選、說話、靜止）匯進同一個權重之後的優先序；③ 收尾寫入 ——
// 少寫那一次，focus 會停在歸零前的殘值上，畫面上是頭永遠歪一點點。
// 前四條情境是從已移除的 test_speaking_gaze.cpp 移植過來的等價案例。
#include <QtTest>

#include "core/gaze_director.h"

using namespace l2m;

namespace {

// 與 GazeDirectorTuning 的預設值一致，邊界斷言都以它為準
constexpr double kHold = 3000;
constexpr double kEngage = 500;
constexpr double kRelease = 2000;
// production 的游標輪詢間隔（app_controller.cpp 的 kCursorIntervalMs）
constexpr double kTick = 40;
// 「走完整段回正」與「靜止到超過 holdMs」各需要幾輪。寫死輪數的話，
// 手感常數一改（1400→2000 ms 就發生過）測試會在不相干的地方紅一片。
constexpr int kReleaseTicks = static_cast<int>(kRelease / kTick) + 1;
constexpr int kHoldTicks = static_cast<int>(kHold / kTick) + 1;

struct Driver {
  GazeDirector gaze;
  double nowMs = 0;

  GazeOutput step(double dtMs, bool moved, bool enabled = true, bool pinned = false, bool suppressed = false) {
    nowMs += dtMs;
    GazeInput in;
    in.nowMs = nowMs;
    in.enabled = enabled;
    in.pinned = pinned;
    in.suppressed = suppressed;
    in.cursorMoved = moved;
    return gaze.tick(in);
  }

  // 滑鼠持續移動直到權重滿檔（第一輪 dt 為 0，所以多推幾輪）
  void engageFully() {
    step(0, true);
    for (int i = 0; i < 20; ++i) step(kTick, true);
  }
};

}  // namespace

class TestGazeDirector : public QObject {
  Q_OBJECT

private slots:
  // 剛啟動、滑鼠沒動：把 focus 擺正寫一次，之後閉嘴
  void startsFacingFrontAndThenGoesQuiet() {
    Driver d;
    const GazeOutput first = d.step(0, false);
    QVERIFY(first.write);
    QCOMPARE(first.weight, 0.0);
    QVERIFY(!d.step(kTick, false).write);
  }

  // 滑鼠一動就轉過去，但是漸進的 —— 不是一輪就到位
  void engagesGraduallyWhenCursorMoves() {
    Driver d;
    d.step(0, true);
    const GazeOutput out = d.step(kTick, true);
    QVERIFY(out.write);
    QVERIFY(out.weight > 0);
    QVERIFY(out.weight < 1);
    for (int i = 0; i < 20; ++i) d.step(kTick, true);
    QCOMPARE(d.gaze.weight(), 1.0);
  }

  // holdMs 邊界卡死：差 1 ms 仍在追，跨過去才開始回正
  void holdBoundaryIsExact() {
    Driver d;
    d.engageFully();
    QCOMPARE(d.gaze.weight(), 1.0);

    const double edge = d.nowMs + kHold;
    while (d.nowMs + kTick <= edge - 1) d.step(kTick, false);
    d.step(edge - 1 - d.nowMs, false);
    QCOMPARE(d.gaze.weight(), 1.0);

    d.step(1, false);
    QVERIFY(d.gaze.weight() < 1.0);
  }

  // 被吸引該快、失去興趣該慢：同樣的 dt，上升走的距離大於下降
  void releaseIsSlowerThanEngage() {
    Driver up;
    up.step(0, true);
    up.step(100, true);
    const double rose = up.gaze.weight();

    Driver down;
    down.engageFully();
    const double edge = down.nowMs + kHold;
    while (down.nowMs + kTick <= edge) down.step(kTick, false);
    down.step(edge - down.nowMs, false);
    const double before = down.gaze.weight();
    down.step(100, false);
    const double fell = before - down.gaze.weight();

    QCOMPARE(rose, 100.0 / kEngage);
    QCOMPARE(fell, 100.0 / kRelease);
    QVERIFY(fell < rose);
  }

  // 說話期間正視前方：權重降到 0；說完滑鼠還在動就追回去
  void speechReleasesGazeAndComesBack() {
    Driver d;
    d.engageFully();
    for (int i = 0; i < kReleaseTicks; ++i) d.step(kTick, true, true, false, true);
    QCOMPARE(d.gaze.weight(), 0.0);

    const GazeOutput out = d.step(kTick, true);
    QVERIFY(out.write);
    QVERIFY(out.weight > 0);
  }

  // 說完話時滑鼠早就不動了 → 留在正面。舊版是硬跳回游標，那時游標可能離開很久了
  void speechEndWithStillCursorStaysFront() {
    Driver d;
    d.engageFully();
    for (int i = 0; i < kHoldTicks; ++i) d.step(kTick, false, true, false, true);
    QCOMPARE(d.gaze.weight(), 0.0);
    QVERIFY(!d.step(kTick, false).write);
    QCOMPARE(d.gaze.weight(), 0.0);
  }

  // 釘選期間一個位元組都不能寫 —— 25 Hz 的輪詢只要寫一次就把 look_at 抹掉
  void pinnedNeverWrites() {
    Driver d;
    d.engageFully();
    const GazeOutput out = d.step(kTick, true, true, true);
    QVERIFY(!out.write);
    QCOMPARE(d.gaze.weight(), 0.0);
    for (int i = 0; i < 20; ++i) QVERIFY(!d.step(kTick, true, true, true).write);
  }

  // 解除釘選時滑鼠靜止 → 停在正面；要重新動過才追出去
  void unpinNeedsFreshMovement() {
    Driver d;
    d.step(0, true);
    d.step(kTick, true, true, true);
    d.step(kTick, false);
    QCOMPARE(d.gaze.weight(), 0.0);
    d.step(kTick, true);
    QVERIFY(d.gaze.weight() > 0);
  }

  // 總開關關著：滑鼠怎麼動都不追
  void disabledNeverTracks() {
    Driver d;
    for (int i = 0; i < 30; ++i) d.step(kTick, true, false);
    QCOMPARE(d.gaze.weight(), 0.0);
  }

  // 追蹤中把開關關掉：平滑回正而不是卡在最後一個位置（順手修好的行為）
  void disablingMidTrackReturnsToFront() {
    Driver d;
    d.engageFully();
    QCOMPARE(d.gaze.weight(), 1.0);
    for (int i = 0; i < kReleaseTicks; ++i) d.step(kTick, true, false);
    QCOMPARE(d.gaze.weight(), 0.0);
    QVERIFY(!d.step(kTick, true, false).write);
  }

  // 收尾那一次一定要寫，否則 focus 停在歸零前的殘值上
  void settleWritesOnceThenStops() {
    Driver d;
    d.engageFully();
    GazeOutput out;
    for (int i = 0; i < 200 && d.gaze.weight() > 0; ++i) out = d.step(kTick, false);
    QCOMPARE(d.gaze.weight(), 0.0);
    QVERIFY(out.write);
    QCOMPARE(out.weight, 0.0);
    QVERIFY(!d.step(kTick, false).write);
  }

  // 休眠喚醒之後 nowMs 一次跳好幾秒，不夾住就會在一輪內走完整條曲線
  void hugeTimeGapDoesNotSnap() {
    Driver d;
    d.step(0, true);
    d.step(10000, true);
    QVERIFY(d.gaze.weight() < 1.0);
    QCOMPARE(d.gaze.weight(), 250.0 / kEngage);
  }

  // 輸出過 smoothstep：起步被壓扁（兩端慢），中點原封不動
  void outputIsSmoothstepped() {
    Driver slow;
    slow.step(0, true);
    const GazeOutput low = slow.step(0.2 * kEngage, true);
    QCOMPARE(slow.gaze.weight(), 0.2);
    QVERIFY(low.weight < slow.gaze.weight());

    Driver half;
    half.step(0, true);
    const GazeOutput mid = half.step(0.5 * kEngage, true);
    QCOMPARE(half.gaze.weight(), 0.5);
    QCOMPARE(mid.weight, 0.5);
  }

  // reset() 之後從正面重來，那段空白不算成一次巨大的 dt
  void resetStartsOverFromFront() {
    Driver d;
    d.engageFully();
    QCOMPARE(d.gaze.weight(), 1.0);
    d.gaze.reset();
    QCOMPARE(d.gaze.weight(), 0.0);
    d.nowMs += 600000;
    const GazeOutput out = d.step(kTick, true);
    QCOMPARE(d.gaze.weight(), 0.0);
    QVERIFY(out.write);
    QCOMPARE(out.weight, 0.0);
  }
};

QTEST_APPLESS_MAIN(TestGazeDirector)
#include "test_gaze_director.moc"
