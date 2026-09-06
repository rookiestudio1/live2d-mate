#include "bubble_shape.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace l2m {

namespace {

constexpr double kPi = 3.14159265358979323846;

// ── 尾巴 ──
// 接點最多能離開「尾巴那一側的正中」多少。夾在 45 度有兩個理由：再側過去
// 尾巴會接在橢圓幾乎垂直的那一段上、方向看起來不像從嘴巴出來；而且尾尖會
// 伸出橢圓的左右範圍，視窗的側邊餘裕就得跟著加寬。
constexpr double kMaxAttachAngle = kPi / 4;
// 尾巴的比例是「像不像葉子」的全部。參考的漫畫氣泡量出來：接口寬 21 px、
// 尾巴長 59 px，**長寬比 2.8** —— 是一把細長的鐮刀，不是一個胖尖角。
// 所以尾巴長度由**接口寬**決定而不是橢圓半徑（曾經是 0.55×短半徑，長寬比只有
// 1.2，畫出來就是一片葉子），再用 0.85×短半徑收一次，小氣泡的尾巴才不會
// 長得比氣泡本身還誇張。
constexpr double kTailPerMouth = 2.7;
constexpr double kTailOfSemiMinor = 0.85;
constexpr double kTailMinLength = 26;
constexpr double kTailMaxLength = 70;
// 接口的半寬（相對於長半徑）。參考圖量出來只有 0.075 —— 接口一寬尾巴就胖了。
constexpr double kMouthRatio = 0.08;
constexpr double kMouthMinHalf = 7;
constexpr double kMouthMaxHalf = 13;
// 尾尖的側偏（相對尾長）。尾巴還是要指得到角色，所以不能太大。
constexpr double kTipHook = 0.42;
// 尾巴的中線彎多少、兩條邊各自離中線多遠，單位都是「接口半寬」。
// **兩條邊是掛在同一個中線控制點上、對稱地各偏 kTailEdge**（見 speechTail）：
// 這樣相減之後只剩 w 方向恆正的一項，兩條邊永遠不會交叉，而 kTailBend
// 只讓尾巴彎、不會讓它變細。兩個控制點各推各的那一版，鉤子一大就穿過彼此，
// 畫出來是中間掐斷的蝴蝶結。
constexpr double kTailBend = -1.10;
constexpr double kTailEdge = 0.95;

// ── 思考泡泡的圓點 ──
constexpr double kDotBaseRatio = 0.085;  // 最大那顆的半徑，相對於長半徑
constexpr double kDotMinRadius = 7;
constexpr double kDotMaxRadius = 15;
constexpr double kDotShrink[3] = {1.0, 0.72, 0.5};
constexpr double kDotFirstGap = 1.35;  // 第一顆離橢圓多遠（單位：最大半徑）
// 圓點之間看得到的**實際間隙**（單位：最大半徑）。調這一個就能整串鬆緊。
constexpr double kDotGap = 0.85;
// 圓點串每一步轉多少（弧度）。第一顆正對著接點，之後每一步的方向再多轉一個
// kDotTurn —— 等角轉向畫出來就是一段圓弧，調大就更彎。
//
// 曾經改成「每顆圓心落在一條斜率預先給定的射線上」（斜率隨序號成三次成長），
// 那樣有閉式解，但最後一步的斜率從 0.32 跳到 0.72，圓心得幾乎橫著移過去才追
// 得上那條線：實測最後一步偏離法線 81.6 度，畫面上就是「最小那顆跟前一顆幾乎
// 水平」。直接指定每一步的方向就沒有這個問題。
//
// 上限是 90 度／步：再大 cos 變負，圓點會往回走，bubbleTailExtent 的上界也跟著破。
// 乘上 (1 - |lean|) 讓它隨接點外移而收斂：接點已經在橢圓側邊時再往外彎，
// 最後那顆會頂出視窗的側邊餘裕。
constexpr double kDotTurn = 0.42;  // ≈ 24 度

// 外框是畫在路徑上的，線寬有一半落在路徑外面（見 bubble_window.cpp 的
// kOutlineWidth），再加上反鋸齒會再糊出去一點 —— 留這麼多就不會被視窗裁掉。
constexpr double kOutlineMargin = 4;

// ── 陰影 ──
// 位移與模糊半徑（px）。**模糊半徑一定要 ≤ 位移**：模糊會往四面糊開，而視窗只在
// 右下多留餘裕（bubbleShadowExtent），blur ≤ offset 時往左上糊開的那一段剛好被位移
// 吃掉，於是四邊裡最薄的那一側（尾巴那側只有 kOutlineMargin）不必跟著加厚。
// 位移取 4 px 是照字級來的：14 px 的字配 4 px 的影子還看得出是同一顆氣泡的影子，
// 再大就變成兩顆疊在一起。
constexpr double kShadowOffset = 4;
constexpr double kShadowBlur = 4;
// 硬邊比柔邊淡：硬邊是一整片同樣濃度的色塊，柔邊的邊緣會自己衰減掉，
// 同一個 alpha 下硬邊看起來會重得多。
constexpr int kHardShadowAlpha = 64;
constexpr int kSoftShadowAlpha = 96;

double clampd(double value, double lo, double hi) { return std::min(std::max(value, lo), hi); }

// 橢圓上的局部座標系。角度 phi 從「尾巴那一側的正中」量起：
//   point(phi) = (cx + a·sin(phi), cy + s·b·cos(phi))
// s ＝ +1 時 phi=0 是橢圓底部（氣泡在角色上方，尾巴朝下），-1 時是頂部。
struct Frame {
  double cx = 0;
  double cy = 0;
  double a = 1;
  double b = 1;
  double s = 1;
  double lean = 0;  // 接點的水平偏移比例（已夾限），-1..1
  double phi = 0;   // 接點角度
  double nx = 0;    // 接點處的向外單位法線
  double ny = 0;
  double vx = 0;  // 沿著接口的單位切向（與法線正交）
  double vy = 0;

  BubblePoint at(double angle) const { return {cx + a * std::sin(angle), cy + s * b * std::cos(angle)}; }
};

Frame frameOf(const Rect& ellipse, double tailX, BubbleSide side) {
  Frame f;
  // 退化成零尺寸時所有公式都會除以 0；夾成 1 px 讓它畫出一個看得見的點，
  // 而不是丟出 NaN 座標讓 QPainterPath 整條路徑失效
  f.a = std::max(ellipse.width / 2, 1.0);
  f.b = std::max(ellipse.height / 2, 1.0);
  f.cx = ellipse.x + ellipse.width / 2;
  f.cy = ellipse.y + ellipse.height / 2;
  f.s = side == BubbleSide::Above ? 1.0 : -1.0;

  const double maxLean = std::sin(kMaxAttachAngle);
  f.lean = clampd((tailX - f.cx) / f.a, -maxLean, maxLean);
  f.phi = std::asin(f.lean);

  // 橢圓在 phi 處的向外法線 ∝ (b·sin(phi), s·a·cos(phi))
  const double nx = f.b * std::sin(f.phi);
  const double ny = f.s * f.a * std::cos(f.phi);
  const double len = std::hypot(nx, ny);
  f.nx = nx / len;
  f.ny = ny / len;
  // 切向由法線轉 90 度得到，再乘 s —— 上下翻面時整個尾巴要鏡像，
  // 鉤子才會留在同一側而不是跟著翻到裡面去
  f.vx = -f.ny * f.s;
  f.vy = f.nx * f.s;
  return f;
}

double tailLengthOf(double a, double b) {
  const double mouth = 2 * clampd(kMouthRatio * a, kMouthMinHalf, kMouthMaxHalf);
  return clampd(std::min(kTailPerMouth * mouth, kTailOfSemiMinor * b), kTailMinLength, kTailMaxLength);
}

double dotBaseRadiusOf(double a) { return clampd(kDotBaseRatio * a, kDotMinRadius, kDotMaxRadius); }

// 圓點串在尾巴局部座標裡的位置（along ＝ 沿法線，lateral ＝ 側偏）與半徑。
// bubbleTailExtent() 與 thoughtDots() 必須吃同一支 —— 各算一份的話餘裕與實際
// 位置會慢慢對不上，最後那顆就被視窗裁掉。
//
// **間距是照 2D 的實際距離排的，不是照 along**：每一步的長度直接就是
// 「兩顆半徑 ＋ kDotGap」。照 along 等距排過，但側偏會把最後一段拉開 ——
// 實測最後兩顆的實際間隙變成前兩顆的 2.1 倍（2.06 vs 0.96 個半徑），
// 畫面上就是「最小那顆離得特別遠」。
struct DotStep {
  double along = 0;
  double lateral = 0;
  double radius = 0;
};

std::array<DotStep, 3> dotSteps(double a, double turn) {
  const double base = dotBaseRadiusOf(a);
  std::array<DotStep, 3> steps{};

  // 第一顆正對著接點（沿法線），之後每一步的方向再多轉一個 turn
  double along = base * (kDotFirstGap + kDotShrink[0]);
  double lateral = 0;
  steps[0] = {along, lateral, base * kDotShrink[0]};

  for (size_t i = 1; i < steps.size(); ++i) {
    const double radius = base * kDotShrink[i];
    // 步長 ＝ 兩顆的半徑加上要看到的間隙，所以 kDotGap 就是眼睛看到的那個間隙
    const double step = steps[i - 1].radius + radius + base * kDotGap;
    const double angle = turn * static_cast<double>(i);
    along += step * std::cos(angle);
    lateral += step * std::sin(angle);
    steps[i] = {along, lateral, radius};
  }
  return steps;
}

// 內部座標 phi → QPainterPath::arcTo 的角度（度，3 點鐘為 0、逆時針為正）。
// s=+1： theta = phi - 90°；s=-1： theta = 90° - phi。兩者都是從
// point(phi) 與 Qt 的 (cx + a·cos(theta), cy - b·sin(theta)) 對消出來的。
double arcDegrees(const Frame& f, double phi) {
  const double deg = phi * 180.0 / kPi;
  return f.s > 0 ? deg - 90.0 : 90.0 - deg;
}

}  // namespace

double bubbleTailExtent(BubbleStyle style, const BubbleSize& ellipse) {
  const double a = std::max(ellipse.width / 2, 1.0);
  const double b = std::max(ellipse.height / 2, 1.0);

  if (style == BubbleStyle::Speech) {
    // 貝茲曲線落在控制點的凸包裡，而沿法線最遠的是尾尖（接點外剛好 L，
    // 側偏那一項與法線正交所以不計）；接點本身最遠也只到橢圓外框的邊上。
    return tailLengthOf(a, b) + kOutlineMargin;
  }

  // turn 給 0：不轉向的那一組沿法線走得最遠（每一步都 cos(角度) ≤ 1），
  // 所以它就是餘裕的上界
  const std::array<DotStep, 3> steps = dotSteps(a, 0);
  return steps.back().along + steps.back().radius + kOutlineMargin;
}

BubbleShadowSpec bubbleShadowSpec(BubbleShadow shadow) {
  switch (shadow) {
    case BubbleShadow::Hard:
      return {kShadowOffset, 0, kHardShadowAlpha};
    case BubbleShadow::Soft:
      return {kShadowOffset, kShadowBlur, kSoftShadowAlpha};
    case BubbleShadow::Off:
      break;
  }
  return {};
}

double bubbleShadowExtent(BubbleShadow shadow) {
  const BubbleShadowSpec spec = bubbleShadowSpec(shadow);
  // 模糊往右下糊開的那一段疊在位移之上，所以是相加而不是取大的那個
  return spec.offset + spec.blur;
}

const std::vector<std::string>& bubbleShadowIds() {
  // 順序即為設定下拉選單由上到下的順序
  static const std::vector<std::string> ids{"off", "hard", "soft"};
  return ids;
}

BubbleShadow bubbleShadowFromId(const std::string& id) {
  if (id == "off") return BubbleShadow::Off;
  if (id == "soft") return BubbleShadow::Soft;
  return BubbleShadow::Hard;
}

const char* bubbleShadowId(BubbleShadow shadow) {
  switch (shadow) {
    case BubbleShadow::Off:
      return "off";
    case BubbleShadow::Soft:
      return "soft";
    case BubbleShadow::Hard:
      break;
  }
  return "hard";
}

SpeechTail speechTail(const Rect& ellipse, double tailX, BubbleSide side, bool mirrored) {
  const Frame f = frameOf(ellipse, tailX, side);
  const double length = tailLengthOf(f.a, f.b);
  // 鏡射只翻兩件事：尾尖的側偏、以及中線的彎向。接點與 w（baseA → baseB）不動 ——
  // 兩條邊的偏移量因此原封不動，「永遠不交叉」那個保證跟著保住。
  // 結果剛好是原圖對接點鉛直線的鏡像（接口本身對接點對稱）。
  const double flip = mirrored ? -1.0 : 1.0;
  const double halfMouth = clampd(kMouthRatio * f.a, kMouthMinHalf, kMouthMaxHalf);
  // 接口的角寬度由「要多寬的嘴巴」反推：弧長 ≈ a·cos(phi)·dphi
  const double half = clampd(halfMouth / (f.a * std::cos(f.phi)), 0.08, 0.55);

  const BubblePoint mouthMid = f.at(f.phi);
  SpeechTail tail;
  tail.mouthMid = mouthMid;
  tail.baseA = f.at(f.phi - half);
  tail.baseB = f.at(f.phi + half);
  tail.tip = {mouthMid.x + f.nx * length - flip * f.vx * kTipHook * length, mouthMid.y + f.ny * length - flip * f.vy * kTipHook * length};

  // 尾巴自己的座標系：軸是「接口中點 → 尾尖」，w 與它正交並指向 baseB 那一側
  double axisX = tail.tip.x - mouthMid.x;
  double axisY = tail.tip.y - mouthMid.y;
  const double axisLength = std::max(std::hypot(axisX, axisY), 1e-6);
  axisX /= axisLength;
  axisY /= axisLength;
  double wx = -axisY;
  double wy = axisX;
  if ((tail.baseB.x - tail.baseA.x) * wx + (tail.baseB.y - tail.baseA.y) * wy < 0) {
    wx = -wx;
    wy = -wy;
  }

  // 兩條邊掛在**同一個**中線控制點上，對稱地各偏 kTailEdge 個接口半寬。
  // 於是 edge1(t) − edge2(1−t) 化簡成 −2h·w·[(1−t)² + 2t(1−t)·kTailEdge]，
  // 括號裡在 t<1 恆為正 —— 交叉在數學上就不可能發生，而寬度只由 kTailEdge
  // 決定，跟彎多少無關。
  const double centreX = mouthMid.x + axisX * axisLength * 0.5 + wx * flip * kTailBend * halfMouth;
  const double centreY = mouthMid.y + axisY * axisLength * 0.5 + wy * flip * kTailBend * halfMouth;
  tail.ctrlA = {centreX - wx * kTailEdge * halfMouth, centreY - wy * kTailEdge * halfMouth};
  tail.ctrlB = {centreX + wx * kTailEdge * halfMouth, centreY + wy * kTailEdge * halfMouth};

  // 圓弧從 baseB 出發，繞開接口那一段（2·half）回到 baseA
  tail.arcStartDeg = arcDegrees(f, f.phi + half);
  const double spanDeg = (2 * kPi - 2 * half) * 180.0 / kPi;
  tail.arcSpanDeg = f.s > 0 ? spanDeg : -spanDeg;
  return tail;
}

std::vector<ThoughtDot> thoughtDots(const Rect& ellipse, double tailX, BubbleSide side, bool mirrored) {
  const Frame f = frameOf(ellipse, tailX, side);
  const double turn = (mirrored ? -1.0 : 1.0) * kDotTurn * (1.0 - std::abs(f.lean));
  const std::array<DotStep, 3> steps = dotSteps(f.a, turn);
  const BubblePoint mid = f.at(f.phi);

  std::vector<ThoughtDot> dots;
  dots.reserve(steps.size());
  for (const DotStep& step : steps) {
    ThoughtDot dot;
    dot.radius = step.radius;
    dot.center = {mid.x + f.nx * step.along - f.vx * step.lateral, mid.y + f.ny * step.along - f.vy * step.lateral};
    dots.push_back(dot);
  }
  return dots;
}

}  // namespace l2m
