#include "topmost_watchdog.h"

namespace l2m {

bool shouldReassertTopmost(bool wantTopmost, bool nativeTopmost, bool windowVisible, bool coveredByNormalWindow) {
  if (!wantTopmost) return false;
  if (!windowVisible) return false;
  // 兩種症狀擇一成立就補：旗標被拔掉，或旗標還在但已經被壓在一般視窗底下
  return !nativeTopmost || coveredByNormalWindow;
}

}  // namespace l2m
