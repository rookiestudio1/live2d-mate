#include "console.h"

namespace l2m {
namespace platform {

void attachParentConsole() {
  // Linux 沒有「GUI 子系統」這回事：從終端機啟動時 stderr 本來就接在那個
  // 終端機上，桌面環境啟動時則進 session 的 journal。不必做事（同 macOS）。
}

}  // namespace platform
}  // namespace l2m
