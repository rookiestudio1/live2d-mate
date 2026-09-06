#include "user_idle.h"

#include <windows.h>

namespace l2m {
namespace platform {

std::optional<double> userIdleMs() {
  LASTINPUTINFO lii{};
  lii.cbSize = sizeof(lii);  // 沒填 cbSize 直接失敗
  if (!GetLastInputInfo(&lii)) return std::nullopt;
  // 刻意不是 GetTickCount64()：dwTime 是 32 位元時基，
  // 無號減法自動處理 49.7 天的繞回（理由見標頭）
  const DWORD now = GetTickCount();
  return static_cast<double>(now - lii.dwTime);
}

}  // namespace platform
}  // namespace l2m
