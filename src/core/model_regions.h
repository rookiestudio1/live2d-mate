#pragma once

// 「點在角色身上的哪裡」—— 沒有 HitAreas 的模型靠幾何補。
//
// model3.json 的 `HitAreas` 是 Cubism 規格裡唯一「作者標了哪一塊是頭」的地方，
// 但實務上幾乎沒人填：手邊 2956 個 model3.json 實測只有 34 個（1.1%）有非空的
// HitAreas，其餘連鍵都沒有、或寫成 `"HitAreas": []`。也就是說
// `ModelController::hitTest()` 對 99% 的模型永遠回空陣列，「摸頭要播 touch_head」
// 這件事光靠規格根本做不到。
//
// 所以改用另一份一定有的資料：模型此刻在畫面上的實際外接框（由 drawable 頂點
// 算出來，見 ModelController::visibleBoundsView），點擊落在框內的**相對高度**
// 就能粗分部位。座標一律是 `ModelController::screenToView()` 的 view 座標，
// 跟 hitTest 吃的是同一組數字 —— 兩條路才不會各用一套。
//
// 唯一不能寫死的是「頭佔多少」：全身立繪的頭只佔 1/4 上下，胸像（很多桌寵模型
// 是這種）的頭卻能佔到一半，同一個常數兩邊必有一邊錯得離譜。這裡拿外接框的
// 長寬比當代理 —— 全身立繪細長（h/w ≈ 2.5），胸像接近方形（h/w ≈ 1.1），
// headFractionFor() 就是那條換算。view 座標是等向的（模型空間本身等向，
// `CubismModelMatrix` 的縮放也是等比），所以長寬比直接相除就對。
//
// 判定不準的代價很小：最差就是摸頭播成了 touch_body，而在這之前那一下**本來
// 就是隨機播 wedding／mail**。所以這裡刻意不追求精準，只要方向對。

#include "hit_area_semantics.h"

namespace l2m {

// 角色在畫面上的外接框，view 座標（**y 軸向上**，+1 在上、-1 在下，
// 同 ModelController::screenToView 的輸出）。
struct ModelBox {
  double left = 0;
  double right = 0;
  double bottom = 0;
  double top = 0;

  double width() const { return right - left; }
  double height() const { return top - bottom; }
  bool valid() const { return width() > 0 && height() > 0; }
};

// 外接框長寬比（高/寬）→ 頭部佔全高的比例。
// 0.55 / aspect 夾在 [0.16, 0.5]：全身 2.5 → 0.22、半身 1.7 → 0.32、
// 胸像 1.1 → 0.50。上限 0.5 是因為再高就沒有身體可分了。
double headFractionFor(double aspect);

// 點（view 座標）落在哪個部位。框不合法時回 Unknown。
// 框外的點會被夾回框內 —— 剪影與外接框都有一格左右的誤差，
// 邊緣的一下判成 Unknown 會讓它掉回隨機動作，那比夾進來更糟。
//
// 分四段（頭之後的三段是把剩下的高度依 18% / 27% / 55% 切開）：
//   Head  → touch_head
//   Chest → touch_special（碧藍航線那個「特殊觸摸」就是胸口那一下）——
//           這一段刻意窄，理由見 .cpp 的 kChestShare
//   Body  → touch_body
//   Leg   → 極少模型有對應動作，最後會退回 Body 那一組
BodyPart regionAt(const ModelBox& box, double viewX, double viewY);

}  // namespace l2m
