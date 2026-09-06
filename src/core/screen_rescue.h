#pragma once

// 螢幕熱插拔救援：視窗還看得見嗎；看不見時最近的可見位置。
//
// 症狀：桌寵放在外接螢幕上，拔掉螢幕（或改解析度）之後視窗留在已經不存在的
// 座標空間裡 —— 程式明明在跑，畫面上卻找不到角色，使用者只能砍掉重開。
//
// 純幾何，不碰 Qt 的 QScreen（那些由 AppController 轉成矩形餵進來），
// 「怎樣算看得見」「救去哪裡」的規則才測得到。

#include <optional>
#include <vector>

namespace l2m {

struct ScreenRect {
  int x = 0;
  int y = 0;
  int w = 0;
  int h = 0;
};

// 視窗至少要有這麼大的一塊（正方形邊長 px）落在某個工作區內才算搆得到 ——
// 只露出 2px 的邊理論上「可見」，實務上根本點不到
inline constexpr int kMinReachablePx = 48;

// 視窗與任一工作區的交集寬與高都達到 minVisiblePx 才算看得見
bool windowReachable(const ScreenRect& window, const std::vector<ScreenRect>& screens, int minVisiblePx = kMinReachablePx);

// 看不見時：挑中心距離最近的工作區，把視窗 clamp 進去，回新的左上角。
// 本來就看得見（或一個螢幕都沒有）回 nullopt ＝ 不必動。
std::optional<std::pair<int, int>> rescuePosition(const ScreenRect& window, const std::vector<ScreenRect>& screens, int minVisiblePx = kMinReachablePx);

}  // namespace l2m
