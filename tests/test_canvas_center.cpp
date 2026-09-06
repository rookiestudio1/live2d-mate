// moc3 畫布中心相對模型原點的位移（core/canvas_center.h）
#include <QtTest>

#include <limits>

#include "core/canvas_center.h"

using namespace l2m;

class TestCanvasCenter : public QObject {
  Q_OBJECT

private slots:
  // 原點就在畫布正中央（手邊 23 個 moc3 有 19 個是這樣）：位移必須是**精確的 0**，
  // 不是「很接近 0」—— 漏出浮點誤差就是每一隻模型的構圖都跟改動前差一點點
  void centeredOriginIsExactlyZero() {
    // Frieren：5167×9410、原點 (2583.5, 4705)、ppu 5167
    const CanvasOffset offset = canvasCenterOffset(5167, 9410, 2583.5, 4705, 5167);
    QCOMPARE(offset.x, 0.0);
    QCOMPARE(offset.y, 0.0);
  }

  // 原點在腳底（畫布下緣附近）：畫布中心在原點**上方**，位移為正 ——
  // draw() 會拿它把模型往下推，上半身才不會跑到視窗外面。
  // 300301_hujisheng：3508×4961、原點 (1754, 4663.34)、ppu 201.9129
  void originAtFeetPushesCanvasCenterUp() {
    const CanvasOffset offset = canvasCenterOffset(3508, 4961, 1754, 4663.34, 201.9129);
    QCOMPARE(offset.x, 0.0);
    QVERIFY(offset.y > 0);
    QVERIFY(qAbs(offset.y - 10.81) < 0.01);
  }

  // 原點在頭頂附近：畫布中心在原點**下方**，位移為負。
  // 這一條專門釘住正負號 —— 只測上面那一隻的話，把 Y 的方向寫反也會通過
  //（兩者只差正負，絕對值都對）。lumine：5000×8000、原點 (2500, 1280)、ppu 5000
  void originAtHeadPushesCanvasCenterDown() {
    const CanvasOffset offset = canvasCenterOffset(5000, 8000, 2500, 1280, 5000);
    QCOMPARE(offset.x, 0.0);
    QVERIFY(offset.y < 0);
    QVERIFY(qAbs(offset.y - (-0.544)) < 0.0001);
  }

  // 原點偏左：X 與像素同向，畫布中心在原點右邊 → 位移為正
  void originLeftOfCentreShiftsRight() {
    const CanvasOffset offset = canvasCenterOffset(1000, 1000, 250, 500, 100);
    QCOMPARE(offset.x, 2.5);
    QCOMPARE(offset.y, 0.0);
  }

  // 讀不到 canvas info 時 pixelsPerUnit 會是 0 —— 不能除下去，
  // 一律回 {0,0}（維持原樣至少跟改動前一模一樣）
  void invalidPixelsPerUnitFallsBackToZero() {
    QCOMPARE(canvasCenterOffset(3508, 4961, 1754, 4663, 0).y, 0.0);
    QCOMPARE(canvasCenterOffset(3508, 4961, 1754, 4663, -1).y, 0.0);
    QCOMPARE(canvasCenterOffset(3508, 4961, 1754, 4663, std::numeric_limits<double>::quiet_NaN()).y, 0.0);
  }

  // 尺寸或原點是 NaN／Inf 也一律放棄 —— 算下去會把平移寫成 NaN，
  // 而 NaN 進了矩陣是整隻模型當場消失，比擺歪難查得多
  void nonFiniteInputsFallBackToZero() {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();
    QCOMPARE(canvasCenterOffset(nan, 4961, 1754, 4663, 201.9).x, 0.0);
    QCOMPARE(canvasCenterOffset(3508, inf, 1754, 4663, 201.9).y, 0.0);
    QCOMPARE(canvasCenterOffset(3508, 4961, nan, 4663, 201.9).x, 0.0);
    QCOMPARE(canvasCenterOffset(3508, 4961, 1754, inf, 201.9).y, 0.0);
  }
};

QTEST_APPLESS_MAIN(TestCanvasCenter)
#include "test_canvas_center.moc"
