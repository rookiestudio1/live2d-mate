// 氣泡擺放：上方優先、貼邊翻到下方、左右夾住時尾巴補償、多螢幕與位移的邊界、
// 以及「依角色在螢幕的哪一半把氣泡往外側推開並鏡射尾巴」，
// 以及「氣泡貼的是模型頭頂而不是視窗頂端」的錨點矩形
#include <QtTest>

#include <limits>

#include "core/bubble_placement.h"

using namespace l2m;

namespace {

// 1920x1080、上方沒有工作列的螢幕
const Rect kArea{0, 0, 1920, 1040};
// scale = 1 的角色視窗，放在右下角
const Rect kChar{1496, 416, 400, 600};
const BubbleSize kSize{320, 90};
// 撐到最大寬度的氣泡：只有這種寬度才會真的碰到螢幕邊緣
const BubbleSize kWide{456, 120};

// 氣泡視窗的水平中心
double p_centre(const BubblePlacement& p, const BubbleSize& size) { return p.x + size.width / 2; }

}  // namespace

class TestBubblePlacement : public QObject {
  Q_OBJECT

private slots:
  // 預設放在角色正上方，底邊離角色頂端一個 gap
  void defaultAboveWithGap() {
    const auto p = placeBubble(kChar, kSize, kArea);
    QCOMPARE(p.side, BubbleSide::Above);
    QCOMPARE(p.y + kSize.height, kChar.y - kBubbleGap);
  }

  // 水平不是對齊角色中心，而是**往螢幕中心那一側推開 kBubbleSideShift**：
  // 角色因此落在氣泡外側，尾巴得斜著往回指。正上方對齊的話尾巴幾乎是直的，
  // 看起來像從頭頂長出一根天線。
  void pushedTowardScreenCentre() {
    const auto right = placeBubble(kChar, kSize, kArea);  // kChar 在右半邊 → 往左推
    QCOMPARE(p_centre(right, kSize), kChar.x + kChar.width / 2 - kBubbleSideShift);
    QVERIFY(!right.mirrored);

    Rect left = kChar;
    left.x = 200;  // 左半邊 → 往右推、尾巴鏡射
    const auto p = placeBubble(left, kSize, kArea);
    QCOMPARE(p_centre(p, kSize), left.x + left.width / 2 + kBubbleSideShift);
    QVERIFY(p.mirrored);
  }

  // 位移量要乘上模型比例 —— 角色縮到 0.5 倍時固定挪 40 px 會歪過頭
  void sideShiftScalesWithTheModel() {
    const auto half = placeBubble(kChar, kSize, kArea, kBubbleGap, 0, 0.5);
    QCOMPARE(p_centre(half, kSize), kChar.x + kChar.width / 2 - kBubbleSideShift * 0.5);
  }

  // 尾巴的接點**永遠在氣泡正中**，不追著角色跑 —— 追著跑會把上面那個位移的
  // 斜度當場抵銷掉，看起來就不像從嘴巴噴出來的
  void tailAttachesAtTheBubbleCentre() {
    QCOMPARE(double(placeBubble(kChar, kSize, kArea).tailX), kSize.width / 2);

    Rect left = kChar;
    left.x = 200;
    QCOMPARE(double(placeBubble(left, kSize, kArea).tailX), kSize.width / 2);

    // 角色被拖到螢幕外、氣泡被夾在邊緣，接點還是在正中
    Rect off = kChar;
    off.x = -320;
    QCOMPARE(double(placeBubble(off, kSize, kArea).tailX), kSize.width / 2);
  }

  // 角色貼在螢幕頂端時翻到下方，尾巴改朝上
  void flipsBelowAtScreenTop() {
    Rect top = kChar;
    top.y = 24;
    const auto p = placeBubble(top, kSize, kArea);
    QCOMPARE(p.side, BubbleSide::Below);
    QCOMPARE(double(p.y), top.y + top.height + kBubbleGap);
  }

  // 高度剛好塞得下就不翻面 —— 邊界是「等於」也算塞得下
  void exactFitStaysAbove() {
    Rect exact = kChar;
    exact.y = kArea.y + kSize.height + kBubbleGap;
    QCOMPARE(placeBubble(exact, kSize, kArea).side, BubbleSide::Above);
    Rect oneLess = exact;
    oneLess.y = exact.y - 1;
    QCOMPARE(placeBubble(oneLess, kSize, kArea).side, BubbleSide::Below);
  }

  // 角色被拖出螢幕左緣時氣泡被夾在螢幕內（往中心的位移抵銷不掉那麼多）
  void clampedAtLeftEdge() {
    Rect left = kChar;
    left.x = -100;
    const auto p = placeBubble(left, kWide, kArea);
    QCOMPARE(double(p.x), kArea.x);
    QVERIFY(p.mirrored);
  }

  // 角色被拖出螢幕右緣時同樣夾住
  void clampedAtRightEdge() {
    Rect right = kChar;
    right.x = 1600;
    const auto p = placeBubble(right, kWide, kArea);
    QCOMPARE(p.x + kWide.width, kArea.x + kArea.width);
    QVERIFY(!p.mirrored);
  }

  // 角色剛好停在螢幕正中央：邊界取「算右半邊」，不能左右抖動
  void exactScreenCentreIsTreatedAsRightHalf() {
    Rect middle = kChar;
    middle.x = 760;  // 中心 960 ＝ 螢幕正中央
    const auto p = placeBubble(middle, kWide, kArea);
    QVERIFY(p.mirrored);
    QCOMPARE(p_centre(p, kWide), middle.x + middle.width / 2 + kBubbleSideShift);
  }

  // 多螢幕：工作區原點不是 0 時照樣夾在該螢幕內
  void multiScreenNonZeroOrigin() {
    const Rect second{1920, 0, 1280, 1000};
    const Rect ch{1920, 400, 400, 600};
    const BubbleSize huge{600, 120};
    const auto p = placeBubble(ch, huge, second);
    QCOMPARE(double(p.x), second.x);
    // 左右半邊是拿**該螢幕**的中心判的（2560），不是虛擬桌面的原點 ——
    // 拿 0 去判的話這個角色會被當成右半邊，往反方向推
    QVERIFY(p.mirrored);
  }

  // 角色頂端被拖出螢幕上緣時翻到下方
  void flipsBelowWhenDraggedAboveScreen() {
    Rect off = kChar;
    off.y = -100;
    const auto p = placeBubble(off, kSize, kArea);
    QCOMPARE(p.side, BubbleSide::Below);
    QCOMPARE(double(p.y), off.y + off.height + kBubbleGap);
  }

  // 負的 offset 把氣泡往角色身上拉近；在上方時是往下移
  void negativeOffsetPullsBubbleDownWhenAbove() {
    const auto base = placeBubble(kChar, kSize, kArea);
    const auto moved = placeBubble(kChar, kSize, kArea, kBubbleGap, -40);
    QCOMPARE(moved.side, BubbleSide::Above);
    QCOMPARE(moved.y, base.y + 40);
  }

  // 翻到下方時同一個負值改成往上移 —— offset 是「與角色的距離」而不是螢幕 Y 軸位移
  void negativeOffsetPushesBubbleUpWhenBelow() {
    Rect top = kChar;
    top.y = 24;
    const auto base = placeBubble(top, kSize, kArea);
    const auto moved = placeBubble(top, kSize, kArea, kBubbleGap, -40);
    QCOMPARE(base.side, BubbleSide::Below);
    QCOMPARE(moved.side, BubbleSide::Below);
    QCOMPARE(moved.y, base.y - 40);
  }

  // offset 大過角色高度時被夾住：最多壓到角色的另一端，不會穿過去
  //（穿過去的話 side 會與實際位置相反，尾巴朝錯邊）
  void offsetClampedAtCharacterHeight() {
    const auto p = placeBubble(kChar, kSize, kArea, kBubbleGap, -5000);
    QCOMPARE(p.side, BubbleSide::Above);
    QCOMPARE(double(p.y) + kSize.height, kChar.y + kChar.height);
  }

  // 正的 offset 把氣泡推遠，但仍然被夾在工作區內
  void largePositiveOffsetStaysInsideArea() {
    const auto p = placeBubble(kChar, kSize, kArea, kBubbleGap, 5000);
    QVERIFY(double(p.y) >= kArea.y);
    QVERIFY(double(p.y) + kSize.height <= kArea.y + kArea.height);
  }

  // ── 錨點矩形（bubbleAnchorRect）：氣泡貼的是模型頭頂，不是視窗頂端 ──

  // 上下緣各切掉一段，回來的矩形只動垂直方向；水平原封不動
  void anchorTrimsVerticallyOnly() {
    const Rect a = bubbleAnchorRect(kChar, 0.25, 0.75);
    QCOMPARE(a.x, kChar.x);
    QCOMPARE(a.width, kChar.width);
    QCOMPARE(a.y, kChar.y + 0.25 * kChar.height);
    QCOMPARE(a.height, 0.5 * kChar.height);
  }

  // 0/1 ＝ 整個視窗，也就是量到之前的舊行為（必須是 no-op）
  void anchorFullExtentIsIdentity() {
    const Rect a = bubbleAnchorRect(kChar, 0.0, 1.0);
    QCOMPARE(a.y, kChar.y);
    QCOMPARE(a.height, kChar.height);
  }

  // 畫布上方留白大的模型：氣泡底邊貼的是模型頭頂，而不是視窗頂端
  void bubbleSitsOnModelHead() {
    const Rect head = bubbleAnchorRect(kChar, 0.3, 1.0);
    const auto p = placeBubble(head, kSize, kArea);
    QCOMPARE(p.side, BubbleSide::Above);
    QCOMPARE(double(p.y) + kSize.height, kChar.y + 0.3 * kChar.height - kBubbleGap);
    // 對照組：不套錨點的話會高出留白那 180px
    const auto raw = placeBubble(kChar, kSize, kArea);
    QCOMPARE(p.y - raw.y, static_cast<int>(0.3 * kChar.height));
  }

  // 水平中心不受影響：kBubbleSideShift 的推開量與尾巴鏡射都跟原本一樣
  void anchorKeepsHorizontalPlacement() {
    const auto raw = placeBubble(kChar, kSize, kArea);
    const auto anchored = placeBubble(bubbleAnchorRect(kChar, 0.3, 0.95), kSize, kArea);
    QCOMPARE(anchored.x, raw.x);
    QCOMPARE(anchored.mirrored, raw.mirrored);
    QCOMPARE(anchored.tailX, raw.tailX);
  }

  // 量測不可信時原樣回傳視窗矩形：顛倒、超薄、非有限數、零高度視窗
  void anchorFallsBackOnBadMeasurement() {
    const Rect flipped = bubbleAnchorRect(kChar, 0.8, 0.2);
    QCOMPARE(flipped.y, kChar.y);
    QCOMPARE(flipped.height, kChar.height);
    // 只抓到一條細縫（低於 kMinModelExtent）
    const Rect sliver = bubbleAnchorRect(kChar, 0.5, 0.5 + kMinModelExtent / 2);
    QCOMPARE(sliver.y, kChar.y);
    QCOMPARE(sliver.height, kChar.height);
    const Rect nan = bubbleAnchorRect(kChar, std::numeric_limits<double>::quiet_NaN(), 1.0);
    QCOMPARE(nan.y, kChar.y);
    QCOMPARE(nan.height, kChar.height);
    Rect empty = kChar;
    empty.height = 0;
    QCOMPARE(bubbleAnchorRect(empty, 0.2, 0.9).height, 0.0);
  }

  // 超出 0..1 的值先夾再判斷（遮罩量出來的一定在範圍內，但夾了才不會算出視窗外的錨點）
  void anchorClampsToWindow() {
    const Rect a = bubbleAnchorRect(kChar, -0.5, 1.5);
    QCOMPARE(a.y, kChar.y);
    QCOMPARE(a.height, kChar.height);
  }
};

QTEST_APPLESS_MAIN(TestBubblePlacement)
#include "test_bubble_placement.moc"
