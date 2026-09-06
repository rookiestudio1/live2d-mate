#include "canvas_center.h"

#include <cmath>

namespace l2m {

CanvasOffset canvasCenterOffset(double canvasWidthPx, double canvasHeightPx, double originXPx, double originYPx, double pixelsPerUnit) {
  if (!std::isfinite(pixelsPerUnit) || pixelsPerUnit <= 0) return {};
  if (!std::isfinite(canvasWidthPx) || !std::isfinite(canvasHeightPx) || !std::isfinite(originXPx) || !std::isfinite(originYPx)) return {};

  // X：像素與模型座標同向（往右為正），所以是「畫布中心 − 原點」。
  // Y：像素往下、模型往上，方向相反，所以是「原點 − 畫布中心」。
  // 兩條都寫成 (a/2 - b) 而不是 (a - 2b)/2 之類的等價式：原點剛好在正中央時
  // canvasWidthPx / 2 與 originXPx 是同一個位元組合，相減必然是精確的 0。
  return CanvasOffset{(canvasWidthPx / 2 - originXPx) / pixelsPerUnit, (originYPx - canvasHeightPx / 2) / pixelsPerUnit};
}

}  // namespace l2m
