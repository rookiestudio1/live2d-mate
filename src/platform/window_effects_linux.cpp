// Linux（X11）的視窗效果實作，對應 window_effects_win.cpp / window_effects_mac.mm。
//
// X11 的手段其實比 Windows 好：XShape 1.1 的 **input region** 可以只裁點擊、
// 不裁畫面（Windows 的 SetWindowRgn 是連畫面一起裁，所以那邊的遮罩要外擴
// 補鋸齒），畫面完全交給合成器的 alpha。置頂走 EWMH：applyTopmost 發
// _NET_WM_STATE client message 給 root 讓 WM 處理，isTopmost 讀 WM 實際掛在
// 視窗上的 _NET_WM_STATE property —— 與 Windows 版同樣是「原生實況而非 Qt
// 快取」（topmost_watchdog 的存在理由，見 window_effects.h）。
//
// Wayland session 下拿不到 X11 Display，所有函式安靜退化成 no-op：
// 穿透與置頂失效，但程式照常跑。Wayland 原生支援是另一個工程
// （合成器不給 client 讀全域游標、也沒有 always-on-top 概念）。

#include "window_effects.h"

#include <QGuiApplication>

#include <cmath>
#include <cstddef>
#include <vector>

// X11 標頭定義了一堆與 Qt 相撞的巨集（None、Bool、Status…），一律排在 Qt 之後
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/shape.h>

namespace l2m::platform {

namespace {

// Wayland session（或 QGuiApplication 還沒建）會回 nullptr，呼叫端一律要接受
Display* x11Display() {
  if (!qGuiApp) return nullptr;
  auto* x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
  return x11 ? x11->display() : nullptr;
}

}  // namespace

void applyClickableRegion(QWindow* window, const QRegion& region) {
  if (!window || !window->handle()) return;
  Display* dpy = x11Display();
  if (!dpy) return;

  // QRegion 是邏輯像素、XShape 是裝置像素：依 devicePixelRatio 縮放並外擴到
  // 整數邊界。QRegion 的矩形保證依 Y 再 X 排序且互不重疊，正好是 YXBanded。
  const qreal dpr = window->devicePixelRatio();
  std::vector<XRectangle> rects;
  rects.reserve(static_cast<size_t>(region.rectCount()));
  for (const QRect& r : region) {
    const int x = static_cast<int>(std::floor(r.left() * dpr));
    const int y = static_cast<int>(std::floor(r.top() * dpr));
    const int x2 = static_cast<int>(std::ceil((r.right() + 1) * dpr));
    const int y2 = static_cast<int>(std::ceil((r.bottom() + 1) * dpr));
    rects.push_back(XRectangle{static_cast<short>(x), static_cast<short>(y), static_cast<unsigned short>(x2 - x), static_cast<unsigned short>(y2 - y)});
  }

  // 空清單＝空 input region＝整窗穿透，語意與 Windows 版的空 region 一致。
  // 只設 client 視窗即可：Xfwm4（與多數 EWMH WM）會把 client 的 input shape
  // 同步到自己的 frame 視窗上（實測 client 222 rects → frame 222 rects），
  // 不必自己去找 frame 動手。
  XShapeCombineRectangles(dpy, static_cast<Window>(window->winId()), ShapeInput, 0, 0, rects.data(), static_cast<int>(rects.size()), ShapeSet, YXBanded);
  XFlush(dpy);
}

void clearClickableRegion(QWindow* window) {
  if (!window || !window->handle()) return;
  Display* dpy = x11Display();
  if (!dpy) return;
  // mask 給 None 就是「恢復預設＝整窗可點」
  XShapeCombineMask(dpy, static_cast<Window>(window->winId()), ShapeInput, 0, 0, None, ShapeSet);
  XFlush(dpy);
}

void keepVisibleWhenAppInactive(QWindow*) {
  // X11 沒有 macOS 那種「app 失去作用中就把 NSPanel 收起來」的行為，不用做事
}

void stripLayeredStyle(QWindow*) {
  // WS_EX_LAYERED 是 Windows 專有的；X11 的透明靠合成器合成，沒東西要拔。
  // macOS 在這裡順手關視窗陰影，X11 的無邊框視窗預設就沒有陰影，也不用。
}

bool isTopmost(QWindow* window) {
  if (!window || !window->handle()) return false;
  Display* dpy = x11Display();
  if (!dpy) return false;  // Wayland：查不到就回 false，applyTopmost 也是 no-op，無害

  const Atom stateAtom = XInternAtom(dpy, "_NET_WM_STATE", True);
  const Atom aboveAtom = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", True);
  if (stateAtom == None || aboveAtom == None) return false;

  Atom actualType = None;
  int actualFormat = 0;
  unsigned long count = 0;
  unsigned long bytesAfter = 0;
  unsigned char* data = nullptr;
  const int status = XGetWindowProperty(dpy, static_cast<Window>(window->winId()), stateAtom, 0, 1024, False, XA_ATOM, &actualType, &actualFormat, &count, &bytesAfter, &data);
  if (status != Success || !data) return false;

  bool found = false;
  if (actualType == XA_ATOM && actualFormat == 32) {
    const Atom* atoms = reinterpret_cast<const Atom*>(data);
    for (unsigned long i = 0; i < count; ++i) {
      if (atoms[i] == aboveAtom) {
        found = true;
        break;
      }
    }
  }
  XFree(data);
  return found;
}

void applyTopmost(QWindow* window, bool enabled) {
  if (!window || !window->handle()) return;
  Display* dpy = x11Display();
  if (!dpy) return;

  // 已 map 的視窗改 _NET_WM_STATE 必須發 client message 給 root 讓 WM 處理，
  // 直接改 property WM 不會理 —— 與 Windows「不能直接改 WS_EX_TOPMOST bit、
  // 要走 SetWindowPos」是同一類規矩。
  //
  // Xfwm4 陷阱：Qt 在 map 前就把 _NET_WM_STATE_ABOVE 寫進初始屬性，Xfwm4 會把
  // 狀態「記」在 property 上，實際堆疊卻沒有放進置頂層（實測：一般視窗照樣壓在
  // 上面，而 isTopmost 讀 property 是 true，看門狗因此永遠不會來救）。對這種
  // 「已記錄」的狀態再送 ADD 是 no-op，唯一可靠的辦法是先 REMOVE 再 SET 強迫
  // 重算層級。呼叫端只在顯示／切換／看門狗補救時呼叫，多送一發無妨。
  const Atom stateAtom = XInternAtom(dpy, "_NET_WM_STATE", False);
  const Atom aboveAtom = XInternAtom(dpy, "_NET_WM_STATE_ABOVE", False);
  const long actions[2] = {0, 1};  // 0 = _NET_WM_STATE_REMOVE, 1 = _NET_WM_STATE_ADD
  const int sends = enabled ? 2 : 1;
  for (int i = 0; i < sends; ++i) {
    XEvent ev = {};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = static_cast<Window>(window->winId());
    ev.xclient.message_type = stateAtom;
    ev.xclient.format = 32;
    ev.xclient.data.l[0] = actions[i];
    ev.xclient.data.l[1] = static_cast<long>(aboveAtom);
    ev.xclient.data.l[2] = 0;
    ev.xclient.data.l[3] = 1;  // source indication：一般應用程式
    XSendEvent(dpy, DefaultRootWindow(dpy), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
  }
  XFlush(dpy);
}

bool isCoveredByNormalWindow(QWindow*) {
  // X11 沒做：要問這件事得 XQueryTree 整棵 root 的子視窗重建堆疊順序，
  // 還得穿過 WM 的 reparent frame 才找得到自己那一格，沒有實機可驗就不猜。
  // 理由與 Windows 版的做法寫在 window_effects.h。
  return false;
}

}  // namespace l2m::platform
