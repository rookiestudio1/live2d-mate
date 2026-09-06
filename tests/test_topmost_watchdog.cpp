// 置頂看門狗的判定（core/topmost_watchdog.h）：系統拔掉置頂之後，
// QWindow::setFlags() 的 early-out 會讓它回不來，所以多了這個看門狗，
// 也就多了這條要釘住的判定。
//
// 這裡要防的是未來有人「簡化」成 `return wantTopmost && !nativeTopmost;`
// 或乾脆每輪都無條件宣告 —— 前者會讓隱藏中的角色被拉回 z-order 頂端，
// 也會讓「旗標還在、人卻被壓在一般視窗底下」那個症狀永遠等不到救援；
// 後者會讓桌寵每 2 秒把自己插到 topmost 層最上面去跟別的置頂視窗搶位置。
#include <QtTest>

#include "core/topmost_watchdog.h"

using namespace l2m;

class TestTopmostWatchdog : public QObject {
  Q_OBJECT

private slots:
  // 症狀一：設定說要置頂、視窗看得見，但原生的 bit 被系統拔掉了
  void reassertsWhenNativeFlagWasStripped() { QVERIFY(shouldReassertTopmost(true, false, true, false)); }

  // 症狀二：bit 還在，人卻已經被排到一般視窗底下。這一格才是「開了小畫家之後
  // 桌寵就沉下去」的那個 bug —— 只看 bit 的話這裡會回 false，看門狗每 2 秒
  // 醒來一次都判定「沒事」，桌寵要等使用者自己點它一下才回得來
  void reassertsWhenBuriedWithFlagIntact() { QVERIFY(shouldReassertTopmost(true, true, true, true)); }

  // 已經置頂、也沒被壓住就不要再下 SetWindowPos —— 每次都下會把自己插到
  // topmost 層最上面，去跟工作管理員、輸入法候選窗那些同樣置頂的視窗搶位置
  void staysQuietWhenAlreadyTopmost() { QVERIFY(!shouldReassertTopmost(true, true, true, false)); }

  // 使用者關掉了「永遠置頂」，看門狗就不該把它拉回去 —— 兩個症狀都一樣
  void neverFightsTheUserSetting() {
    QVERIFY(!shouldReassertTopmost(false, false, true, false));
    QVERIFY(!shouldReassertTopmost(false, true, true, false));
    QVERIFY(!shouldReassertTopmost(false, true, true, true));
  }

  // 角色隱藏（或正在淡出）時不動 z-order；重新現身那條路自己會補一次
  void skipsWhileHidden() {
    QVERIFY(!shouldReassertTopmost(true, false, false, false));
    QVERIFY(!shouldReassertTopmost(true, true, false, false));
    QVERIFY(!shouldReassertTopmost(true, true, false, true));
  }

  // 檢查週期：太短是白花，太長使用者會看到桌寵沉在下面
  void checksAtATwoSecondCadence() { QCOMPARE(kTopmostCheckIntervalMs, 2000); }
};

QTEST_APPLESS_MAIN(TestTopmostWatchdog)
#include "test_topmost_watchdog.moc"
