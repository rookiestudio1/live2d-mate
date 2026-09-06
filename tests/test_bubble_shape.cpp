// 氣泡外型：橢圓的內接關係、換行寬度收斂、尾巴真的接在橢圓上、
// 餘裕的保守上界對任何 tailX 都成立
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "core/bubble_shape.h"

using namespace l2m;

namespace {

constexpr double kPi = 3.14159265358979323846;

// 一顆典型的氣泡（三行字左右）與一顆短句氣泡
const Rect kBig{22, 22, 287, 173};
const Rect kSmall{22, 22, 238, 96};

// QPainterPath::arcTo 慣例下的橢圓取點：3 點鐘為 0、逆時針為正、y 軸向上
BubblePoint arcPoint(const Rect& e, double degrees) {
  const double rad = degrees * kPi / 180.0;
  return {e.x + e.width / 2 + e.width / 2 * std::cos(rad), e.y + e.height / 2 - e.height / 2 * std::sin(rad)};
}

bool near(double actual, double expected, double tolerance = 1e-6) { return std::abs(actual - expected) <= tolerance; }

std::vector<BubblePoint> tailPoints(const SpeechTail& tail) { return {tail.baseA, tail.ctrlA, tail.tip, tail.ctrlB, tail.baseB}; }

BubblePoint quadAt(const BubblePoint& p0, const BubblePoint& p1, const BubblePoint& p2, double t) {
  const double mt = 1 - t;
  return {mt * mt * p0.x + 2 * mt * t * p1.x + t * t * p2.x, mt * mt * p0.y + 2 * mt * t * p1.y + t * t * p2.y};
}

double distance(const BubblePoint& a, const BubblePoint& b) { return std::hypot(a.x - b.x, a.y - b.y); }

// 尾巴在「走完 t 的長度」時還有多寬：兩條邊各取對應的參數點量距離
double tailWidthAt(const SpeechTail& tail, double t) {
  return distance(quadAt(tail.baseA, tail.ctrlA, tail.tip, t), quadAt(tail.tip, tail.ctrlB, tail.baseB, 1 - t));
}

double headingDegrees(const BubblePoint& from, const BubblePoint& to) { return std::atan2(to.y - from.y, to.x - from.x) * 180.0 / kPi; }

BubblePoint mouthMidOf(const SpeechTail& tail) { return {(tail.baseA.x + tail.baseB.x) / 2, (tail.baseA.y + tail.baseB.y) / 2}; }

// 接口的弦方向（＝橢圓在接點的切線方向）。鏡射的軸就是它的垂線，
// 拿 baseA/baseB 算是最直接的 —— 不必知道 bubble_shape.cpp 內部怎麼參數化。
BubblePoint mouthDirection(const SpeechTail& tail) {
  const double dx = tail.baseB.x - tail.baseA.x;
  const double dy = tail.baseB.y - tail.baseA.y;
  const double len = std::hypot(dx, dy);
  return {dx / len, dy / len};
}

// 掃過 tailX 可能出現的整個範圍（含 placeBubble 夾到兩端的情形）
std::vector<double> tailXSweep(const Rect& e) {
  std::vector<double> out;
  for (int i = 0; i <= 20; ++i) out.push_back(e.x - 40 + (e.width + 80) * i / 20.0);
  return out;
}

}  // namespace

class TestBubbleShape : public QObject {
  Q_OBJECT

private slots:
  // **最重要的一條**：圓弧的兩端必須正好落在 baseB 與 baseA 上。
  // 角度換算錯一個象限，尾巴就接在橢圓的另一側、中間裂一條縫
  void arcEndpointsMatchTheTailBase() {
    for (const BubbleSide side : {BubbleSide::Above, BubbleSide::Below}) {
      for (const double tailX : tailXSweep(kBig)) {
        const SpeechTail tail = speechTail(kBig, tailX, side);
        const BubblePoint start = arcPoint(kBig, tail.arcStartDeg);
        const BubblePoint end = arcPoint(kBig, tail.arcStartDeg + tail.arcSpanDeg);
        QVERIFY(near(start.x, tail.baseB.x, 1e-6) && near(start.y, tail.baseB.y, 1e-6));
        QVERIFY(near(end.x, tail.baseA.x, 1e-6) && near(end.y, tail.baseA.y, 1e-6));
      }
    }
  }

  // 圓弧要繞掉接口那一段（接近整圈，但不是整圈）
  void arcSkipsOnlyTheTailMouth() {
    const SpeechTail tail = speechTail(kBig, kBig.x + kBig.width / 2, BubbleSide::Above);
    const double span = std::abs(tail.arcSpanDeg);
    QVERIFY(span > 300 && span < 360);
  }

  // 尾尖要在橢圓外面而且朝角色那一側 —— 浮在空中或縮進本體都看得出來
  void tipPointsOutwardTowardTheCharacter() {
    const double cx = kBig.x + kBig.width / 2;
    const double cy = kBig.y + kBig.height / 2;
    const double a = kBig.width / 2;
    const double b = kBig.height / 2;

    for (const double tailX : {cx - 120.0, cx, cx + 120.0}) {
      const SpeechTail tail = speechTail(kBig, tailX, BubbleSide::Above);
      // 橢圓方程式 > 1 ＝ 在外面
      const double dx = (tail.tip.x - cx) / a;
      const double dy = (tail.tip.y - cy) / b;
      QVERIFY(dx * dx + dy * dy > 1.0);
      // 氣泡在角色上方時尾巴一定朝下
      QVERIFY(tail.tip.y > cy + b);
      // 角色偏哪一邊，接點就往哪一邊移
      if (tailX < cx) QVERIFY(tail.baseA.x < cx);
      if (tailX > cx) QVERIFY(tail.baseB.x > cx);
    }
  }

  // **這一條就是「像不像葉子」**。參考的漫畫氣泡量出來長寬比 2.8；曾經用
  // 0.55×短半徑當尾長，長寬比只有 1.2 —— 又短又胖的尖角，看起來就是一片葉子。
  void tailIsSlenderNotALeaf() {
    for (const Rect& ellipse : {kBig, kSmall}) {
      for (const double tailX : tailXSweep(ellipse)) {
        const SpeechTail tail = speechTail(ellipse, tailX, BubbleSide::Above);
        const double mouth = distance(tail.baseA, tail.baseB);
        const double length = distance(mouthMidOf(tail), tail.tip);
        QVERIFY2(length > mouth * 1.7, qPrintable(QString("長寬比 %1（尾長 %2 / 嘴寬 %3）").arg(length / mouth).arg(length).arg(mouth)));
      }
    }
  }

  // 該收的還是要收，但不能在半路就收成一條線
  void tailKeepsItsWidthTowardTheTip() {
    for (const Rect& ellipse : {kBig, kSmall}) {
      const SpeechTail tail = speechTail(ellipse, ellipse.x + ellipse.width / 2, BubbleSide::Above);
      const double mouth = distance(tail.baseA, tail.baseB);
      QVERIFY2(tailWidthAt(tail, 0.7) > mouth * 0.3, qPrintable(QString("t=0.7 寬 %1 / 嘴寬 %2").arg(tailWidthAt(tail, 0.7)).arg(mouth)));
      QVERIFY(tailWidthAt(tail, 0.5) > tailWidthAt(tail, 0.7));
      QVERIFY(tailWidthAt(tail, 0.3) > tailWidthAt(tail, 0.5));
    }
  }

  // 兩條邊**永遠不准交叉**。各推各的控制點那一版，鉤子一大就穿過彼此，
  // 畫出來是一個中間掐斷的蝴蝶結。現在的中線構造讓這件事在數學上不可能，
  // 這一條是那個保證的守門人。
  void tailEdgesNeverCross() {
    for (const BubbleSide side : {BubbleSide::Above, BubbleSide::Below}) {
      for (const Rect& ellipse : {kBig, kSmall}) {
        for (const double tailX : tailXSweep(ellipse)) {
          const SpeechTail tail = speechTail(ellipse, tailX, side);
          const double span = distance(tail.baseA, tail.baseB);
          const double wx = (tail.baseB.x - tail.baseA.x) / span;
          const double wy = (tail.baseB.y - tail.baseA.y) / span;
          for (int i = 1; i < 20; ++i) {
            const double t = i / 20.0;
            const BubblePoint outer = quadAt(tail.baseA, tail.ctrlA, tail.tip, t);
            const BubblePoint inner = quadAt(tail.tip, tail.ctrlB, tail.baseB, 1 - t);
            const double separation = (inner.x - outer.x) * wx + (inner.y - outer.y) * wy;
            QVERIFY2(separation > 0, qPrintable(QString("t=%1 兩條邊已經交叉（間距 %2）").arg(t).arg(separation)));
          }
        }
      }
    }
  }

  // 尾巴要是一把**彎的**鐮刀，不是一根直的針
  void tailCurvesRatherThanRunningStraight() {
    for (const Rect& ellipse : {kBig, kSmall}) {
      const SpeechTail tail = speechTail(ellipse, ellipse.x + ellipse.width / 2, BubbleSide::Above);
      const BubblePoint mouthMid = mouthMidOf(tail);
      const double length = distance(mouthMid, tail.tip);

      // 中線 = 兩條邊在對應參數處的中點
      const BubblePoint outer = quadAt(tail.baseA, tail.ctrlA, tail.tip, 0.5);
      const BubblePoint inner = quadAt(tail.tip, tail.ctrlB, tail.baseB, 0.5);
      const BubblePoint centre{(outer.x + inner.x) / 2, (outer.y + inner.y) / 2};
      const BubblePoint chordMid{(mouthMid.x + tail.tip.x) / 2, (mouthMid.y + tail.tip.y) / 2};

      QVERIFY2(distance(centre, chordMid) > length * 0.07, qPrintable(QString("弧高 %1 / 尾長 %2，幾乎是直的").arg(distance(centre, chordMid)).arg(length)));
    }
  }

  // 上下翻面就是鏡像：x 一致、y 對稱於橢圓中心
  void aboveAndBelowAreMirrored() {
    const double cy = kBig.y + kBig.height / 2;
    const SpeechTail above = speechTail(kBig, kBig.x + 60, BubbleSide::Above);
    const SpeechTail below = speechTail(kBig, kBig.x + 60, BubbleSide::Below);

    const std::vector<BubblePoint> up = tailPoints(above);
    const std::vector<BubblePoint> down = tailPoints(below);
    for (size_t i = 0; i < up.size(); ++i) {
      QVERIFY(near(up[i].x, down[i].x, 1e-6));
      QVERIFY(near(up[i].y - cy, -(down[i].y - cy), 1e-6));
    }
  }

  // 鏡射就是對「接點處的法線」翻面 —— **不是鉛直線**：接點偏離正下方時橢圓的
  // 法線是斜的，兩者只有在接點正好在正下方時才重合。
  // 氣泡被推向螢幕外側之後尾巴要往回斜，所以這個翻面一定要跟 placeBubble 的
  // 位移方向綁在一起 —— 各判各的話會變成氣泡往一邊挪、尾巴往另一邊甩。
  void mirroredTailIsTheMirrorImage() {
    for (const BubbleSide side : {BubbleSide::Above, BubbleSide::Below}) {
      for (const double tailX : tailXSweep(kBig)) {
        const SpeechTail plain = speechTail(kBig, tailX, side, false);
        const SpeechTail flipped = speechTail(kBig, tailX, side, true);

        // 軸通過**接點**，不是弦的中點（弦的中點沿半徑縮進來了一點）
        const BubblePoint origin = plain.mouthMid;
        QVERIFY(near(flipped.mouthMid.x, origin.x, 1e-6) && near(flipped.mouthMid.y, origin.y, 1e-6));
        // across ＝ 沿著接口的弦（鏡射要翻的那一軸）；along ＝ 它的垂線
        const BubblePoint t = mouthDirection(plain);
        const auto across = [&](const BubblePoint& p) { return (p.x - origin.x) * t.x + (p.y - origin.y) * t.y; };
        const auto along = [&](const BubblePoint& p) { return (p.x - origin.x) * (-t.y) + (p.y - origin.y) * t.x; };

        // 法線方向不變、側向取負 ＝ 對法線鏡射
        QVERIFY(near(along(flipped.tip), along(plain.tip), 1e-6));
        QVERIFY(near(across(flipped.tip), -across(plain.tip), 1e-6));
        // 兩條邊互換：鏡射後的外緣控制點 ＝ 原本內緣控制點的鏡像
        QVERIFY(near(along(flipped.ctrlA), along(plain.ctrlB), 1e-6));
        QVERIFY(near(across(flipped.ctrlA), -across(plain.ctrlB), 1e-6));
      }
    }
  }

  // 鏡射不能破壞「兩條邊不交叉」那個保證
  void mirroredTailStillNeverCrosses() {
    for (const double tailX : tailXSweep(kBig)) {
      const SpeechTail tail = speechTail(kBig, tailX, BubbleSide::Above, true);
      const double span = distance(tail.baseA, tail.baseB);
      const double wx = (tail.baseB.x - tail.baseA.x) / span;
      const double wy = (tail.baseB.y - tail.baseA.y) / span;
      for (int i = 1; i < 20; ++i) {
        const double t = i / 20.0;
        const BubblePoint outer = quadAt(tail.baseA, tail.ctrlA, tail.tip, t);
        const BubblePoint inner = quadAt(tail.tip, tail.ctrlB, tail.baseB, 1 - t);
        QVERIFY((inner.x - outer.x) * wx + (inner.y - outer.y) * wy > 0);
      }
    }
  }

  // 思考泡泡的圓點串也要跟著鏡射 —— 兩種氣泡吃的是同一個 mirrored
  void mirroredThoughtDotsFlipToo() {
    const double tailX = kBig.x + kBig.width / 2;
    const auto plain = thoughtDots(kBig, tailX, BubbleSide::Above, false);
    const auto flipped = thoughtDots(kBig, tailX, BubbleSide::Above, true);
    QCOMPARE(flipped.size(), plain.size());
    for (size_t i = 0; i < plain.size(); ++i) {
      QVERIFY(near(flipped[i].center.x - tailX, -(plain[i].center.x - tailX), 1e-6));
      QVERIFY(near(flipped[i].center.y, plain[i].center.y, 1e-6));
      QCOMPARE(flipped[i].radius, plain[i].radius);
    }
  }

  // 思考泡泡：三顆、由大到小、依序遠離本體
  void thoughtDotsShrinkAndMarchAway() {
    const auto dots = thoughtDots(kBig, kBig.x + kBig.width / 2, BubbleSide::Above);
    QCOMPARE(dots.size(), size_t(3));

    const double bottom = kBig.y + kBig.height;
    double previousRadius = 1e9;
    double previousY = bottom;
    for (const ThoughtDot& dot : dots) {
      QVERIFY(dot.radius > 0);
      QVERIFY(dot.radius < previousRadius);
      QVERIFY(dot.center.y > previousY);
      previousRadius = dot.radius;
      previousY = dot.center.y;
    }
    // 第一顆不能黏在橢圓上，否則看起來像尾巴而不是圓點串
    QVERIFY(dots.front().center.y - dots.front().radius > bottom);
  }

  // 圓點串要沿著一道**弧**延伸，不是一條斜線。側偏一旦寫成距離的線性函數，
  // 三顆圓心就會剛好共線 —— 那正是這一條要擋的回歸。
  void thoughtDotsFollowACurve() {
    const auto dots = thoughtDots(kBig, kBig.x + kBig.width / 2, BubbleSide::Above);
    QCOMPARE(dots.size(), size_t(3));

    const double first = headingDegrees(dots[0].center, dots[1].center);
    const double second = headingDegrees(dots[1].center, dots[2].center);
    QVERIFY2(std::abs(first - second) > 8.0, qPrintable(QString("兩段方向差 %1 度，幾乎共線").arg(std::abs(first - second))));

    // 而且第一顆要幾乎在氣泡正下方 —— 一出發就大幅偏開的話，弧就變回斜線了
    const double cx = kBig.x + kBig.width / 2;
    QVERIFY(std::abs(dots[0].center.x - cx) < std::abs(dots[2].center.x - cx) / 3);
  }

  // 圓點之間**看得到的間隙**必須均勻。曾經照「沿法線的距離」排，而側偏是三次的、
  // 最後一段甩得最開 —— 實測最後兩顆的間隙是前兩顆的 2.1 倍（2.06 vs 0.96 個
  // 半徑），畫面上就是「最小那顆離得特別遠」。
  void thoughtDotGapsAreEven() {
    for (const Rect& ellipse : {kBig, kSmall}) {
      for (const bool mirrored : {false, true}) {
        const auto dots = thoughtDots(ellipse, ellipse.x + ellipse.width / 2, BubbleSide::Above, mirrored);
        const double first = distance(dots[0].center, dots[1].center) - dots[0].radius - dots[1].radius;
        const double second = distance(dots[1].center, dots[2].center) - dots[1].radius - dots[2].radius;
        QVERIFY(first > 0 && second > 0);
        QVERIFY2(std::abs(second - first) < first * 0.05, qPrintable(QString("間隙 %1 vs %2").arg(first).arg(second)));
      }
    }
  }

  // 最後一步不能變成幾乎橫的。把圓心釘在「斜率預先給定的射線」上時，最後一步
  // 偏離法線 81.6 度，畫面上就是「最小那顆跟前一顆幾乎水平」—— 這一條用
  // 「沿著尾巴方向前進了多少」把它釘住。
  void lastDotKeepsAdvancingAlongTheTail() {
    for (const Rect& ellipse : {kBig, kSmall}) {
      // 接點在正下方時尾巴朝正下，沿尾巴方向前進 ＝ y 增加
      const auto dots = thoughtDots(ellipse, ellipse.x + ellipse.width / 2, BubbleSide::Above, false);
      const double firstStep = dots[1].center.y - dots[0].center.y;
      const double secondStep = dots[2].center.y - dots[1].center.y;
      QVERIFY(firstStep > 0);
      QVERIFY2(secondStep > firstStep * 0.4, qPrintable(QString("第二步只前進 %1，第一步是 %2").arg(secondStep).arg(firstStep)));
    }
  }

  // 上下翻面時圓點串也要跟著翻，不能還留在原來那一側
  void thoughtDotsFollowTheSide() {
    const auto below = thoughtDots(kBig, kBig.x + kBig.width / 2, BubbleSide::Below);
    for (const ThoughtDot& dot : below) QVERIFY(dot.center.y < kBig.y);
  }

  // **量測不能依賴 tailX**（視窗大小要在 placeBubble 之前定案），所以
  // bubbleTailExtent 給的必須是對任何 tailX 都成立的上界。破了就是尾巴被裁掉。
  void extentBoundsEveryTailPosition() {
    struct Case {
      Rect ellipse;
      BubbleStyle style;
    };
    const Case cases[] = {{kBig, BubbleStyle::Speech}, {kSmall, BubbleStyle::Speech}, {kBig, BubbleStyle::Thought}, {kSmall, BubbleStyle::Thought}};

    for (const Case& item : cases) {
      const BubbleSize size{item.ellipse.width, item.ellipse.height};
      const double extent = bubbleTailExtent(item.style, size);
      const double bottom = item.ellipse.y + item.ellipse.height;
      const double top = item.ellipse.y;

      for (const double tailX : tailXSweep(item.ellipse)) {
        if (item.style == BubbleStyle::Speech) {
          for (const BubblePoint& point : tailPoints(speechTail(item.ellipse, tailX, BubbleSide::Above))) {
            QVERIFY2(point.y - bottom <= extent, qPrintable(QString("下緣超出 %1 > %2").arg(point.y - bottom).arg(extent)));
          }
          for (const BubblePoint& point : tailPoints(speechTail(item.ellipse, tailX, BubbleSide::Below))) {
            QVERIFY(top - point.y <= extent);
          }
        } else {
          for (const ThoughtDot& dot : thoughtDots(item.ellipse, tailX, BubbleSide::Above)) {
            QVERIFY2(dot.center.y + dot.radius - bottom <= extent, qPrintable(QString("下緣超出 %1 > %2").arg(dot.center.y + dot.radius - bottom).arg(extent)));
          }
        }
      }
    }
  }

  // 側邊：尾巴與圓點都不准伸出視窗留的那一圈餘裕（kBubblePadding）。
  //
  // **只掃接點附近**：placeBubble 的 tailX 固定是 size.width / 2（接點永遠在
  // 氣泡正中，理由見 bubble_placement.cpp），所以偏很遠的接點是 speechTail
  // 支援、但目前產生不出來的情形。那種情形下尾尖會伸出餘裕 —— 餘裕只夠外框，
  // 不夠一支斜出去的尾巴，真要讓接點追著角色跑就得同時把 kBubblePadding 加大。
  void tailStaysWithinTheLateralPadding() {
    for (const Rect& ellipse : {kBig, kSmall}) {
      const double left = ellipse.x - kBubblePadding;
      const double right = ellipse.x + ellipse.width + kBubblePadding;
      const double centre = ellipse.x + ellipse.width / 2;
      for (const double tailX : {centre - ellipse.width * 0.05, centre, centre + ellipse.width * 0.05}) {
        for (const BubblePoint& point : tailPoints(speechTail(ellipse, tailX, BubbleSide::Above))) {
          QVERIFY2(point.x >= left && point.x <= right, qPrintable(QString("x=%1 超出 [%2, %3]").arg(point.x).arg(left).arg(right)));
        }
        for (const ThoughtDot& dot : thoughtDots(ellipse, tailX, BubbleSide::Above)) {
          QVERIFY2(dot.center.x - dot.radius >= left && dot.center.x + dot.radius <= right,
                   qPrintable(QString("圓點 x=%1 r=%2 超出 [%3, %4]").arg(dot.center.x).arg(dot.radius).arg(left).arg(right)));
        }
      }
    }
  }


  // === 陰影 ===

  // **整組陰影唯一的幾何約束**：模糊糊開的距離不准超過位移。視窗只在右下多留餘裕
  //（bubbleShadowExtent），左上靠的是既有的餘裕，而尾巴那一側只有 4 px；
  // blur > offset 的話影子會往左上溢出去，畫面上是「氣泡上緣或尾尖的影子被切掉一條直線」。
  void shadowNeverSpillsPastTheExistingMargin() {
    for (const BubbleShadow shadow : {BubbleShadow::Off, BubbleShadow::Hard, BubbleShadow::Soft}) {
      const BubbleShadowSpec spec = bubbleShadowSpec(shadow);
      QVERIFY2(spec.blur <= spec.offset, bubbleShadowId(shadow));
      // 右下要留的就是「位移 ＋ 糊開的距離」，兩者是疊加的
      QCOMPARE(bubbleShadowExtent(shadow), spec.offset + spec.blur);
    }
  }

  // 關掉就要真的什麼都不做：alpha 0（不畫）且不多佔一格視窗
  void shadowOffCostsNothing() {
    QCOMPARE(bubbleShadowSpec(BubbleShadow::Off).alpha, 0);
    QCOMPARE(bubbleShadowExtent(BubbleShadow::Off), 0.0);
  }

  // 硬邊沒有模糊、柔邊有；兩者都要看得見（alpha > 0）
  void hardIsCrispAndSoftIsBlurred() {
    QCOMPARE(bubbleShadowSpec(BubbleShadow::Hard).blur, 0.0);
    QVERIFY(bubbleShadowSpec(BubbleShadow::Soft).blur > 0);
    QVERIFY(bubbleShadowSpec(BubbleShadow::Hard).alpha > 0);
    QVERIFY(bubbleShadowSpec(BubbleShadow::Soft).alpha > 0);
  }

  // 這份清單同時是 config 的允許值（config_schema.cpp 的 readEnum 直接吃它），
  // 所以每個 id 都必須解得出一個不同的檔位、而且轉得回原本那個字串。
  // 對不上的下場是設定存得進去卻畫不出來。
  void shadowIdsRoundTrip() {
    const std::vector<std::string>& ids = bubbleShadowIds();
    QCOMPARE(ids.size(), size_t(3));
    std::vector<std::string> seen;
    for (const std::string& id : ids) {
      QCOMPARE(std::string(bubbleShadowId(bubbleShadowFromId(id))), id);
      seen.push_back(id);
    }
    std::sort(seen.begin(), seen.end());
    QCOMPARE(std::unique(seen.begin(), seen.end()), seen.end());
  }

  // 認不得的字串一律回預設的硬邊 —— 手改壞 config.json 不該讓氣泡變成沒有影子
  //（那看起來就像設定被忽略了）
  void unknownShadowIdFallsBackToHard() {
    QCOMPARE(bubbleShadowFromId("nope"), BubbleShadow::Hard);
    QCOMPARE(bubbleShadowFromId(""), BubbleShadow::Hard);
  }

  // 退化尺寸不能吐出 NaN：那會讓整條 QPainterPath 靜默消失
  void degenerateEllipseStaysFinite() {
    const Rect flat{0, 0, 0, 0};
    const SpeechTail tail = speechTail(flat, 0, BubbleSide::Above);
    for (const BubblePoint& point : tailPoints(tail)) {
      QVERIFY(std::isfinite(point.x) && std::isfinite(point.y));
    }
    QVERIFY(std::isfinite(tail.arcStartDeg) && std::isfinite(tail.arcSpanDeg));
    for (const ThoughtDot& dot : thoughtDots(flat, 0, BubbleSide::Above)) {
      QVERIFY(std::isfinite(dot.center.x) && std::isfinite(dot.center.y) && dot.radius > 0);
    }
  }
};

QTEST_APPLESS_MAIN(TestBubbleShape)
#include "test_bubble_shape.moc"
