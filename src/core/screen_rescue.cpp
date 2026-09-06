#include "screen_rescue.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace l2m {

namespace {

// 交集矩形的寬高（無交集回 0）
std::pair<int, int> intersectionSize(const ScreenRect& a, const ScreenRect& b) {
  const int left = std::max(a.x, b.x);
  const int top = std::max(a.y, b.y);
  const int right = std::min(a.x + a.w, b.x + b.w);
  const int bottom = std::min(a.y + a.h, b.y + b.h);
  return {std::max(0, right - left), std::max(0, bottom - top)};
}

double centerDistance(const ScreenRect& a, const ScreenRect& b) {
  const double ax = a.x + a.w / 2.0;
  const double ay = a.y + a.h / 2.0;
  const double bx = b.x + b.w / 2.0;
  const double by = b.y + b.h / 2.0;
  return std::hypot(ax - bx, ay - by);
}

}  // namespace

bool windowReachable(const ScreenRect& window, const std::vector<ScreenRect>& screens, int minVisiblePx) {
  for (const auto& screen : screens) {
    const auto [w, h] = intersectionSize(window, screen);
    // 視窗比門檻還小的極端情況：整個進到工作區就算搆得到
    const int needW = std::min(minVisiblePx, window.w);
    const int needH = std::min(minVisiblePx, window.h);
    if (w >= needW && h >= needH) return true;
  }
  return false;
}

std::optional<std::pair<int, int>> rescuePosition(const ScreenRect& window, const std::vector<ScreenRect>& screens, int minVisiblePx) {
  if (screens.empty()) return std::nullopt;  // 一個螢幕都沒有：救無可救，別亂動
  if (windowReachable(window, screens, minVisiblePx)) return std::nullopt;

  // 挑中心距離最近的工作區 —— 拔掉右邊的螢幕，角色該回到右邊那顆的鄰居
  const ScreenRect* nearest = &screens.front();
  double best = std::numeric_limits<double>::max();
  for (const auto& screen : screens) {
    const double distance = centerDistance(window, screen);
    if (distance < best) {
      best = distance;
      nearest = &screen;
    }
  }

  // clamp 進工作區；視窗比工作區大時貼齊左上（至少頭看得到）
  const int maxX = nearest->x + std::max(0, nearest->w - window.w);
  const int maxY = nearest->y + std::max(0, nearest->h - window.h);
  const int newX = std::clamp(window.x, nearest->x, maxX);
  const int newY = std::clamp(window.y, nearest->y, maxY);
  return std::make_pair(newX, newY);
}

}  // namespace l2m
