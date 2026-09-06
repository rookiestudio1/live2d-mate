#pragma once

// app 閒置與 OS 真閒置的合成規則。
//
// 兩個訊號的語意不同：app 閒置 =「距上次碰角色／AI 下指令多久」（touchIdle），
// OS 閒置 =「距上次任何鍵盤滑鼠輸入多久」（platform/user_idle.h）。
// 閒置**分級**（idleLevelFor）要的是「使用者發呆多久了」—— 你在旁邊狂打程式碼
// 但三小時沒碰角色，不代表你發呆三小時，所以取**小**的那一個。
//
// 刻意不把 OS 訊號接上 touchIdle：那會讓每一個鍵擊都重新倒數 performAfterMs，
// 桌寵永遠不表演 —— 與目標完全相反。OS 訊號只降級、不重置。

#include <algorithm>
#include <optional>

namespace l2m {

inline double effectiveIdleMs(double appIdleMs, std::optional<double> osIdleMs) {
  // 平台拿不到訊號時退回 app 閒置：行為與沒有這個功能的版本一模一樣
  if (!osIdleMs) return appIdleMs;
  return std::min(appIdleMs, *osIdleMs);
}

}  // namespace l2m
