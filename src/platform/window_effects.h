#pragma once

// 平台原生的視窗效果。
//
// 點擊穿透不用 Qt::WindowTransparentForInput：執行期切 Qt window flag
// 會重建原生視窗造成閃爍。改用平台原生 API 動態切換，兩邊手段不同：
//   Windows：SetWindowRgn 把視窗裁成角色形狀，區域外沒有視窗表面
//   macOS：沒有任意形狀視窗的 API，改成每幀拿游標位置去問遮罩，
//          再切 NSWindow.ignoresMouseEvents（全有全無，但效果等價）

#include <QRegion>
#include <QWindow>

namespace l2m::platform {

// 點擊穿透。Windows 是形狀視窗法：把視窗形狀裁成角色本體的區域，
// 區域外沒有視窗表面，點擊自然落到下層視窗。
//
// 不用 WS_EX_LAYERED|WS_EX_TRANSPARENT：layered 會把 GL 視窗的 DWM 合成
// 推回 redirection surface 路徑造成半透明區抖色；而 Chromium 的解法
// WS_EX_NOREDIRECTIONBITMAP 只能建窗時設定，且 Qt 的 wgl 呈現路徑
// 在無 redirection surface 下畫不出東西（皆已實測）。
//
// macOS 沒有等價的形狀視窗 API，改成用 region 對現在的游標位置做命中測試，
// 命中就收下滑鼠事件、沒命中就整窗 ignoresMouseEvents。也因此在 macOS 上
// 這個函式必須每幀呼叫（呼叫端本來就是每幀呼叫，見 character_window.cpp）。
//
// applyClickableRegion：套用角色形狀（座標為視窗內邏輯像素）
// clearClickableRegion：恢復整窗可點（穿透功能關閉時）
void applyClickableRegion(QWindow* window, const QRegion& region);
void clearClickableRegion(QWindow* window);

// 拔掉 Qt 自己掛上、但會壞掉畫面的原生視窗屬性。
//
// Windows：WS_EX_LAYERED。
// Qt 對透明（帶 alpha format）的 GL 視窗會自己掛上 layered style，
// 而 layered 會把 DWM 合成推回 redirection surface 路徑，
// 半透明區整片抖色棋盤紋（實測確認）。形狀視窗穿透法不需要 layered，
// 視窗建立後與整窗不透明度回到 1 時都要拔一次。
//
// macOS：沒有 layered 這種東西，改成關掉視窗陰影 —— 系統的陰影是依 alpha
// 形狀算好後快取的，角色每幀在動，陰影不會更新，會在角色周圍留一圈鬼影。
// 讓角色視窗在 app 非作用中時也留在畫面上。
// macOS：Qt::Tool → NSPanel 預設 hidesOnDeactivate=YES，會在 app 失去作用中狀態時
//        被收起來，而桌寵長期就是非作用中的；收起來之後渲染迴圈也跟著停住回不來。
// Windows：沒有對應行為，是空實作。
// 視窗每次顯示都要套一次（Qt 可能重建原生視窗）。
void keepVisibleWhenAppInactive(QWindow* window);
void stripLayeredStyle(QWindow* window);

// 置頂（always-on-top）的原生查詢與宣告。
//
// **刻意不走 QWindow::setFlag(Qt::WindowStaysOnTopHint, …)**：`QWindow::setFlags()`
// 開頭就是「flag 跟快取的一樣就直接 return」，所以當系統把 WS_EX_TOPMOST 拔掉之後
// （鎖定螢幕、顯示變更、其他 app 全螢幕…），Qt 仍然以為自己是置頂的，那條路完全
// 打不到原生視窗，桌寵就永遠沉在下面回不來。理由與實測數字寫在
// core/topmost_watchdog.h —— 這跟 stripLayeredStyle、applyClickableRegion 是同一類
// 「Qt 的抽象在這件事上不可靠，只能直接下原生 API」的情況。
//
// isTopmost 回報的是**原生視窗此刻的實況**，不是 Qt 的快取；原生視窗還沒建立時回 false。
// applyTopmost 無條件下達，呼叫端負責先問過 shouldReassertTopmost 再決定要不要叫。
bool isTopmost(QWindow* window);
void applyTopmost(QWindow* window, bool enabled);

// 「此刻有沒有一般（非置頂）視窗排在它上面」。
//
// 這是置頂掉了的**第二種症狀**：WS_EX_TOPMOST 還在，人卻已經被排到一般視窗底下，
// 只問 isTopmost 的看門狗完全看不見它（實測數字與重現方式見 core/topmost_watchdog.h）。
//
// Windows：沿著 z-order 往上走（GetWindow 的 GW_HWNDPREV），碰到第一個「沒帶置頂旗標、
// 而且真的看得到」的視窗就回 true。「真的看得到」那三個濾網缺一不可 —— 誤判的代價是
// 每 2 秒把自己插到 topmost 層最上面去跟別的置頂視窗搶位置：最小化的、被 DWM 標成
// cloaked 的（人在別的虛擬桌面上、或 UWP 被暫停），以及尺寸為零的隱形工具視窗。
// 找到就馬上收工，所以正常狀態下只走過我們上面那幾個置頂視窗，成本可以忽略。
//
// Linux／macOS：一律回 false。X11 要靠 XQueryTree 重建堆疊順序、還得處理 WM 的
// reparent frame 才問得出同一件事，沒有實機可驗就不猜（Xfwm4 那個「property 說置頂、
// 堆疊卻沒有」的個案已由 applyTopmost 的 REMOVE→ADD 硬打回去）；macOS 的
// NSWindow.level 就是系統真正的排序依據，沒有「level 還在卻被壓下去」這種狀態。
bool isCoveredByNormalWindow(QWindow* window);

}  // namespace l2m::platform
