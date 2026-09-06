#include "bubble_placement.h"

#include <algorithm>
#include <cmath>

namespace l2m {

namespace {

double clamp(double value, double lo, double hi) { return std::min(std::max(value, lo), hi); }

}  // namespace

Rect bubbleAnchorRect(const Rect& window, double topNormalized, double bottomNormalized) {
  if (window.height <= 0) return window;
  if (!std::isfinite(topNormalized) || !std::isfinite(bottomNormalized)) return window;

  const double top = clamp(topNormalized, 0.0, 1.0);
  const double bottom = clamp(bottomNormalized, 0.0, 1.0);
  // 顛倒或太薄都當成量測失敗 —— 遮罩抓到一條細縫時，照著擺會把氣泡釘在半空中
  if (bottom - top < kMinModelExtent) return window;

  Rect anchor = window;
  anchor.y = window.y + top * window.height;
  anchor.height = (bottom - top) * window.height;
  return anchor;
}

BubblePlacement placeBubble(const Rect& character, const BubbleSize& size, const Rect& area, double gap, double offsetY, double scale) {
  // 模型被 fit() 置中在角色視窗裡，所以視窗中心就是角色中心
  const double centerX = character.x + character.width / 2;

  // 角色在螢幕的哪一半決定氣泡往哪邊挪、尾巴往哪邊斜（見 kBubbleSideShift）。
  // 邊界用「<=」：正中央時算左半邊，跟 side 的邊界判斷一樣取一個確定的答案，
  // 免得角色停在正中央時左右抖動。
  const double screenCenterX = area.x + area.width / 2;
  const bool mirrored = centerX <= screenCenterX;
  const double shift = kBubbleSideShift * std::max(scale, 0.0) * (mirrored ? 1.0 : -1.0);

  // 夾限在位移之後 —— 先挪再夾，貼到螢幕邊緣時位移自然被吃掉
  const int x = static_cast<int>(std::lround(clamp(centerX - size.width / 2 + shift, area.x, area.x + std::max(0.0, area.width - size.width))));

  // offsetY 直接加在間距上，上下兩側就自動鏡像成「與角色的距離」。
  // 下界夾在 -character.height：再往下就穿過角色到另一側了，那時 side 會與實際
  // 位置相反、尾巴朝錯邊。這個界限剛好對稱 —— 上方氣泡的底邊最多壓到角色底端，
  // 下方氣泡的頂邊最多推到角色頂端。
  const double gapPx = std::max(gap + offsetY, -character.height);

  // 垂直：優先放上方（不擋臉）；上方塞不下才翻到角色下方
  const double above = character.y - gapPx - size.height;
  const BubbleSide side = above >= area.y ? BubbleSide::Above : BubbleSide::Below;
  const int y = static_cast<int>(std::lround(clamp(side == BubbleSide::Above ? above : character.y + character.height + gapPx, area.y, area.y + std::max(0.0, area.height - size.height))));

  // 尾巴的接點**固定在氣泡正中**，不跟著角色跑。
  // 曾經讓它追著角色（氣泡貼到螢幕邊緣時尾巴還指得回去），但那會把
  // kBubbleSideShift 的效果吃掉一半：氣泡往外側挪、接點卻往回追，尾巴的斜度
  // 當場被抵銷，看起來就不像從嘴巴噴出來 —— 要補回同樣的斜度得把視窗挪得更遠。
  // 斜度現在完全由尾巴自己的側偏（bubble_shape.h 的 kTipHook）與 mirrored 提供。
  const int tailX = static_cast<int>(std::lround(size.width / 2));

  return {x, y, side, tailX, mirrored};
}

}  // namespace l2m
