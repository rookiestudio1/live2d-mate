#pragma once

// 角色淡入淡出的時長常數與狀態機。
//
// 角色是自己每幀畫出來的 GL 內容，沒有現成的過場動畫可以交辦，
// 「現在該是多少 alpha」必須有人算。
//
// 為什麼抽到 core：理由跟 core/splash_timing.h 一樣 —— 測試全部只連 l2m_core，
// CharacterWindow 需要 GL context 測不到，但這裡是純數學，值得逐條釘住。
//
// 用在四個時機（全部吃同一個 kFadeDurationMs）：
//   啟動與切換模型的淡入（模型載入完成的那一幀起算）
//   切換模型的淡出（必須整整跑完才能開始載入 —— 載入是同步的，
//                   一進去 GUI 執行緒就凍住，見 app/splash_process.h）
//   離開程式的淡出、隱藏／顯示角色

namespace l2m {

// 淡入淡出時長（毫秒）。四個時機共用這一個值，不做成設定項。
inline constexpr double kFadeDurationMs = 400.0;

class Fade {
public:
  // 立刻定住在某個 alpha，不做動畫。
  // 模型載入完成的瞬間要先 set(0) 再 begin(1)，載入後的第一幀才會是全透明的
  //（等下一幀才設的話，第一幀已經以全不透明畫上螢幕，淡入就白做了）。
  void set(double alpha);

  // 從「此刻的 alpha」出發走向 target。
  //
  // 起點刻意取 alphaAt(nowMs) 而不是 0/1：淡出到一半又被切回來（連按兩次切換
  // 模型、set_visible 來回）時畫面不可以瞬間彈到另一端。
  //
  // 實際時長按剩餘距離等比縮短（kFadeDurationMs × |target - from|），
  // 每單位 alpha 的速度才一致 —— 從 0.5 淡回 1.0 若也花滿 400 ms，看起來像卡住。
  // 距離為 0 時時長為 0，finished() 當場為真（重複呼叫 setVisible(true) 是 no-op）。
  void begin(double target, double nowMs);

  double alphaAt(double nowMs) const;
  bool finished(double nowMs) const;

  double target() const { return target_; }

private:
  double from_ = 1.0;
  double target_ = 1.0;
  double startMs_ = 0.0;
  double durationMs_ = 0.0;
};

}  // namespace l2m
