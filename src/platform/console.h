#pragma once

// 讓 GUI 子系統的行程把 stderr 接回啟動它的那個終端機。
//
// live2d_mate 是 qt_add_executable(... WIN32 ...) 的 GUI 子系統程式 ——
// 它**沒有自己的 console**。從 cmd/pwsh 啟動時 stderr 看不看得到，
// 取決於父行程有沒有把 handle 傳下來，不可靠；要靠 AttachConsole 主動接上去。
//
// **但這裡有一個會把事情弄反的陷阱**：Qt Creator 是用 pipe 接管子行程的 stderr 的，
// 那種情況下 handle 本來就有效，再去 freopen 到 CONOUT$ 會把 pipe 蓋掉，
// Application Output 窗格反而變成空的。所以判定順序是
// 「stderr handle 已經有效就完全不要碰，無效才 AttachConsole」。
//
// 呼叫時機：**必須在建立任何 spdlog sink 之前**。spdlog 的 wincolor_stderr_sink
// 在建構時就把 GetStdHandle(STD_ERROR_HANDLE) 抓起來存著，之後再換就來不及了。
//
// 雙擊啟動（沒有父 console）時這支什麼都不做，也不會另外開一個黑視窗 ——
// 桌寵旁邊多一個 console 視窗是不能接受的。

namespace l2m {
namespace platform {

void attachParentConsole();

}  // namespace platform
}  // namespace l2m
