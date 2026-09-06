#pragma once

// 在席偵測：把「OS 閒置毫秒數」的取樣序列變成 Active/Away 狀態與
// 回座（CameBack）／休眠喚醒（Resumed）／久用（LongSession）轉場事件。
// 歡迎詞的三個觸發裡有兩個靠它（第三個是啟動時的 firstFrameRendered）。
//
// 為什麼要另外偵測休眠、不能靠 userIdleMs()：**Windows 的 GetTickCount 在 S3
// 睡眠期間是停止的**，機器睡了三小時醒來，GetLastInputInfo 算出來的閒置時間
// 可能只有幾秒。牆上時鐘不受影響，拿它跟心跳間隔比就抓得到 ——
// 完全不必碰任何 Win32 API，測試注兩個假時鐘就驗得到。
//
// 已知誤判：手動改系統時間、時區切換、NTP 大幅校正也會讓牆上時鐘跳躍。
// resumeGapMs = 5 分鐘讓它罕見，而且誤判的後果只是多講一句歡迎詞 —— 可接受。

#include <optional>

namespace l2m {

enum class PresenceState { Active, Away };

struct PresenceEvent {
  enum class Kind { None, WentAway, CameBack, Resumed, LongSession };
  Kind kind = Kind::None;
  double awayForMs = 0;     // CameBack / Resumed：離開了多久（決定要不要打招呼）
  double sessionForMs = 0;  // LongSession：連續使用了多久
};

class PresenceTracker {
public:
  struct Options {
    double awayAfterMs = 600000;       // 10 分沒輸入 ＝ 離座
    double greetAfterAwayMs = 900000;  // 至少離開這麼久才值得打招呼（CameBack 才發）
    double resumeGapMs = 300000;       // 牆上時鐘跳超過 5 分鐘 ＝ 機器睡過
    double longSessionMs = 3600000;    // 連續使用滿 1 小時發 LongSession（久坐提醒用）
  };
  // 不寫 `Options options = {}` 的預設引數：預設引數屬於外層類別的 complete-class
  // context，會在類別本身還沒定義完成時就要用到 Options 的成員預設值 ——
  // clang 直接報錯（MSVC 放行），所以拆成兩個建構子。
  PresenceTracker() = default;
  explicit PresenceTracker(Options options) : options_(options) {}

  // 設定熱更新（autonomy.awayAfterMs 改了立刻生效）。不影響目前狀態。
  void setOptions(Options options) { options_ = options; }

  // 心跳取樣（呼叫端約每 5 秒一次）。
  // monotonicMs：單調時鐘。wallMs：牆上時鐘（偵測休眠）。
  // idleMs 為 nullopt 時（平台拿不到訊號）**永遠維持 Active**，行為退回沒有
  // 這個功能的樣子 —— 寧可多演也不要誤判成「沒人」而整個安靜下來。
  // 一次取樣至多回一個事件；優先序 Resumed > 狀態轉場 > LongSession。
  PresenceEvent sample(double monotonicMs, double wallMs, std::optional<double> idleMs);

  PresenceState state() const { return state_; }

  // 連續使用了多久（Away 時為 0）。久坐提醒（階段 3）用。
  double continuousActiveMs(double monotonicMs) const;

private:
  Options options_;
  PresenceState state_ = PresenceState::Active;
  bool hasSample_ = false;
  double lastWallMs_ = 0;
  double activeSinceMs_ = 0;  // 這一段連續使用的起點（LongSession 發過就重新起算）
  double awaySinceMs_ = 0;    // 進入 Away 的時刻（回推到最後一次輸入）
};

}  // namespace l2m
