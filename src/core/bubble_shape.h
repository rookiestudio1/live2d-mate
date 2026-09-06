#pragma once

// 氣泡的外型幾何：橢圓本體、月牙尾巴、思考泡泡的圓點串。
//
// 為什麼從 windows/bubble_window.cpp 拆出來：這裡沒有一行碰 Qt，而算錯一格的
// 後果全是只有肉眼才抓得到的 —— 尾巴接在「橢圓底邊那條直線」上（橢圓沒有底邊，
// 接點偏離正中時尾巴會浮在半空）、外擴的餘裕少算一格就把尾尖裁掉、
// 橢圓尺寸算錯就排成一顆又扁又長的橢圓。純函數化才驗得到。
//
// 尺寸關係，改動前先讀懂：
//
// ① **橢圓的大小不在這裡算**。以前是「量一個矩形 → 半徑各乘 √2 外接」，
//    四個角白白浪費 36% 的面積。現在文字是照著橢圓的形狀逐行排的
//    （上下短、中間長），大小由 core/ellipse_text_fit.h 的收斂搜尋決定；
//    這一支只吃算好的橢圓尺寸，負責把尾巴與圓點串掛上去。
//
// ② **視窗的上下餘裕是不對稱的**。尾巴那一側要留滿尾巴的長度，另外三邊只要
//    留給外框。placeBubble() 把「視窗邊緣」擺在離角色 kBubbleGap 的位置，所以
//    尾巴側的餘裕剛好等於尾巴長度時，尾尖就正好停在離角色 kBubbleGap 的地方
//    —— 大小氣泡自動貼合，不必再為「短句氣泡飄太遠」補特例。
//
// bubbleTailExtent() 刻意**只吃外型與橢圓尺寸、不吃 tailX**：量測要在
// placeBubble() 之前跑完（尾巴的水平位置是它算出來的），視窗大小不能反過來
// 依賴它。所以尾巴的伸出量一律取與接點角度無關的保守上界。

#include <string>
#include <vector>

#include "bubble_placement.h"

namespace l2m {

// 氣泡外型。Speech ＝ 橢圓＋月牙尾巴，Thought ＝ 橢圓＋一串遞減的小圓。
enum class BubbleStyle { Speech, Thought };

struct BubblePoint {
  double x = 0;
  double y = 0;
};

// 月牙尾巴。畫法是**一條連續的封閉路徑**：從 baseB 沿橢圓繞一整圈到 baseA，
// 再走兩段二次貝茲 baseA → tip → baseB 收口。
// 不用「橢圓 ∪ 尾巴」的布林聯集是因為兩者本來就相切，聯集會在切點留下毛邊。
struct SpeechTail {
  // 尾巴的錨點：接口正中、在橢圓上。整條尾巴都是從這裡長出去的，鏡射的軸也
  // 通過它。**不是** baseA/baseB 的中點 —— 那個中點是弦的中點，對橢圓來說
  // 沿著半徑方向縮進來了一點。
  BubblePoint mouthMid;
  BubblePoint baseA;  // 圓弧的終點，也是尾巴的起點
  BubblePoint ctrlA;  // baseA → tip 的控制點（外緣，凸的那一側）
  BubblePoint tip;    // 尾尖
  BubblePoint ctrlB;  // tip → baseB 的控制點（內緣，凹的那一側，月牙的來源）
  BubblePoint baseB;  // 圓弧的起點，也是尾巴的終點

  // 本體那一段圓弧，**以 QPainterPath::arcTo 的慣例表示**：角度單位是度，
  // 3 點鐘方向為 0、逆時針為正（y 軸向上）。直接餵給 arcMoveTo/arcTo 就對了。
  double arcStartDeg = 0;
  double arcSpanDeg = 0;
};

struct ThoughtDot {
  BubblePoint center;
  double radius = 0;
};

// 尾巴／圓點串在橢圓之外還要佔掉多少（px）。視窗在尾巴那一側要留這麼多餘裕。
double bubbleTailExtent(BubbleStyle style, const BubbleSize& ellipse);

// ── 陰影 ──
// 氣泡的投影樣式，對應設定 tts.bubbleShadow 的三檔字串。Hard ＝ 硬邊的位移剪影
//（漫畫印刷就是這樣印的），Soft ＝ 糊過一圈的一般 UI 投影。
enum class BubbleShadow { Off, Hard, Soft };

// 陰影的幾何與濃度。offset 是往右下的位移，blur 是模糊半徑（Hard 為 0），
// alpha 是墨色的不透明度（0..255）。
struct BubbleShadowSpec {
  double offset = 0;
  double blur = 0;
  int alpha = 0;
};

BubbleShadowSpec bubbleShadowSpec(BubbleShadow shadow);

// 視窗要在**右邊與下邊**各多留多少 px，陰影才不會被裁掉。
//
// 只有右下要留，是因為 blur 一律 ≤ offset：模糊往左上糊開的那一段剛好被位移吃掉，
// 所以左邊與上邊的既有餘裕（尾巴那一側只有 kOutlineMargin ＝ 4 px，是四邊裡最薄的）
// 完全不必加厚。這條不變量由 shadowNeverSpillsPastTheExistingMargin() 釘住（不過它只驗得到
// 這裡的邏輯像素；換算成裝置像素的那一步一律無條件捨去才守得住，理由寫在
// windows/bubble_window.cpp 的 boxBlur 呼叫處）——
// 破了的症狀是「氣泡上緣或尾尖的影子被切掉一條直線」。
double bubbleShadowExtent(BubbleShadow shadow);

// config 的字串（"off" / "hard" / "soft"）與列舉互轉。**這份清單就是 schema 的允許值**
// —— config_schema.cpp 的 readEnum 直接吃它，兩邊各寫一份的下場是設定存得進去卻畫不出來。
// 認不得的字串一律回 Hard，也就是預設值。
const std::vector<std::string>& bubbleShadowIds();
BubbleShadow bubbleShadowFromId(const std::string& id);
const char* bubbleShadowId(BubbleShadow shadow);

// ellipse 是橢圓的外接矩形（視窗座標），tailX 是接點在同一座標系的 x（指回角色
// 中心），side 決定尾巴朝下（Above ＝ 氣泡在角色上方）還是朝上（Below）。
//
// mirrored ＝ 左右鏡射，由 placeBubble 依角色在螢幕的哪一半決定（BubblePlacement
// ::mirrored）。氣泡被往螢幕中心推開之後，角色落在氣泡的外側，尾巴要往那邊斜
// 才指得回去，所以「往哪邊推」與「往哪邊斜」一定是同一個布林值，不能各判各的。
SpeechTail speechTail(const Rect& ellipse, double tailX, BubbleSide side, bool mirrored = false);
std::vector<ThoughtDot> thoughtDots(const Rect& ellipse, double tailX, BubbleSide side, bool mirrored = false);

}  // namespace l2m
