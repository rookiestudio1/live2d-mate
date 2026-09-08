#pragma once

// 開機自啟。三個平台的語意一致 —— **自啟就是一次普通的啟動**，角色照樣顯示、
// splash 照樣跑，不帶任何旗標（不再有 --hidden，理由見 core/autostart_command.h）：
//   Windows：HKCU\...\Run 寫入 "exe路徑"
//   Linux：~/.config/autostart/live2d_mate.desktop 的 Exec= 寫同一行
//   macOS：SMAppService（13+）註冊一份隨 bundle 附的 launchd agent

// 純字串的那一半（命令列組裝、旗標判讀）在 core，測試才連得到
#include "core/autostart_command.h"

namespace l2m::platform {

void setOpenAtLogin(bool enabled);
bool isOpenAtLogin();

// 啟動時呼叫一次：既有使用者手上那筆自啟項若還是舊的「exe --hidden」，就地改寫成
// 現在的格式。判別在 core 的 needsHiddenFlagStripped()（連什麼都不改寫的理由也在那裡）。
// macOS 是 no-op：參數寫在隨 bundle 出貨的 plist 裡，app 一更新就跟著換掉了。
void refreshOpenAtLogin();

}  // namespace l2m::platform
