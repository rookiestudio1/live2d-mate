#pragma once

// 啟動 Splash 的時間計算。
//
// 為什麼需要 splash：ModelController::load() 是同步的，
// 8192² 貼圖的模型會把 GUI 執行緒卡住數秒（見 app/splash_process.h）。
//
// 為什麼抽到 core：理由跟 core/tray_label.h、core/autostart_command.h 一樣 ——
// 測試全部只連 l2m_core，SplashWindow 是 QWidget 測不到，
// 但「光條在時間 t 該在哪」與「還要撐多久才能淡出」是純數學，值得釘住。

namespace l2m {

// 不確定進度條的光條左緣（相對軌道左緣，可為負）。
// 光條從 -segmentWidth 一路滑到 trackWidth（兩端都完全滑出畫面才回頭），
// 中間套 smoothstep，看起來像加速衝出去再減速停住。
//
// 相位刻意由「經過的毫秒」推而不是幀計數：重繪節奏不保證穩定（16 ms 的
// timer 在系統忙碌時會漏拍），用幀計數會讓掃描速度忽快忽慢。
//
// cycleMs <= 0 時回傳起始位置（除零保護，不是錯誤）。
double splashSweepOffset(double elapsedMs, double trackWidth, double segmentWidth, double cycleMs);

// 還要撐多久才可以開始淡出（毫秒）；0 = 現在就可以。
//
// 存在的理由：小模型 0.2 秒就載完，splash 一閃而過比不顯示還難看。
// elapsedMs 從 begin() 起算，所以淡入的時間也算在最短顯示時間裡。
double splashRemainingHoldMs(double elapsedMs, double minVisibleMs);

}  // namespace l2m
