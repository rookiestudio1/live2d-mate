#pragma once

// 「置頂被系統弄掉了，該不該重新宣告」的判定。
//
// 為什麼需要這個東西：Windows 會在某些時機把桌寵擠出置頂層
// （已知：工作階段鎖定／解鎖的 secure desktop 切換、顯示模式變更、
// 其他 app 進入全螢幕、程式沒回應被換成 ghost window，以及某些 app
// 光是被開起來就會 —— Windows 內建的小畫家就是）。這是系統行為，
// 攔不掉也預防不了 —— 桌寵能做的只有「發現掉了就宣告回去」。
//
// **症狀有兩種，只認第一種的話有一半的情況救不回來**：
//
//   ① WS_EX_TOPMOST 這個 bit 真的被拔掉。
//   ② **bit 還在，人卻已經被排到一般視窗底下。** 2026-09-04 實測：開一次
//      小畫家（class MSPaintApp），角色視窗的 exstyle 從頭到尾都是 0x88
//      （WS_EX_TOOLWINDOW | WS_EX_TOPMOST），z-order 上卻有兩個沒帶置頂
//      旗標的一般視窗排在它上面 —— 從此任何視窗都蓋得住桌寵。
//
// 原本這裡只問 ①，於是 ② 發生時看門狗每 2 秒醒來一次都判定「沒事」，
// 桌寵就一直沉在下面。② 的偵測在 platform::isCoveredByNormalWindow，
// 補救動作與 ① 完全一樣：實測一發 SetWindowPos(HWND_TOPMOST) 就拉回來了，
// 不必先 NOTOPMOST 再 TOPMOST 來回切。
//
// **而 Qt 的那條路兩種都救不了**：`WindowManager::setAlwaysOnTop()`
// 走的是 `QWindow::setFlag(Qt::WindowStaysOnTopHint, …)`，而 `QWindow::setFlags()`
// 第一行就是 `if (d->windowFlags == flags) return;` —— Qt 只信自己快取的那份 flag，
// 不會去看原生視窗現在到底長什麼樣。系統把 bit 拔掉之後 Qt 仍然認為「已經是置頂了」，
// 於是 setAlwaysOnTop(true) 變成一個什麼都不做的呼叫，桌寵就永遠沉在下面。
// 實測（2026-08-27，Qt 6.11.2）：外部 SetWindowPos(HWND_NOTOPMOST) 之後呼叫
// set_always_on_top(true)，EXSTYLE 從 0x88 掉到 0x80 之後**紋風不動**；
// 要 false→true 來回切一次（讓 Qt 的快取真的改變）才會回來。
// 所以修法一定要繞過 Qt 直接下 SetWindowPos，見 platform/window_effects.h 的 applyTopmost。
//
// 症狀 ① 時「點一下角色」是沒有用的：那只是把它拉到**一般視窗層**的最上面，
// 並沒有回到 topmost 層，所以一去用別的 app 就又被蓋住。症狀 ② 時點一下確實會回來
// （前景化那一下會把視窗重新插回它自己的置頂層），但兩種都不該要使用者自己動手。
//
// 這裡只放判定，不碰任何平台 API —— 四個輸入的組合要能不開視窗就驗到。

namespace l2m {

// 多久檢查一次原生的置頂實況。
//
// 2 秒是刻意的折衷：解鎖回來最多沉 2 秒（那段時間畫面本來就在淡入，看不出來），
// 而檢查本身只是一次 GetWindowLongPtr 加一小段 z-order 走訪，比每幀都在跑的
// 命中遮罩便宜好幾個數量級。再短沒有意義，再長使用者就會注意到。
inline constexpr int kTopmostCheckIntervalMs = 2000;

// 這一輪要不要重新宣告置頂。
//
// nativeTopmost 是**原生視窗現在的實況**（不是 Qt 以為的），coveredByNormalWindow
// 是「此刻有沒有一般視窗排在它上面」—— 兩個各對應上面那兩種症狀，任一個成立就得補一刀。
// 這正是整件事的重點：光問旗標會整組漏掉症狀 ②。
//
// 兩個都沒事就不動 —— 每次都無條件 SetWindowPos(HWND_TOPMOST) 會把自己
// 一直插到 topmost 層的最上面，去跟工作管理員、輸入法候選窗那些同樣置頂的視窗搶位置。
//
// windowVisible 為 false 時不動：角色被隱藏（或正在淡出）時去改 z-order 沒有意義，
// 而且 setVisible(true) 那條路自己會補一次。
bool shouldReassertTopmost(bool wantTopmost, bool nativeTopmost, bool windowVisible, bool coveredByNormalWindow);

}  // namespace l2m
