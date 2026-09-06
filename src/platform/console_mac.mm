#include "console.h"

namespace l2m {
namespace platform {

// macOS 沒有 Windows 那種「GUI 子系統沒有 console」的分野：
// 從終端機啟動時 stderr 本來就是那個終端機，用 .app bundle 開就進系統紀錄。
// 兩種情況都不需要額外接線
void attachParentConsole() {}

}  // namespace platform
}  // namespace l2m
