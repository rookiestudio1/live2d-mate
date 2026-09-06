#include "viewer_settings.h"

#include <algorithm>

#include "json_doc.h"

namespace l2m {

namespace {

// 「看得到」的門檻：抓得到標題列那一條就算。整片都要在螢幕上是過度的 ——
// 使用者刻意把視窗擺在螢幕邊緣、或跨在兩面螢幕中間，都是合理的擺法。
constexpr int kMinVisibleWidth = 160;
constexpr int kMinVisibleHeight = 32;

// 數值欄位。型別不對、NaN、離譜的量級一律回 false（整份設定跟著作廢）。
// 範圍比較刻意寫成 !(lo <= v && v <= hi)：NaN 的所有比較都是 false，
// 這樣寫才會被擋下來而不是漏過去。
bool readInt(yyjson_val* obj, const char* key, int limit, int& out) {
  yyjson_val* value = jsonu::get(obj, key);
  if (!value || !yyjson_is_num(value)) return false;
  const double num = yyjson_get_num(value);
  if (!(num >= -static_cast<double>(limit) && num <= static_cast<double>(limit))) return false;
  out = static_cast<int>(num);
  return true;
}

// 視窗與螢幕的交集尺寸（沒交集就是 0×0）
void overlapSize(const WindowGeometry& window, const ScreenRect& screen, int& width, int& height) {
  const int left = std::max(window.x, screen.x);
  const int top = std::max(window.y, screen.y);
  const int right = std::min(window.x + window.width, screen.x + screen.width);
  const int bottom = std::min(window.y + window.height, screen.y + screen.height);
  width = std::max(0, right - left);
  height = std::max(0, bottom - top);
}

long long overlapArea(const WindowGeometry& window, const ScreenRect& screen) {
  int width = 0;
  int height = 0;
  overlapSize(window, screen, width, height);
  return static_cast<long long>(width) * static_cast<long long>(height);
}

// **面積不能拿來判斷「看不看得見」**：只露出右邊 10 px 的一條縫，乘上 600 px 的
// 視窗高度就有 6000 —— 面積門檻輕鬆跨過，畫面上卻只有一條抓不住的細線。
// 要的是「橫的與直的**各自**都夠一塊標題列」，所以兩個維度分開比。
bool visibleEnough(const WindowGeometry& window, const ScreenRect& screen) {
  int width = 0;
  int height = 0;
  overlapSize(window, screen, width, height);
  return width >= kMinVisibleWidth && height >= kMinVisibleHeight;
}

}  // namespace

std::optional<WindowGeometry> parseViewerWindow(const std::string& json) {
  const std::optional<jsonu::Doc> doc = jsonu::Doc::parse(json);
  if (!doc) return std::nullopt;

  yyjson_val* window = jsonu::get(doc->root(), "window");
  if (!window || !yyjson_is_obj(window)) return std::nullopt;

  WindowGeometry geometry;
  if (!readInt(window, "x", kMaxWindowCoord, geometry.x)) return std::nullopt;
  if (!readInt(window, "y", kMaxWindowCoord, geometry.y)) return std::nullopt;
  if (!readInt(window, "width", kMaxWindowSize, geometry.width)) return std::nullopt;
  if (!readInt(window, "height", kMaxWindowSize, geometry.height)) return std::nullopt;
  if (geometry.width < kMinWindowWidth || geometry.height < kMinWindowHeight) return std::nullopt;

  // maximized 缺了就是 false —— 手寫這個檔的人只會寫位置與大小，
  // 少一個布林不該讓整份設定作廢。
  yyjson_val* maximized = jsonu::get(window, "maximized");
  geometry.maximized = maximized && yyjson_is_bool(maximized) && yyjson_get_bool(maximized);
  return geometry;
}

std::string serializeViewerWindow(const WindowGeometry& window) {
  jsonu::MutDoc doc;
  yyjson_mut_val* root = yyjson_mut_obj(doc.get());
  doc.setRoot(root);

  yyjson_mut_val* section = yyjson_mut_obj(doc.get());
  yyjson_mut_obj_add_val(doc.get(), root, "window", section);
  yyjson_mut_obj_add_int(doc.get(), section, "x", window.x);
  yyjson_mut_obj_add_int(doc.get(), section, "y", window.y);
  yyjson_mut_obj_add_int(doc.get(), section, "width", window.width);
  yyjson_mut_obj_add_int(doc.get(), section, "height", window.height);
  yyjson_mut_obj_add_bool(doc.get(), section, "maximized", window.maximized);
  return doc.write(true);
}

WindowGeometry fitToScreens(const WindowGeometry& saved, const std::vector<ScreenRect>& screens) {
  if (screens.empty()) return saved;

  // 交集最大的那面螢幕就是「這個視窗原本待的地方」。一面都碰不到（副螢幕拔掉、
  // 桌面重新排列）時退回第一面 —— 呼叫端把主螢幕排在第一個。
  size_t best = 0;
  long long bestArea = 0;
  for (size_t i = 0; i < screens.size(); ++i) {
    const long long area = overlapArea(saved, screens[i]);
    if (area > bestArea) {
      bestArea = area;
      best = i;
    }
  }
  const ScreenRect& screen = screens[best];

  WindowGeometry result = saved;
  // 先夾尺寸：在 4K 上開得好好的視窗，換到 1080p 就會比整面螢幕還大，
  // 右下角那半截（含清單那一欄）永遠拉不回來。
  result.width = std::min(result.width, screen.width);
  result.height = std::min(result.height, screen.height);

  // 看得到就不要自作聰明地搬動它
  if (visibleEnough(result, screen)) return result;

  // 幾乎看不見：擺到那面螢幕正中央。貼邊會讓人以為視窗根本沒開起來。
  result.x = screen.x + (screen.width - result.width) / 2;
  result.y = screen.y + (screen.height - result.height) / 2;
  return result;
}

}  // namespace l2m
