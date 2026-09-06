#pragma once

// 由 alpha 遮罩量「模型在視窗裡的視覺上下緣」，以及切換模型時的「腳底對齊」計算。
//
// 每個模型的 moc3 畫布比例與 model3.json Layout 都不同，同一個 400×600 舞台裡
// 角色腳底落在不同高度 —— 換模型後角色會懸空或陷進工作列。這裡把「模型視覺
// 上下緣在哪」與「視窗該移去哪」的計算抽成純函式：量測資料來自 AlphaHitMask 的
// alpha 回讀，但計算本身不碰 GUI / GL，單元測試才跑得動（同 bubble_placement）。
//
// 兩個消費端：**腳底對齊**吃底緣（切模型時把新模型的腳底擺回舊模型的高度），
// **氣泡錨點**吃頂緣（氣泡貼的是模型頭頂而不是視窗頂端，
// 見 core/bubble_placement.h 的 bubbleAnchorRect）。
//
// 兩個必須知道的約定：
//   1. 量的是「未膨脹」的原始 alpha（AlphaHitMask::pixels_），不是 maskBits_ ——
//      後者為了點擊穿透外擴了 kDilate ＝ 4 格（1 格 ≈ 2.6px，即約 10.4px），
//      會把上下緣各往外推那麼多。
//   2. GL 回讀的像素上下顛倒（原點在左下）：buffer 的 row 0 就是視窗最底下
//      一列，所以「從 row 0 往上掃到的第一個不透明列」即視覺最低點，
//      反過來從最後一列往下掃到的才是視覺最高點。

#include <cstddef>
#include <cstdint>
#include <optional>

namespace l2m {

// 從 row 0 往上掃，回傳第一個含 alpha > alphaThreshold 像素的列索引；
// 全透明回 nullopt。rgba 為緊密或帶 padding 的 RGBA 緩衝（alpha 在每 4 bytes
// 的第 3 個），strideBytes 是一列的位元組數。
std::optional<int> firstOpaqueRow(const uint8_t* rgba, int width, int height, size_t strideBytes, int alphaThreshold);

// 從最後一列（row height-1，即視窗最頂）往下掃，回傳第一個含 alpha >
// alphaThreshold 像素的列索引；全透明回 nullopt。參數意義同 firstOpaqueRow。
std::optional<int> lastOpaqueRow(const uint8_t* rgba, int width, int height, size_t strideBytes, int alphaThreshold);

// bottom-up buffer 的列索引 → 該格「下緣」的 normalized Y（0=視窗頂，1=視窗底）。
// row 0 的下緣就是視窗最底（回 1.0）。
double bottomUpRowToNormalizedBottom(int row, int height);

// bottom-up buffer 的列索引 → 該格「上緣」的 normalized Y（0=視窗頂，1=視窗底）。
// 最後一列（row height-1）的上緣就是視窗最頂（回 0.0）。
double bottomUpRowToNormalizedTop(int row, int height);

// normalized 底部 → 視窗內像素偏移（自視窗頂端起算，lround）
int bottomOffsetPx(double bottomNormalized, int windowHeightPx);

// 由「目標底部螢幕 Y」與「新模型底部在視窗內的偏移」算視窗左上角 y。
// 新舊模型用同一套量測（同解析度、同門檻），半格的系統偏差會互相抵消。
inline int windowYForBottom(int targetBottomScreenY, int bottomOffsetInWindowPx) { return targetBottomScreenY - bottomOffsetInWindowPx; }

}  // namespace l2m
