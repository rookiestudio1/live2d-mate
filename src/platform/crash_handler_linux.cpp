#include "crash_handler.h"

#include <cstdlib>

namespace l2m {
namespace platform {

// Linux 版的 %APPDATA% 等價物：XDG 的 data home。
//
// 必須與 QStandardPaths::AppDataLocation 在 Linux 上算出來的路徑一致 ——
// 這支存在的唯一理由就是「在 QApplication 還沒建立、QStandardPaths 用不了的
// 時機算出同一個目錄」（見 crash_handler.h）。兩者分岔的話，crash 檔會寫到
// 一個地方、下次啟動的掃描與「開啟日誌資料夾」按鈕卻去看另一個地方，
// 而且不會有任何錯誤訊息。
//
// XDG 基礎目錄規格：$XDG_DATA_HOME 有設就用它，否則退回 ~/.local/share。
std::filesystem::path appDataDirFromEnv() {
  if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
    return std::filesystem::path(xdg) / "live2d_mate";
  }
  const char* home = std::getenv("HOME");
  if (!home || !*home) return {};
  return std::filesystem::path(home) / ".local" / "share" / "live2d_mate";
}

// 以下三支是空殼（同 crash_handler_mac.mm）。
//
// **Linux 的 crash handler 還沒實裝。** 現有的實作整份是 Windows 專有的：
// 四個進場點裡有三個是 MSVC CRT 的（set_terminate 的 per-thread 語意、
// _set_purecall_handler、_set_invalid_parameter_handler），堆疊擷取靠
// CaptureStackBackTrace 與 x64 的 .pdata unwind table，符號化靠 DbgHelp 與
// PDB。Linux 上對應的東西是 sigaction + backtrace()/libunwind + DWARF，
// 沒有一項是共用的 —— 那是一次獨立的移植，不是補幾個 #ifdef。
//
// 空殼的作用是讓 main.cpp 與 mcp_http_server.cpp 只有一份程式碼路徑：
// 它們無條件呼叫這四支，不必為平台加分支。**沒有這個檔案，Linux 建置會
// 直接在連結期倒在四個未定義符號上**（crash handler 這個功能是在 Linux
// 移植之後才進 master 的，兩邊各自都編得過，合起來才會壞）。
void installCrashHandler(const std::filesystem::path&, const char*) {}

void installThreadCrashSupport() {}

void triggerCrashTestFromEnv() {}

}  // namespace platform
}  // namespace l2m
