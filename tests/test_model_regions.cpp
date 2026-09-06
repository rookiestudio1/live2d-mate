// 沒有 HitAreas 的模型怎麼從外接框推出「點在哪個部位」（core/model_regions.h）
#include <QtTest>

#include "core/model_regions.h"

using namespace l2m;

namespace {

// 全身立繪：高 2.0、寬 0.8（h/w = 2.5）
constexpr ModelBox kFullBody{-0.4, 0.4, -1.0, 1.0};
// 胸像：高 1.0、寬 0.9（h/w ≈ 1.11）—— 桌寵模型常見的那一種
constexpr ModelBox kBustUp{-0.45, 0.45, -0.5, 0.5};

// 框內相對高度（0＝頭頂、1＝最底）換成 view 座標的 y
double atDepth(const ModelBox& box, double relative) { return box.top - relative * box.height(); }

}  // namespace

class TestModelRegions : public QObject {
  Q_OBJECT

private slots:
  // 頭部佔比隨長寬比走：細長的全身立繪頭很小，接近方形的胸像頭佔一半
  void headFractionTracksAspect() {
    QVERIFY(headFractionFor(2.5) > 0.21 && headFractionFor(2.5) < 0.23);
    QVERIFY(headFractionFor(1.7) > 0.31 && headFractionFor(1.7) < 0.33);
    // 上下限：再方也不會超過一半，再細長也留 0.16
    QCOMPARE(headFractionFor(1.0), 0.5);
    QCOMPARE(headFractionFor(5.0), 0.16);
    // 退化的框（高度或寬度為 0 時算出來的 aspect）不能讓它爆掉
    QCOMPARE(headFractionFor(0.0), 0.5);
    QCOMPARE(headFractionFor(-1.0), 0.5);
  }

  // 全身立繪由上而下：頭 → 胸 → 身體 → 腿
  void fullBodySplitsTopDown() {
    QCOMPARE(regionAt(kFullBody, 0, atDepth(kFullBody, 0.05)), BodyPart::Head);
    QCOMPARE(regionAt(kFullBody, 0, atDepth(kFullBody, 0.30)), BodyPart::Chest);
    QCOMPARE(regionAt(kFullBody, 0, atDepth(kFullBody, 0.45)), BodyPart::Body);
    QCOMPARE(regionAt(kFullBody, 0, atDepth(kFullBody, 0.95)), BodyPart::Leg);
  }

  // **胸口那一段要窄。** 它是 touch_special 的入口，區間開大的話整個上半身
  // 戳下去都播 special、touch_body 幾乎叫不出來（見 kChestShare 的註解）。
  // 腰腹的高度必須已經是 Body。
  void chestBandStaysNarrow() {
    QCOMPARE(regionAt(kFullBody, 0, atDepth(kFullBody, 0.40)), BodyPart::Body);
    QCOMPARE(regionAt(kFullBody, 0, atDepth(kFullBody, 0.50)), BodyPart::Body);
  }

  // **同一個相對高度，在胸像上要判成頭而不是胸** ——
  // 頭部佔比寫死成常數的話這一格必錯，那正是要用長寬比的理由
  void bustUpKeepsHeadBigger() {
    QCOMPARE(regionAt(kBustUp, 0, atDepth(kBustUp, 0.30)), BodyPart::Head);
    QCOMPARE(regionAt(kFullBody, 0, atDepth(kFullBody, 0.30)), BodyPart::Chest);
  }

  // 框外的點夾回框內：剪影與外接框都有誤差，邊緣那一下判成 Unknown
  // 會讓它掉回隨機動作，比夾進來更糟
  void clampsOutsidePoints() {
    QCOMPARE(regionAt(kFullBody, 0, 5.0), BodyPart::Head);
    QCOMPARE(regionAt(kFullBody, 0, -5.0), BodyPart::Leg);
  }

  // 沒有可用的外接框（模型還沒畫過、或整個透明）
  void invalidBoxIsUnknown() {
    QCOMPARE(regionAt(ModelBox{}, 0, 0), BodyPart::Unknown);
    QCOMPARE(regionAt(ModelBox{0.4, -0.4, -1.0, 1.0}, 0, 0), BodyPart::Unknown);
  }
};

QTEST_APPLESS_MAIN(TestModelRegions)
#include "test_model_regions.moc"
