#pragma once

// OS 層的「真閒置」：距離上一次任何鍵盤／滑鼠輸入的毫秒數。
//
// nullopt = 這個平台拿不到或系統呼叫失敗。**呼叫端必須能在沒有這個訊號時
// 照常運作**（PresenceTracker 拿到 nullopt 會永遠維持 Active —— 寧可多演
// 也不要誤判成「沒人」而整個安靜下來）。
//
// Windows 唯一的陷阱：LASTINPUTINFO::dwTime 是 **32 位元 GetTickCount 時基**，
// 約 49.7 天繞回。必須用 32 位元無號減法 —— 拿 GetTickCount64() 的 64 位元值
// 去減 32 位元的 dwTime，開機超過 49.7 天之後會算出 49 天的閒置時間，
// 桌寵從此再也不動。實作見 user_idle_win.cpp。
//
// 已知限制（會變成使用者回報的「bug」，先寫在這裡）：不計入 UIPI 阻擋的輸入
//（權限更高的視窗裡打字看不到）、不計入純影片播放 —— 它回答的是
// 「有沒有人在打字／動滑鼠」，不是「有沒有人在」。
// 另外 Windows 的 GetTickCount 在 S3 睡眠期間停止，睡了三小時醒來這個值可能
// 只有幾秒 —— 休眠要靠牆上時鐘跳躍另外偵測（core/presence_tracker.h）。

#include <optional>

namespace l2m {
namespace platform {

std::optional<double> userIdleMs();

}  // namespace platform
}  // namespace l2m
