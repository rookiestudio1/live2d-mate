#pragma once

// 開機自啟的純字串邏輯（註冊表要寫哪一行、這次啟動要不要隱藏）。
//
// 放在 l2m_core 而不是 platform/：platform 的實作編進執行檔，
// 只連 l2m_core 的 QTest 連不到它。真正碰註冊表的部分留在
// platform/autostart_win.cpp。
//
// autostartCommandLine 只有 Windows 用得到（macOS 的旗標寫在 launchd agent 的
// plist 裡，見 platform/autostart_mac.mm），但 shouldStartHidden 兩邊共用。

#include <string>
#include <vector>

namespace l2m::platform {

// 自啟時帶的旗標：開機那次不要跳出視窗，角色安靜地待在系統匣
inline constexpr const char* kHiddenFlag = "--hidden";

// 註冊表要寫的那一行。路徑一律加引號，含空白的安裝路徑才不會被切斷。
std::string autostartCommandLine(const std::string& exePath);

// 這次啟動要不要隱藏。兩個平台都是純看 argv：macOS 註冊的是自己的 launchd agent，
// 旗標直接寫在 plist 的 ProgramArguments，不必回頭問系統「是不是以登入項目啟動的」。
bool shouldStartHidden(const std::vector<std::string>& args);

}  // namespace l2m::platform
