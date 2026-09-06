#pragma once

// model3.json 的 `Layout` 到底能不能信 —— 「照作者擺」還是「退回自動 fit + 置中」。
//
// ── 為什麼需要判斷「能不能信」──
//
// `draw()` 本來的規則是「作者寫了 Layout 就一步都不碰」（見 model_controller.h 的
// `layoutSpecifiesSize_`／`layoutSpecifiesPosition_`）。那條規則假設 Layout 是作者
// 對這隻模型的構圖，但實務上不是：手邊 29 隻模型只有 2 隻有 Layout，而**那 2 隻正是
// 位置壞掉的那 2 隻**（《原神》可莉與派蒙，都是從別的桌寵程式搬過來的）。
//
//   可莉 {"height": 2.6, "bottom": 2.0, "top": 0.3}
//   派蒙 {"height": 2.2, "bottom": 2.3}
//
// 根因不在數值大小，而是 **`CubismModelMatrix` 的位置算式假設原點在畫布角落**：
// `Bottom(y)` 是 `TranslateY(y - 畫布高)`、`CenterY(y)` 是 `TranslateY(y - 畫布高/2)`,
// 兩者都把「原點」當成畫布的某個邊。但現代模型的原點多半在畫布正中央（實測 23 隻
// moc3 有 19 隻剛好在中央，見 core/canvas_center.h），於是同一條算式會把畫布整個
// 推掉半個身子 —— 例如原點在中央、`center_y: 0` 算出來的畫布範圍是 [-2, 0] 而不是
// [-1, 1]。可莉還同時寫了 `top` 與 `bottom` 兩個互斥的錨點，Framework 照 map 順序
// 兩個都套、後面那個無聲地蓋掉前面那個。
//
// 所以判別方式是**看結果而不是看有沒有寫**：把 `SetupFromLayout()` 的算式原樣重跑
// 一遍，算出畫布在 view 座標的矩形，落在標準視野 [-1,1]² 之內才採用。
// 這同時也是「Framework 的 Layout 算式對這隻模型成不成立」的檢查。
//
// **判別刻意與視窗長寬比無關**：拿當下的視窗去算的話，同一隻模型會在拖動視窗邊緣的
// 過程中在兩種構圖之間跳來跳去，而且檢視器與桌寵會給出不同的答案 ——「檢視器裡看到的
// 就是設定成桌寵之後會看到的」是 Viewer 的前提。
//
// 對得起來的不變式是：**`draw()` 保證 [-1,1]² 這塊一定看得見**（採用 Layout 時
// 橫向視窗以高度為準、直向以寬度為準），所以「Layout 的結果落在我們保證看得見的
// 範圍內」才採用。橫幅畫布（寬 > 高）配 Layout 因此一定會被否決 —— 它本來就不可能
// 塞進正方形視野，退回依視窗長寬比挑受限維度的 fit 才是對它好的結果。
//
// 純函式的理由同 core/canvas_center.h：`l2m_live2d` 連不進測試，而這裡每一條分支
// 錯了都只會表現成「某一隻模型的構圖怪怪的」，沒有任何錯誤訊息。
//
// ── 兩個已知的取捨 ──
//
// **① 判別的是「畫布」在不在視野內，不是「模型」在不在**。畫布常常帶大片留白，
// 所以「作者刻意放大、把留白裁掉」的寫法（例如只有 `{"height": 2.4}`）也會被否決。
// 刻意不改用 `ModelController::visibleBoundsView()` 的可見外接框：那個框會把畫布外
// 的裝飾用 drawable 一起算進去（實測派蒙有一片 `ArtMesh13` 佔到畫布寬的 286%），
// 拿它去 fit 會讓角色縮成一小點；而且它隨動作變動，用來當構圖的判準會抖。
// 手邊 29 隻模型只有 2 隻有 Layout 且兩隻都是壞的，這個取捨目前沒有代價 ——
// 真的遇到「作者刻意放大卻被否決」的模型時，再回來想（`qWarning` 會說出是哪一隻）。
//
// **② 起始縮放的假設只在有寫 `width`／`height` 時完全成立**。這裡一律以
// `CubismModelMatrix` 建構子的 `SetHeight(2.0)` 當起點，但 `draw()` 只在
// `specifiesSize` 為真時保留那個縮放；**只寫位置沒寫大小**的 Layout 會落到 fit 分支被
// `SetWidth`／`SetHeight(2.0)` 覆寫縮放、卻保留 `SetupFromLayout` 算出的平移，
// 等於「用 A 縮放算的平移」配上「B 縮放」。這個不對稱在本檔案出現之前就存在，
// 觸發條件也很窄（要原點偏離中心**且**畫布長寬比大於視窗），先記在這裡。

#include <string>
#include <vector>

namespace l2m {

// Layout 的一個鍵值。**要照 model3.json 裡的出現順序**放進 vector ——
// Framework 是照 map 的順序套的，同一類的鍵後面會蓋掉前面（可莉的 top 蓋掉 bottom）。
struct LayoutEntry {
  std::string key;
  double value = 0;
};

struct LayoutPlan {
  // 有沒有 width／height（決定 draw() 要不要做每幀強制 fit）
  bool specifiesSize = false;
  // 有沒有 x／y／center_x／center_y／top／bottom／left／right（決定要不要自動置中）
  bool specifiesPosition = false;
  // 套完之後畫布還在標準視野內。false = 整份 Layout 都不採用，退回 fit + 置中。
  // 沒有 Layout、或算不出來（畫布尺寸不合法）時是 true —— 那代表「沒有東西要否決」。
  bool usable = true;
};

// canvasWidthUnits／canvasHeightUnits：畫布尺寸（模型單位，也就是像素 ÷ PixelsPerUnit）
// canvasCenterX／canvasCenterY：畫布中心相對模型原點的位移（見 core/canvas_center.h）
//
// 畫布尺寸不是正的有限數時直接回 {false, false, true} —— 不知道就別亂動，
// 維持跟改動前一模一樣的行為。
LayoutPlan planLayout(const std::vector<LayoutEntry>& layout, double canvasWidthUnits, double canvasHeightUnits, double canvasCenterX, double canvasCenterY);

}  // namespace l2m
