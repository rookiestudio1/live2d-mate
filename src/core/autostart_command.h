#pragma once

// 開機自啟的純字串邏輯（自啟項要寫哪一行、這次啟動要不要隱藏）。
//
// 放在 l2m_core 而不是 platform/：platform 的實作編進執行檔，
// 只連 l2m_core 的 QTest 連不到它。真正碰註冊表／.desktop 的部分留在
// platform/autostart_win.cpp 與 platform/autostart_linux.cpp。
//
// autostartCommandLine 是 Windows 與 Linux 共用的（macOS 的參數寫在 launchd
// agent 的 plist 裡，見 platform/autostart_mac.mm），shouldStartHidden 三邊共用。
//
// **開機自啟不再帶 --hidden**（三個平台一起）。從前自啟的那一次會把角色藏起來、
// 連啟動畫面都不生（main.cpp 的 showSplash 與 windowManager.setVisible 都 gate
// 在這個旗標上），立意是「登入時安靜地縮在系統匣」；但系統匣選單裡根本沒有
// 「顯示角色」這一項（tray.cpp 只建大小／透明度／位置／設定／離開），使用者得先
// 開設定視窗、切到「一般」分頁才找得到那個核取方塊 —— 症狀就是「開機之後只有
// 托盤圖示，模型和 splash 都不見了，而且看不出來要去哪裡把它叫回來」。
// 旗標本身留著：手動下 --hidden 仍然有效，只是沒有任何一條自啟路徑會替使用者加上它。

#include <string>
#include <vector>

namespace l2m::platform {

// 手動指定的旗標：這次啟動不顯示角色，安靜地縮在系統匣。
// **不再由開機自啟寫入**，理由見檔頭。
inline constexpr const char* kHiddenFlag = "--hidden";

// 自啟項要寫的那一行（Windows 的 HKCU\...\Run 值、Linux 的 .desktop Exec=）。
// 就是執行檔路徑本身，不帶任何參數；路徑一律加引號，含空白的安裝路徑才不會被切斷。
std::string autostartCommandLine(const std::string& exePath);

// 這次啟動要不要隱藏。三個平台都是純看 argv，沒有任何一邊需要回頭問系統
//「是不是以登入項目啟動的」—— 自啟本來就跟一般啟動走同一條路了。
bool shouldStartHidden(const std::vector<std::string>& args);

// 既有使用者手上那筆舊自啟項要不要就地改寫。
//
// 光是改掉 autostartCommandLine 是不夠的：那支只在使用者按下「開機自動啟動」
// 核取方塊的那一刻才寫，**已經開著自啟的人手上那一行不會自己更新** ——
// 症狀就是「明明修好了，重開機起來還是只有托盤圖示」，而且使用者得先想到
// 要把核取方塊關掉再打開。所以啟動時看一眼，是舊格式就當場改寫
//（platform::refreshOpenAtLogin，Windows 與 Linux 各自去讀自己那份）。
//
// **只認「這支執行檔 + 隱藏旗標」這一種形狀**，別的一律不碰：路徑對不上就代表
// 那一筆是另一個安裝寫的（安裝版與便攜版並存很常見），改寫它等於把別人的自啟項
// 劫持成自己的；後面多接了其他參數的也不動，那是使用者自己改過的。
bool needsHiddenFlagStripped(const std::string& storedLine, const std::string& exePath);

}  // namespace l2m::platform
