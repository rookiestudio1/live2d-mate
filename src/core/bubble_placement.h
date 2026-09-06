#pragma once

// 氣泡視窗的定位計算。
//
// 刻意不依賴 GUI：這樣單元測試跑得動，也讓「氣泡該出現在哪」這件事
// 有一份可以單獨驗證的規則，而不是散在視窗程式碼裡。

namespace l2m {

// 矩形（螢幕座標）
struct Rect {
  double x = 0;
  double y = 0;
  double width = 0;
  double height = 0;
};

struct BubbleSize {
  double width = 0;
  double height = 0;
};

// 氣泡在角色的上方還是下方，決定尾巴朝下或朝上
enum class BubbleSide { Above, Below };

struct BubblePlacement {
  int x = 0;
  int y = 0;
  BubbleSide side = BubbleSide::Above;
  // 尾巴接在氣泡上的水平位置（px）。**固定是視窗正中**，不追著角色跑 ——
  // 追著跑會抵銷掉 kBubbleSideShift 的斜度，理由寫在 .cpp 的算式旁邊。
  int tailX = 0;
  // 尾巴（與思考泡泡的圓點串）要左右鏡射嗎。**角色在螢幕左半邊時為真** ——
  // 見下方 kBubbleSideShift 的說明，這兩件事一定要一起翻，否則氣泡往一邊挪、
  // 尾巴卻往另一邊甩，看起來會像尾巴接錯位置。
  bool mirrored = false;
};

// 氣泡視窗留給外框的餘裕（px）：左右兩側，以及**沒有尾巴的那一邊**。
// 尾巴那一邊留多少由 bubbleTailExtent()（core/bubble_shape.h）另外算 ——
// 兩邊一樣厚的話，短句氣泡的尾巴那側會多出一大段空白而看起來離角色太遠。
inline constexpr int kBubblePadding = 10;

// 角色與氣泡之間的空隙
inline constexpr int kBubbleGap = 6;

// 氣泡整體往螢幕外側挪多少（px，會再乘上模型比例）。
//
// 為什麼要挪：氣泡正上方垂直對齊角色時，尾巴幾乎是直的，看起來像從頭頂長出一根
// 天線，不像漫畫。把氣泡往旁邊推開，尾巴就得斜著往回指 —— 那個斜度才是漫畫感。
// 方向依角色在螢幕的哪一半決定：**往螢幕中心那一側推**，於是角色落在氣泡的外側、
// 尾巴往外側斜，順帶讓氣泡避開螢幕邊緣而不是撞上去。
// 乘模型比例是因為尾巴長度也是跟著氣泡走的：角色縮到 0.5 倍時固定挪 60 px
// 會歪過頭。
inline constexpr double kBubbleSideShift = 60;

// 視覺高度低於視窗的這個比例就當成量測失敗（見 bubbleAnchorRect）
inline constexpr double kMinModelExtent = 0.1;

// 模型視覺上下緣（normalized 0..1，0=視窗頂）→ 氣泡真正該貼齊的錨點矩形。
//
// 為什麼需要這一層：角色視窗是固定 400×600 比例的舞台，模型在裡面上下留白多少
// 由 moc3 畫布與 model3.json 的 Layout 決定，app 蓋不掉 —— 直接拿視窗頂端當錨點，
// 畫布上方留一大片空白的模型（全身立繪很常見）氣泡就會浮在頭頂老遠的地方。
// 上下緣由 AlphaHitMask 的 alpha 回讀量出來（core/bottom_align.h），在這裡換算成
// 「氣泡該貼哪」的矩形，於是 offsetY = 0 剛好是「氣泡底邊碰到頭頂」。
//
// **只動垂直方向**：水平仍然用整個視窗的中心。模型被 fit() 置中在視窗裡，
// 水平量測改不了多少，卻會讓角色左右擺動時氣泡跟著漂。
//
// 量測不可信時原樣回傳 window（退回舊行為）：值不是有限數、上下緣顛倒、
// 或量出來的高度不到視窗的 kMinModelExtent —— 遮罩剛好抓到一條細縫時
// 寧可擺得跟從前一樣，也不要把氣泡釘在半空中。
Rect bubbleAnchorRect(const Rect& window, double topNormalized, double bottomNormalized);

// offsetY：使用者可調的間距微調（px，對應設定裡的 tts.bubbleOffsetY）。
// 錨點是**模型的視覺上下緣**（bubbleAnchorRect 量出來的那個矩形），所以 0 就是
// 「氣泡底邊剛好碰到頭頂」；量不到那個矩形時退回角色視窗的頂端與底端。
// **這個語意改過一次**：從前錨的是視窗的頂端與底端，所以曾經拿這個值
// 手動補留白的設定要自己歸零，否則等於再往角色身上推一次。
// 語意刻意是「與角色的距離」而不是螢幕 Y 軸位移，所以上下鏡像：負值在上方是
// 往下靠近角色，翻到下方時是往上靠近角色，同一個值兩側手感一致。
// scale ＝ 模型比例（config 的 model.scale），只用在 kBubbleSideShift 的換算上。
BubblePlacement placeBubble(const Rect& character, const BubbleSize& size, const Rect& area, double gap = kBubbleGap, double offsetY = 0, double scale = 1);

}  // namespace l2m
