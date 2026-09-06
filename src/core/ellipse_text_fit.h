#pragma once

// 把一段文字排進橢圓氣泡裡：上下短、中間長。
//
// 為什麼不是直接量一個矩形：矩形要內接進同軸橢圓，寬高各得乘 √2（見
// core/bubble_shape.h ①），也就是四個角整整浪費 36% 的面積。漫畫氣泡從來
// 不是這樣排的 —— 它的每一行都貼著輪廓，所以中間長、上下短。照形狀排之後
// 同一句話的橢圓小一圈，掛在桌寵旁邊才不會喧賓奪主。
//
// 為什麼在 core/：這裡沒有一行碰 Qt。斷行本身要字型，所以用 EllipseTextMeasure
// 把它注入進來（Qt 那邊是 QTextLayout，測試那邊是一個等寬假字型）。
// 剩下的全是純幾何與收斂邏輯，而那正是會錯又只有肉眼看得到的部分。
//
// 三件事，改動前先讀懂：
//
// ① **每一行是一條有厚度的帶子，不是一條線**。可用半寬要取帶子上下緣裡
//    「離中心較遠」的那一邊算，否則那一行的兩個角會戳出橢圓、壓在黑框上。
//
// ② **橢圓大小與行數互相決定**，這是一個環：橢圓越窄行數越多，行數越多
//    文字塊越高、上下那幾行又更窄。打斷的方式是「先從 1 行開始收斂出行數，
//    再看塞不塞得下；塞不下就把整顆橢圓放大一點重來」。行數的收斂是單調
//    遞增的（寬度只會變窄 → 行數只會變多），所以不會來回震盪。
//
// ③ **種子橢圓一定要比答案小**。這支只會放大不會縮小，種子開太大就直接把
//    那個尺寸交出去了。所以 seedFill 取的是一個沒有任何排版塞得到的填充率
//    （內距一定會吃掉一部分），寧可多跑幾輪放大 —— 種子只影響迭代次數，
//    不影響結果。
//
// 交出去的 lineWidths 是**契約**：呼叫端把它原封不動餵回同一個 measure，
// 一定會得到同樣的行數與行寬。少了這條，量出來的橢圓與畫出來的字就會是
// 兩件不同的事（Qt 會照自己拿到的寬度重新斷行）。

#include <cstddef>
#include <functional>
#include <vector>

#include "bubble_placement.h"

namespace l2m {

// 一趟斷行：吃「每一行允許的寬度」，還「每一行實際佔掉的寬度」——
// 回傳的長度就是斷出來的行數。
//
// 兩條契約，實作端必須遵守：
//   * 行數超過 widths 時，多出來的行一律用 widths.back()（最窄的那一個，保守）。
//   * 實際寬度可以**超過**允許寬度（斷不開的長 token），照實回報不要夾。
using EllipseTextMeasure = std::function<std::vector<double>(const std::vector<double>& widths)>;

struct EllipseTextTuning {
  // 橢圓的長寬比（寬 ÷ 高）。參考的漫畫氣泡量出來是 2.07，取 2。
  // 調小會讓橢圓更接近圓、行寬的長短差距變小（頭尾行不那麼空）。
  double aspect = 2.0;
  // 文字塊四周的內距。**氣泡的內距就是這兩個**，
  // windows/bubble_window.cpp 直接用這裡的預設值，不另外抄一組常數。
  //
  // paddingX 是從每一行的可用寬度左右各扣掉的，所以它精確地就是
  // 「那一行的字到橢圓弧線的水平距離」；paddingY 是文字塊上下緣到
  // 橢圓上下極點的淨空。因為橢圓是搜尋出來的最小值，**調大不會把字
  // 往內擠，是整顆氣泡跟著長大**。
  double paddingX = 20;
  double paddingY = 20;
  // 種子橢圓的假想填充率（見標頭 ③）。這個值要**高於任何實際排版做得到的**
  // 填充率，種子才會偏小。
  double seedFill = 0.85;
  // 每一輪放大多少、最多放大幾輪
  double growStep = 1.08;
  int maxGrow = 32;
  // 行數收斂的次數上限（正常兩三輪就到）
  int maxRelayout = 16;
  // 橢圓的下限與長半徑的上限。碰到上限之後只往高的方向長 —— 一長串的話
  // 該變成一顆高的橢圓，不是橫著長出螢幕。
  double minSemiMinor = 20;
  double maxSemiMajor = 320;
  // 允許寬度的下限。只是避免 0 或負數（那會讓 measure 無窮迴圈），
  // 真正的收斂靠外層放大。
  double minLineWidth = 8;
};

struct EllipseTextLayout {
  // 橢圓本體的外接矩形尺寸（不含外框與尾巴的餘裕）
  BubbleSize ellipse;
  // 每一行允許的寬度（已扣掉 paddingX）。**這一組就是要餵回 measure 的那一組。**
  std::vector<double> lineWidths;
  // 每一行的上緣，相對橢圓中心（負的在上）。文字塊對稱地騎在中心上。
  std::vector<double> lineTops;
  double lineHeight = 0;
  // 放大到上限仍然塞不下（超寬的不可斷 token、或行數怎麼樣都太多）。
  // 呼叫端該用不裁切的方式畫，讓它對稱地溢出去而不是被剁掉。
  bool overflow = false;
};

// 橢圓在 [yTop, yBottom] 這條水平帶裡的可用半寬（y 相對中心）。
// 取兩邊裡離中心較遠的那一個算，整條帶子才都在橢圓內（見標頭 ①）。
double ellipseHalfWidthAt(double semiMajor, double semiMinor, double yTop, double yBottom);

// singleLineWidth ＝ 完全不換行時的單行寬度（只拿來估種子大小），
// lineHeight ＝ 行距。measure 由呼叫端提供（見 EllipseTextMeasure）。
EllipseTextLayout fitTextInEllipse(double singleLineWidth, double lineHeight, const EllipseTextMeasure& measure, const EllipseTextTuning& tuning = {});

}  // namespace l2m
