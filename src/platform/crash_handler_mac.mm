#include "crash_handler.h"

#include <cstdlib>

namespace l2m {
namespace platform {

std::filesystem::path appDataDirFromEnv() {
  const char* home = std::getenv("HOME");
  if (!home || !*home) return {};
  return std::filesystem::path(home) / "Library" / "Application Support" / "live2d_mate";
}

// macOS 的 install 版面（資源要進 Contents/Resources）還沒接，
// 所以 crash 報告也一併留到那時候一起做。空殼保證 main.cpp 只有一份程式碼路徑
void installCrashHandler(const std::filesystem::path&, const char*) {}

// 空殼（macOS 尚未實裝 crash handler，連主執行緒的部分都是）
void installThreadCrashSupport() {}

void triggerCrashTestFromEnv() {}

}  // namespace platform
}  // namespace l2m
