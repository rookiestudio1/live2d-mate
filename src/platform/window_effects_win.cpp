#include "window_effects.h"

#include <windows.h>

#include <dwmapi.h>

#include <cstdint>
#include <vector>

namespace l2m::platform {

void applyClickableRegion(QWindow* window, const QRegion& region) {
  if (!window) return;
  const HWND hwnd = reinterpret_cast<HWND>(window->winId());

  // QRegion → HRGN。
  // 逐個 CombineRgn(hrgn, hrgn, part, RGN_OR) 累積是 O(n²)：每加一個矩形
  // GDI 都要重算整個 region，遮罩產生的幾百個矩形實測要吃掉 ~2ms。
  // 改成把所有矩形一次塞進 RGNDATA 交給 ExtCreateRegion，GDI 內部一次排序合併。
  const int rectCount = region.rectCount();
  if (rectCount <= 0) {
    // 空區域＝整窗不可點；交給 SetWindowRgn 一個空 region 即可
    HRGN empty = CreateRectRgn(0, 0, 0, 0);
    if (!SetWindowRgn(hwnd, empty, FALSE)) DeleteObject(empty);
    return;
  }

  std::vector<uint8_t> buffer(sizeof(RGNDATAHEADER) + sizeof(RECT) * size_t(rectCount));
  auto* data = reinterpret_cast<RGNDATA*>(buffer.data());
  auto* rects = reinterpret_cast<RECT*>(data->Buffer);

  int index = 0;
  for (const QRect& rect : region) {
    rects[index++] = RECT{rect.left(), rect.top(), rect.right() + 1, rect.bottom() + 1};
  }

  const QRect bounds = region.boundingRect();
  data->rdh.dwSize = sizeof(RGNDATAHEADER);
  data->rdh.iType = RDH_RECTANGLES;
  data->rdh.nCount = static_cast<DWORD>(index);
  data->rdh.nRgnSize = static_cast<DWORD>(sizeof(RECT) * size_t(index));
  data->rdh.rcBound = RECT{bounds.left(), bounds.top(), bounds.right() + 1, bounds.bottom() + 1};

  HRGN hrgn = ExtCreateRegion(nullptr, static_cast<DWORD>(buffer.size()), data);
  if (!hrgn) return;  // 建不出來就維持原形狀，別把視窗弄成全不可點

  // bRedraw=FALSE：我們每幀都在重繪，不需要系統再觸發一次
  // SetWindowRgn 成功後 hrgn 歸系統所有，不能 DeleteObject
  if (!SetWindowRgn(hwnd, hrgn, FALSE)) DeleteObject(hrgn);
}

void clearClickableRegion(QWindow* window) {
  if (!window) return;
  const HWND hwnd = reinterpret_cast<HWND>(window->winId());
  SetWindowRgn(hwnd, nullptr, FALSE);
}

void keepVisibleWhenAppInactive(QWindow*) {
  // Windows 沒有「app 失去焦點就把工具視窗收起來」這種行為，不用做事
}

void stripLayeredStyle(QWindow* window) {
  if (!window) return;
  const HWND hwnd = reinterpret_cast<HWND>(window->winId());
  const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
  if (exStyle & WS_EX_LAYERED) SetWindowLongPtrW(hwnd, GWL_EXSTYLE, exStyle & ~WS_EX_LAYERED);
}

bool isTopmost(QWindow* window) {
  // 不用 winId()：那會在原生視窗還沒建立時把它逼出來。這個函式是計時器每 2 秒
  // 叫一次的，沒建好就代表還輪不到它管，回 false 讓呼叫端跳過。
  if (!window || !window->handle()) return false;
  const HWND hwnd = reinterpret_cast<HWND>(window->winId());
  return (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0;
}

void applyTopmost(QWindow* window, bool enabled) {
  if (!window || !window->handle()) return;
  const HWND hwnd = reinterpret_cast<HWND>(window->winId());
  // 一定要走 SetWindowPos。用 SetWindowLongPtr 直接改 WS_EX_TOPMOST 這個 bit
  // 是 MSDN 明文禁止的：style 會變但 z-order 不會跟著搬，視窗從此處於
  // 「自稱置頂但排在一般層裡」的錯亂狀態。
  //
  // SWP_NOACTIVATE：桌寵不該因為重新宣告置頂就把使用者正在打字的視窗搶走焦點。
  SetWindowPos(hwnd, enabled ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

bool isCoveredByNormalWindow(QWindow* window) {
  // 與 isTopmost 同理：原生視窗還沒建立就代表還輪不到它管，不要用 winId() 把它逼出來
  if (!window || !window->handle()) return false;
  const HWND self = reinterpret_cast<HWND>(window->winId());

  // GW_HWNDPREV 是「z-order 上排在前面（也就是更上層）的那一個」，一路往上走到頂
  for (HWND above = GetWindow(self, GW_HWNDPREV); above; above = GetWindow(above, GW_HWNDPREV)) {
    // 置頂視窗排在我們上面本來就是對的：工作管理員、輸入法候選窗、我們自己的氣泡
    if (GetWindowLongPtrW(above, GWL_EXSTYLE) & WS_EX_TOPMOST) continue;

    // 以下三個濾網缺一不可，理由見 window_effects.h：這裡誤判一次，
    // 看門狗就會每 2 秒插隊一次
    if (!IsWindowVisible(above) || IsIconic(above)) continue;
    RECT rect{};
    if (!GetWindowRect(above, &rect) || rect.right <= rect.left || rect.bottom <= rect.top) continue;
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(above, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) continue;

    return true;  // 找到一個就夠了，不必走完整條 z-order
  }
  return false;
}

}  // namespace l2m::platform
