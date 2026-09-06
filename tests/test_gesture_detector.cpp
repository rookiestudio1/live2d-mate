// 手勢偵測（Tap / MultiTap / LongPress / Pet）與 hit area 語意化。
// 時間全由樣本注入，時窗與門檻不必靠真人搓滑鼠來驗。
#include <QtTest>

#include "core/gesture_detector.h"

using namespace l2m;

namespace {

PointerSample at(double x, double nowMs, bool pressed = false, std::vector<std::string> areas = {"Head"}) {
  PointerSample sample;
  sample.x = x;
  sample.y = 100;
  // 測試裡視窗不動，螢幕座標＝視窗內座標（真的拖視窗的情境另有專屬測試）
  sample.screenX = x;
  sample.screenY = 100;
  sample.nowMs = nowMs;
  sample.pressed = pressed;
  sample.inside = true;
  sample.areas = std::move(areas);
  return sample;
}

// 一次乾淨的 tap（按下→放開，原地）。單擊是遞延判定的：
// 放開的回傳只有在連滿三下時才是 MultiTap，其餘是 None
Gesture tapAt(GestureDetector& detector, double nowMs) {
  detector.feed(at(100, nowMs, true));
  return detector.feed(at(100, nowMs + 80, false));
}

}  // namespace

class TestGestureDetector : public QObject {
  Q_OBJECT

private slots:
  // === hit area 語意化 ===

  void areaNamesMapToBodyParts() {
    QCOMPARE(bodyPartFor(std::string("Head")), BodyPart::Head);
    QCOMPARE(bodyPartFor(std::string("TapHead")), BodyPart::Head);
    QCOMPARE(bodyPartFor(std::string("HitAreaHair")), BodyPart::Head);
    QCOMPARE(bodyPartFor(std::string("Face")), BodyPart::Face);
    QCOMPARE(bodyPartFor(std::string("TapBody")), BodyPart::Body);
    QCOMPARE(bodyPartFor(std::string("胸")), BodyPart::Chest);
    QCOMPARE(bodyPartFor(std::string("mystery")), BodyPart::Unknown);
    // 整組結果取第一個認得的；Head 優先於 Body（"HeadBody" 這種合體名算頭）
    QCOMPARE(bodyPartFor(std::vector<std::string>{"weird", "Head"}), BodyPart::Head);
    QCOMPARE(bodyPartFor(std::string("HeadBody")), BodyPart::Head);
  }

  // === Tap / MultiTap ===

  // 單擊遞延判定：放開先掛起（None），超過時窗（預設 400）沒有下一擊
  // 才由 tick 收割成 Tap —— 這是單擊表演與連點不打架的關鍵
  void singleTapIsDeferredUntilTheWindowPasses() {
    GestureDetector detector;
    QCOMPARE(tapAt(detector, 0).kind, GestureKind::None);  // 放開在 80
    QCOMPARE(detector.tick(480).kind, GestureKind::None);  // 時窗（80+400）未過
    const Gesture gesture = detector.tick(481);
    QCOMPARE(gesture.kind, GestureKind::Tap);
    QCOMPARE(gesture.part, BodyPart::Head);
    QCOMPARE(gesture.taps, 1);
    QCOMPARE(gesture.areas, (std::vector<std::string>{"Head"}));
    QCOMPARE(detector.tick(1000).kind, GestureKind::None);  // 只收割一次
  }

  // 時窗內的兩下併成一次 Tap（taps=2）——不會疊兩個點擊動作
  void twoQuickTapsMergeIntoOneTap() {
    GestureDetector detector;
    QCOMPARE(tapAt(detector, 0).kind, GestureKind::None);
    QCOMPARE(tapAt(detector, 300).kind, GestureKind::None);  // 放開在 380
    const Gesture gesture = detector.tick(781);
    QCOMPARE(gesture.kind, GestureKind::Tap);
    QCOMPARE(gesture.taps, 2);
  }

  // 連點滿三下 → 立刻 MultiTap（吞掉掛起的單擊）；超出時窗的第三下只是新的第一下
  void threeQuickTapsMakeMultiTap() {
    GestureDetector detector;
    QCOMPARE(tapAt(detector, 0).kind, GestureKind::None);
    QCOMPARE(tapAt(detector, 300).kind, GestureKind::None);
    const Gesture third = tapAt(detector, 600);
    QCOMPARE(third.kind, GestureKind::MultiTap);
    QCOMPARE(third.taps, 3);
    QCOMPARE(third.areas, (std::vector<std::string>{"Head"}));
    QCOMPARE(detector.tick(5000).kind, GestureKind::None);  // 沒有殘留的單擊

    GestureDetector slow;
    tapAt(slow, 0);
    tapAt(slow, 300);
    QCOMPARE(tapAt(slow, 2000).kind, GestureKind::None);  // 隔太久，重新數
    QCOMPARE(slow.tick(2481).taps, 1);                    // 收割出來只是一下
  }

  // 移動超過 tapMaxMovePx 的按放不算 tap（時窗過了也收割不出東西）
  void draggedPressIsNotATap() {
    GestureDetector detector;
    detector.feed(at(100, 0, true));
    detector.feed(at(160, 50, true));  // 拖了 60px
    QCOMPARE(detector.feed(at(160, 100, false)).kind, GestureKind::None);
    QCOMPARE(detector.tick(1000).kind, GestureKind::None);
  }

  // dragMove 拖視窗：視窗跟著游標跑，視窗內座標不動、螢幕座標動了 ——
  // 位移判定吃螢幕座標，拖曳的放開才不會被誤判成 tap
  void draggingTheWindowIsNotATap() {
    GestureDetector detector;
    PointerSample press = at(100, 0, true);
    detector.feed(press);
    PointerSample moved = at(100, 50, true);  // 視窗內座標原地
    moved.screenX = 300;                      // 螢幕上拖了 200px
    detector.feed(moved);
    PointerSample release = at(100, 100, false);
    release.screenX = 300;
    QCOMPARE(detector.feed(release).kind, GestureKind::None);
    QCOMPARE(detector.tick(1000).kind, GestureKind::None);
  }

  // === LongPress ===

  // 按著不放、沒怎麼動、時間到 → LongPress（由 tick 推進時間）；只發一次
  void holdingStillFiresLongPress() {
    GestureDetector detector;
    detector.feed(at(100, 0, true));
    QCOMPARE(detector.tick(500).kind, GestureKind::None);
    const Gesture gesture = detector.tick(800);
    QCOMPARE(gesture.kind, GestureKind::LongPress);
    QCOMPARE(gesture.part, BodyPart::Head);
    QCOMPARE(detector.tick(1000).kind, GestureKind::None);
    // 長按過的放開不再算 tap
    QCOMPARE(detector.feed(at(100, 1100, false)).kind, GestureKind::None);
  }

  // === Pet ===

  // 放開狀態來回滑動：行程與折返都夠 → Pet；同一段撫摸只發一次
  void strokingBackAndForthIsAPet() {
    GestureDetector detector;
    Gesture fired;
    double t = 0;
    // 左右各 30px 來回四趟（折返 7 次、總行程 240px）
    double x = 100;
    detector.feed(at(x, t));
    for (int leg = 0; leg < 8; ++leg) {
      const double target = (leg % 2 == 0) ? 130 : 100;
      for (int step = 0; step < 3; ++step) {
        x += (target > x ? 10 : -10);
        t += 40;
        const Gesture gesture = detector.feed(at(x, t));
        if (gesture.kind != GestureKind::None) {
          QCOMPARE(fired.kind, GestureKind::None);  // 只該發一次
          fired = gesture;
        }
      }
    }
    QCOMPARE(fired.kind, GestureKind::Pet);
    QCOMPARE(fired.part, BodyPart::Head);
  }

  // 只是滑過去（沒有折返）不算撫摸
  void straightSwipeIsNotAPet() {
    GestureDetector detector;
    double t = 0;
    for (double x = 100; x <= 300; x += 10) {
      QCOMPARE(detector.feed(at(x, t += 40)).kind, GestureKind::None);
    }
  }

  // 換部位重新累計：頭上來回兩次＋身上來回兩次，各自不足門檻
  void changingPartResetsPetProgress() {
    GestureDetector detector;
    double t = 0;
    const auto wiggle = [&](const char* area) {
      for (const double x : {100.0, 130.0, 100.0}) {
        const Gesture gesture = detector.feed(at(x, t += 40, false, {area}));
        QCOMPARE(gesture.kind, GestureKind::None);
      }
    };
    wiggle("Head");
    wiggle("Body");
  }

  // 停頓（petIdleMs）之後可以再觸發一次
  void petCanFireAgainAfterAPause() {
    GestureDetector detector;
    double t = 0;
    const auto stroke = [&detector, &t] {
      Gesture fired;
      double x = 100;
      detector.feed(at(x, t));
      for (int leg = 0; leg < 8; ++leg) {
        const double target = (leg % 2 == 0) ? 130 : 100;
        for (int step = 0; step < 3; ++step) {
          x += (target > x ? 10 : -10);
          t += 40;
          const Gesture gesture = detector.feed(at(x, t));
          if (gesture.kind != GestureKind::None) fired = gesture;
        }
      }
      return fired;
    };
    QCOMPARE(stroke().kind, GestureKind::Pet);
    detector.tick(t += 2000);  // 停一陣
    QCOMPARE(stroke().kind, GestureKind::Pet);
  }
};

QTEST_GUILESS_MAIN(TestGestureDetector)
#include "test_gesture_detector.moc"
