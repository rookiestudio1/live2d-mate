// macOS 的視窗效果實作，對應 window_effects_win.cpp。
//
// 兩邊的手段完全不同：Windows 是「把視窗裁成角色形狀」（SetWindowRgn），
// macOS 沒有等價的任意形狀視窗 API —— NSWindow 只有全有全無的
// ignoresMouseEvents。所以這邊改成「每幀拿游標位置去問遮罩」：
// 游標落在角色本體上就收下滑鼠事件，落在透明處就整窗放行。
// 效果等價（區域外的點擊會落到下層視窗），代價是必須每幀更新一次。

#import <AppKit/AppKit.h>

#include "window_effects.h"

#include <QDebug>

#include <cmath>

namespace l2m::platform {

namespace {

// Qt 在 macOS 的 winId() 回傳 NSView*（Windows 那邊是 HWND）
NSView* nativeView(QWindow* window) {
  if (!window) return nil;
  return reinterpret_cast<NSView*>(window->winId());
}

// 游標位置 → 視窗內、左上為原點的邏輯像素，也就是 QRegion 用的座標系。
// macOS 螢幕座標是左下為原點，y 要翻過來。
// 用 view 的實際 rect 而不是 window.frame：目前是無邊框視窗兩者相同，
// 但哪天長出標題列，用 frame 會整個差一個標題列的高度。
QPoint cursorInView(NSView* view) {
  const NSPoint cursor = [NSEvent mouseLocation];
  const NSRect rect =
    [[view window] convertRectToScreen:[view convertRect:view.bounds toView:nil]];
  return QPoint(static_cast<int>(std::lround(cursor.x - NSMinX(rect))),
                static_cast<int>(std::lround(NSMaxY(rect) - cursor.y)));
}

}  // namespace

void applyClickableRegion(QWindow* window, const QRegion& region) {
  NSView* view = nativeView(window);
  NSWindow* native = view ? [view window] : nil;
  if (!native) return;

  // 按著鍵的時候一律不動：拖曳中把 ignoresMouseEvents 切成 YES，
  // AppKit 會中斷那一輪的滑鼠追蹤，mouseUp 收不到、角色就黏在游標上。
  // 快速拖曳時游標本來就常常暫時跑出遮罩（遮罩慢一幀），這條是必要的。
  if ([NSEvent pressedMouseButtons] != 0) return;

  const BOOL ignore = region.contains(cursorInView(view)) ? NO : YES;
  // 每幀都被呼叫，值沒變就別碰 —— setter 會讓 AppKit 重算事件路由
  if (native.ignoresMouseEvents != ignore) native.ignoresMouseEvents = ignore;
}

void clearClickableRegion(QWindow* window) {
  NSView* view = nativeView(window);
  NSWindow* native = view ? [view window] : nil;
  if (!native) return;
  if (native.ignoresMouseEvents) native.ignoresMouseEvents = NO;
}

void keepVisibleWhenAppInactive(QWindow* window) {
  // Qt::Tool 在 macOS 會被對映成 NSPanel，而 Qt 給它的預設是
  // hidesOnDeactivate = YES —— app 一失去作用中狀態，AppKit 就把這個視窗收起來。
  // 對一般 app 的浮動工具列這是對的，對桌寵是災難：桌寵沒有主視窗、
  // 所有操作都從選單列圖示走，本來就長期處於非作用中，收起來就再也回不來。
  //
  // 實際症狀是「換模型之後角色不見了，重開才會出現」：換模型會生一個膠囊提示
  // 子行程，那個子行程一啟動就搶走作用中狀態，主行程隨即被停用 → 面板被收起來
  //（實測 occlusionState 變成不可見、QWindow::isExposed() 變成 false）→
  // 而 CharacterWindow 的渲染迴圈是靠 frameSwapped → update() 自己餵自己的，
  // 一旦沒有畫面被送出，就永遠不會再有下一次 update，整個迴圈就此停住。
  // 膠囊子行程結束之後也不會恢復，因為選單列 app 拿不回作用中狀態。
  //
  // Windows 沒有這個行為，所以同一份程式在那邊一直是好的。
  // QWidget 有 Qt::WA_MacAlwaysShowToolWindow 可以關掉，但角色視窗是 QWindow
  //（QOpenGLWindow），只能直接對 NSPanel 設。
  NSView* view = nativeView(window);
  NSWindow* native = view ? [view window] : nil;
  if (![native isKindOfClass:[NSPanel class]]) return;
  ((NSPanel*)native).hidesOnDeactivate = NO;
}

void stripLayeredStyle(QWindow* window) {
  // WS_EX_LAYERED 是 Windows 專有的，macOS 沒有對應的東西要拔。
  // 但這個函式被呼叫的時機（視窗顯示、不透明度回到 1、置頂切換 ——
  // 也就是 Qt 可能重建原生視窗的時候）剛好是處理 macOS 這邊對應毛病的位置：
  // 無邊框半透明視窗的陰影是系統依 alpha 形狀算好後「快取」起來的，
  // 角色每幀都在動，陰影不會跟著更新，就變成角色周圍掛著一圈舊輪廓的鬼影。
  // 桌寵本來也不該有視窗陰影，直接關掉。
  NSView* view = nativeView(window);
  NSWindow* native = view ? [view window] : nil;
  if (!native) return;
  if (native.hasShadow) native.hasShadow = NO;
}

bool isTopmost(QWindow* window) {
  // 這裡沒有 Windows 那個「系統會偷偷把 WS_EX_TOPMOST 拔掉」的問題，
  // 但介面要對稱，而且 NSWindow.level 同樣是「原生的實況」而非 Qt 的快取。
  if (!window || !window->handle()) return false;
  NSView* view = nativeView(window);
  NSWindow* native = view ? [view window] : nil;
  if (!native) return false;
  return native.level >= NSFloatingWindowLevel;
}

void applyTopmost(QWindow* window, bool enabled) {
  if (!window || !window->handle()) return;
  NSView* view = nativeView(window);
  NSWindow* native = view ? [view window] : nil;
  if (!native) return;
  const NSWindowLevel level = enabled ? NSFloatingWindowLevel : NSNormalWindowLevel;
  if (native.level != level) native.level = level;
}

bool isCoveredByNormalWindow(QWindow*) {
  // macOS 沒有這種狀態：NSWindow.level 就是系統真正的排序依據，
  // 不像 Windows 的 WS_EX_TOPMOST 會出現「旗標還在、人已經被壓下去」的分歧。
  return false;
}

}  // namespace l2m::platform

