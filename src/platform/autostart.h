#pragma once

// 開機自啟。兩個平台的語意一致：
//   Windows：HKCU\...\Run 寫入 "exe路徑" --hidden（自啟時不顯示角色以外的東西）
//   macOS：SMAppService（13+）註冊一份隨 bundle 附的 launchd agent，
//         旗標寫在它的 ProgramArguments 裡，語意與 Windows 那條完全一致

// 純字串的那一半（命令列組裝、旗標判讀）在 core，測試才連得到
#include "core/autostart_command.h"

namespace l2m::platform {

void setOpenAtLogin(bool enabled);
bool isOpenAtLogin();

}  // namespace l2m::platform
