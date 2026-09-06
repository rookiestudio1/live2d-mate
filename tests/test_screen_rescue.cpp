// 螢幕熱插拔救援：拔掉螢幕之後，留在死座標空間裡的視窗要被撈回可見處。
#include <QtTest>

#include "core/screen_rescue.h"

using namespace l2m;

namespace {

// 主螢幕 1920×1040（工作區）＋ 右側外接 1920×1080
const ScreenRect kPrimary{0, 0, 1920, 1040};
const ScreenRect kRight{1920, 0, 1920, 1080};
const ScreenRect kWindow{2500, 300, 400, 600};  // 在右螢幕上的桌寵

}  // namespace

class TestScreenRescue : public QObject {
  Q_OBJECT

private slots:
  // 視窗在某個工作區裡 → 看得見、不必救
  void visibleWindowNeedsNoRescue() {
    QCOMPARE(windowReachable(kWindow, {kPrimary, kRight}), true);
    QCOMPARE(rescuePosition(kWindow, {kPrimary, kRight}), std::nullopt);
  }

  // 拔掉右螢幕：視窗整個在工作區外 → 救回主螢幕並 clamp
  void offscreenWindowIsRescued() {
    QCOMPARE(windowReachable(kWindow, {kPrimary}), false);
    const auto rescued = rescuePosition(kWindow, {kPrimary});
    QVERIFY(rescued.has_value());
    const auto [x, y] = *rescued;
    QCOMPARE(x, 1920 - 400);  // clamp 到右緣
    QCOMPARE(y, 300);
    // 救回來的位置自己要是可見的
    QCOMPARE(windowReachable({x, y, kWindow.w, kWindow.h}, {kPrimary}), true);
  }

  // 只露出一小條邊不算搆得到（點不到就等於看不見）
  void slightOverlapIsNotReachable() {
    const ScreenRect barely{1920 - 10, 300, 400, 600};  // 只有 10px 在主螢幕內
    QCOMPARE(windowReachable(barely, {kPrimary}), false);
    QVERIFY(rescuePosition(barely, {kPrimary}).has_value());
  }

  // 多螢幕時救去中心距離最近的那一顆
  void rescueGoesToTheNearestScreen() {
    const ScreenRect farLeft{-4000, 300, 400, 600};
    const auto rescued = rescuePosition(farLeft, {kPrimary, kRight});
    QVERIFY(rescued.has_value());
    QCOMPARE(rescued->first, 0);  // clamp 進主螢幕（比右螢幕近）
  }

  // 一個螢幕都沒有：救無可救，別亂動
  void noScreensMeansNoMove() { QCOMPARE(rescuePosition(kWindow, {}), std::nullopt); }

  // 視窗比工作區還大：貼齊左上，至少頭看得到
  void oversizedWindowClampsToOrigin() {
    const ScreenRect tiny{0, 0, 300, 400};
    const ScreenRect huge{5000, 5000, 800, 900};
    const auto rescued = rescuePosition(huge, {tiny});
    QVERIFY(rescued.has_value());
    QCOMPARE(*rescued, std::make_pair(0, 0));
  }
};

QTEST_GUILESS_MAIN(TestScreenRescue)
#include "test_screen_rescue.moc"
