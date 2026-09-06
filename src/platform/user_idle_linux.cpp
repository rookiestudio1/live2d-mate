#include "user_idle.h"

namespace l2m {
namespace platform {

std::optional<double> userIdleMs() {
  // X11 可以走 XScreenSaver 擴充（XScreenSaverQueryInfo）拿到真閒置，但那要
  // 多背一個 libXss 的建置與封裝相依；Wayland 則是刻意拿不到（全域輸入被隔離）。
  // 介面已保證呼叫端能在 nullopt 下照常運作（PresenceTracker 維持 Active，
  // 寧可多演也不要誤判成「沒人」），第一階段先如實回「拿不到」。
  return std::nullopt;
}

}  // namespace platform
}  // namespace l2m
