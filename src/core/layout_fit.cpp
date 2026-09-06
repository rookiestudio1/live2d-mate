#include "layout_fit.h"

#include <cmath>

namespace l2m {

namespace {

// 標準視野的半徑。Cubism 的慣例是 view 座標 [-1, 1]，`SetHeight(2.0f)` 就是「填滿」。
constexpr double kViewHalf = 1.0;
// 剛好貼齊邊界的 Layout（例如 {"height": 2.0}）要算「有落在裡面」，
// 不然浮點誤差會讓最常見的那種寫法被否決掉。
constexpr double kEpsilon = 1e-6;

bool isPositionKey(const std::string& key) { return key == "x" || key == "y" || key == "center_x" || key == "center_y" || key == "top" || key == "bottom" || key == "left" || key == "right"; }

}  // namespace

LayoutPlan planLayout(const std::vector<LayoutEntry>& layout, double canvasWidthUnits, double canvasHeightUnits, double canvasCenterX, double canvasCenterY) {
  LayoutPlan plan;
  if (layout.empty()) return plan;

  for (const LayoutEntry& entry : layout) {
    if (entry.key == "width" || entry.key == "height") plan.specifiesSize = true;
    if (isPositionKey(entry.key)) plan.specifiesPosition = true;
  }

  const bool sizeUsable = std::isfinite(canvasWidthUnits) && std::isfinite(canvasHeightUnits) && canvasWidthUnits > 0 && canvasHeightUnits > 0;
  if (!sizeUsable) return plan;  // 算不出來就別否決

  // ── 以下是 CubismModelMatrix::SetupFromLayout() 的算式原樣重跑 ──
  //
  // 起始縮放是建構子的 SetHeight(2.0f)，不是 1 —— 只寫了位置沒寫大小的 Layout
  // 就是從這個值出發的，漏掉的話那一類會整組算錯。
  double scale = 2.0 / canvasHeightUnits;
  // 第一輪只看大小，第二輪只看位置（Framework 就是分兩輪跑的，
  // 因為位置的算式要用到已經定案的縮放）
  for (const LayoutEntry& entry : layout) {
    if (!std::isfinite(entry.value)) continue;
    if (entry.key == "width") scale = entry.value / canvasWidthUnits;
    if (entry.key == "height") scale = entry.value / canvasHeightUnits;
  }
  if (!std::isfinite(scale) || scale <= 0) return plan;

  // 縮放後的畫布尺寸（view 單位）。Framework 的位置算式吃的是這兩個值。
  const double scaledWidth = canvasWidthUnits * scale;
  const double scaledHeight = canvasHeightUnits * scale;
  double translateX = 0;
  double translateY = 0;
  for (const LayoutEntry& entry : layout) {
    if (!std::isfinite(entry.value)) continue;
    const double v = entry.value;
    // 每一個都是**絕對指派**（Framework 的 TranslateX/Y 直接寫進矩陣），
    // 所以同一軸上寫了兩個鍵時後面那個說了算 —— 可莉的 top 就是這樣蓋掉 bottom 的
    if (entry.key == "x" || entry.key == "left") translateX = v;
    if (entry.key == "y" || entry.key == "top") translateY = v;
    if (entry.key == "center_x") translateX = v - scaledWidth / 2.0;
    if (entry.key == "center_y") translateY = v - scaledHeight / 2.0;
    if (entry.key == "right") translateX = v - scaledWidth;
    if (entry.key == "bottom") translateY = v - scaledHeight;
  }

  // 畫布中心在 view 座標的位置：平移（原點的落點）+ 畫布中心相對原點的位移 × 縮放。
  // **這一項就是 Framework 少算的那一塊** —— 它的位置算式把原點當成畫布的邊，
  // 原點在正中央的模型於是整個被推掉半個畫布。
  const double centerX = translateX + canvasCenterX * scale;
  const double centerY = translateY + canvasCenterY * scale;
  const double halfWidth = scaledWidth / 2.0;
  const double halfHeight = scaledHeight / 2.0;

  plan.usable = std::abs(centerX) + halfWidth <= kViewHalf + kEpsilon && std::abs(centerY) + halfHeight <= kViewHalf + kEpsilon;
  return plan;
}

}  // namespace l2m
