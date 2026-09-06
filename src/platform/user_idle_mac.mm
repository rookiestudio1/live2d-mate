#include "user_idle.h"

#include <CoreGraphics/CoreGraphics.h>

namespace l2m {
namespace platform {

std::optional<double> userIdleMs() {
  // CombinedSessionState 涵蓋整個登入工作階段的鍵盤滑鼠事件，
  // 走 CoreGraphics，**不需要輔助使用權限**
  const CFTimeInterval seconds = CGEventSourceSecondsSinceLastEventType(
    kCGEventSourceStateCombinedSessionState, kCGAnyInputEventType);
  return seconds * 1000.0;
}

}  // namespace platform
}  // namespace l2m
