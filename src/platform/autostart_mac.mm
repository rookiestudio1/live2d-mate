// macOS 的開機自啟實作，對應 autostart_win.cpp。
//
// 用 SMAppService（macOS 13+）而不是舊的 LSSharedFileList：後者從 10.11 起
// 就標為 deprecated，新的 SDK 已經拿不到。
//
// 註冊的是「隨 bundle 附的 launchd agent」而不是 mainAppService。
// 當初挑它的理由是「要能把 --hidden 寫進 ProgramArguments」，而**那個理由已經沒有了**
//（自啟不再帶旗標，見 core/autostart_command.h）；仍然維持 agent 的原因是換過去有實害：
// 已經註冊過的使用者機器上那份 agent 還在 launchd 裡掛著 RunAtLoad，
// 改用 mainAppService 不會順手把它撤掉，登入時就會被叫起來兩次
// —— 而換過去一點好處都沒有（顯示名稱本來就靠 AssociatedBundleIdentifiers 解決了）。
// plist 的內容與各鍵的理由見 resources/macos/com.live2dmate.app.login.plist。
//
// 注意：SMAppService 認的是 app bundle（要有 CFBundleIdentifier，
// 由 CMakeLists 的 MACOSX_BUNDLE_GUI_IDENTIFIER 設定），
// 直接跑 Contents/MacOS/ 底下的執行檔以外的情境沒問題，但沒有 .app 就註冊不起來。

#import <ServiceManagement/ServiceManagement.h>

#include "autostart.h"

#include <QDebug>

namespace l2m::platform {

namespace {

// 檔名要跟 resources/macos/ 底下那份一致，CMake 會把它複製進
// Contents/Library/LaunchAgents/，SMAppService 只認這個目錄
NSString* const kAgentPlistName = @"com.live2dmate.app.login.plist";

}  // namespace

void setOpenAtLogin(bool enabled) {
  if (@available(macOS 13.0, *)) {
    SMAppService* service = [SMAppService agentServiceWithPlistName:kAgentPlistName];
    // 沒註冊過還去 unregister 會回一個 jobNotFound 錯誤，這不是問題，先擋掉
    if (!enabled && service.status == SMAppServiceStatusNotRegistered) return;

    NSError* error = nil;
    const BOOL ok = enabled ? [service registerAndReturnError:&error]
                            : [service unregisterAndReturnError:&error];
    if (!ok) {
      qWarning() << "[autostart] 設定開機自啟失敗:" << (enabled ? "register" : "unregister")
                 << QString::fromNSString(error.localizedDescription);
    }
  } else {
    qWarning() << "[autostart] 開機自啟需要 macOS 13 以上";
  }
}

// macOS 不需要遷移：參數寫在隨 app bundle 出貨的 plist 裡（不像 Windows 的註冊表
// 與 Linux 的 .desktop 是安裝當下寫進使用者家目錄的一份複本），app 一更新就跟著換掉了。
void refreshOpenAtLogin() {}

bool isOpenAtLogin() {
  if (@available(macOS 13.0, *)) {
    // 使用者可能在「系統設定 → 一般 → 登入項目」把它關掉，
    // 那時狀態是 RequiresApproval / NotFound，都不算已啟用
    return [SMAppService agentServiceWithPlistName:kAgentPlistName].status ==
           SMAppServiceStatusEnabled;
  }
  return false;
}

}  // namespace l2m::platform
