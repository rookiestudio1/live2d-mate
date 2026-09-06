#pragma once

// moc3 畫布中心相對「模型原點」的位移 —— 也就是 draw() 把模型擺正要補的那一段。
//
// 原點是作者在 Cubism 編輯器裡擺的那個十字，**不保證在畫布正中央**：實測有擺在
// 腳底的（Reverse:1999 的 300301_hujisheng，原點在畫布高度的 94%），也有擺在頭頂
// 附近的（lumine 16%、LiveroiD 20%、haihukonchan 20%）。而官方範例的投影只做
// 「把畫布縮到視窗大小」不做平移，於是原點會被釘在視窗正中央 —— 原點在腳底的
// 模型上半身整個跑到視窗上緣外面（症狀是「只看得到下半身」），原點在頭頂的則反過來
// 被切掉腳。手邊 23 個 moc3 有 19 個剛好在正中央，所以這件事很容易一直沒被發現。
//
// 抽成純函式是因為**正負號錯一個就是把模型往壞的那一邊再推一次**，而
// l2m_live2d 連不進測試（見 CLAUDE.md 最重要的那條規則）。有兩件事只有寫成
// 純函式才驗得到：
//
//   1. **像素座標的 Y 是由上往下量的**，模型座標的 Y 卻是向上 —— 換算時要反過來。
//      驗證方式是拿 Cubism Core 讀出可見 drawable 的頂點 Y 範圍跟兩種解讀比對：
//      頂點必然落在畫布內，所以哪一種成立是可判定的（lumine 實測頂點 Y ∈
//      [-1.34, 0.22]，只有「Y 由上往下」推出的畫布 [-1.34, 0.26] 容得下它）。
//   2. **原點在正中央時必須是精確的 0**，不是「很接近 0」—— 絕大多數模型走這條，
//      浮點誤差漏出來就是每一隻模型的構圖都跟改動前差一點點。
//
// 呼叫端：live2d/model_controller.cpp 的 setupModel()（跟 Core 要數字）與 draw()
// （每幀乘上 fit 的縮放之後寫進 _modelMatrix 的平移）。

namespace l2m {

// 畫布中心在模型座標裡的位置（模型單位，X 向右、Y 向上）。
// 原點就在畫布正中央時是 {0, 0}，也就是「什麼都不必補」。
struct CanvasOffset {
  double x = 0;
  double y = 0;
};

// csmReadCanvasInfo() 的三組輸出 → 畫布中心相對原點的位移。
// canvasWidthPx／canvasHeightPx 是畫布尺寸，originXPx／originYPx 是原點在畫布上的
// 像素位置（X 由左往右、**Y 由上往下**），pixelsPerUnit 是每個模型單位幾個像素。
//
// pixelsPerUnit 不是正的有限數（讀不到 canvas info 時會是 0）就回 {0, 0} ——
// 那代表「不知道怎麼擺」，而維持原樣至少跟改動前一模一樣。
CanvasOffset canvasCenterOffset(double canvasWidthPx, double canvasHeightPx, double originXPx, double originYPx, double pixelsPerUnit);

}  // namespace l2m
