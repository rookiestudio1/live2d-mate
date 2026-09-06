#pragma once

// 閒置行為的計時邏輯。
//
// 刻意不知道要清什麼、也不知道什麼叫「有活動」—— 那些由呼叫端注入，
// 這裡只管計時，才測得動也才不會跟 AppController 綁死。
// 計時器本身也抽象成 TimerHost：production 用 QTimer，測試用假時鐘。

#include <functional>

namespace l2m {

// setTimeout / clearTimeout 語意的抽象。回傳的 id 供取消用。
class TimerHost {
public:
  virtual ~TimerHost() = default;
  virtual int setTimeout(std::function<void()> fn, double delayMs) = 0;
  virtual void clearTimeout(int id) = 0;
};

// 閒置一段時間沒有動靜就把 AI 留下的狀態收乾淨。
//
// 為什麼需要這個：Live2D 的動作會自己播完、待機動作接手，但「狀態型」指令不會 ——
// 表情、沒帶 hold_ms 的參數覆寫、循環中的合成動作，設了就一直在。
// AI 講完那一輪就結束了，沒有「回來收尾」的機制，所以清理只能由 app 自己做。
class IdleReset {
public:
  struct Deps {
    // 閒置多久才動作（毫秒）；0 或負值代表停用
    std::function<double()> delayMs;
    // 現在還不能清（正在說話、還有排程沒跑）；為真時延後而不是硬清
    std::function<bool()> isBusy;
    std::function<void()> reset;
  };

  IdleReset(TimerHost& timers, Deps deps) : timers_(timers), deps_(std::move(deps)) {}
  ~IdleReset() { stop(); }

  // 有活動：重新計時。設定停用時等同 stop()。
  void touch();
  void stop();

  // 是否正在倒數。設定變更時用它判斷要不要重新套用新的秒數。
  bool pending() const { return timerId_ != -1; }

private:
  void fire();

  TimerHost& timers_;
  Deps deps_;
  int timerId_ = -1;
};

// MCP 收工之後的短復原：AI 下完最後一個指令、話也講完，再安靜這麼久就把它
// 留下的表情與動作收乾淨。IdleReset 的兩分鐘是「沒人理它」的尺度，對一輪
// AI 對話來說太長 —— AI 講完那一段就走了，表情會一直掛在臉上到下一次互動。
inline constexpr double kMcpQuietResetMs = 5000;

// 與 IdleReset 的差別只有「什麼時候該倒數」，所以是包一層而不是另寫一套：
//   IdleReset     —— 永遠在倒數（idle.resetMs），使用者互動也算一次活動。
//   McpQuietReset —— 只有 MCP 真的下過指令才倒數，觸發一次就熄火等下一個指令。
//
// **倒數必須從「講完」起算，不是從「指令送出」起算**：AI 的典型序列是
// set_expression 之後接一句長台詞，從送出那一刻數，話還沒講完表情就被抽掉了。
// 兩層保險缺一不可 ——
//   ① onActivity() 由 AppController::touchIdle() 呼叫，而 SpeechController::
//      finished（佇列真的清空才發，連續 speak 中間不會落下來）已經接在那上面，
//      所以最後一句播完會讓倒數重新給滿。
//   ② 說話期間 isBusy() 為真，IdleReset::fire() 本來就是延後重排而不是硬清。
// 只留 ① 會被「合成中還沒開口」的空窗鑽過去；只留 ② 則會用掉說話前剩下的
// 殘值，變成講完隨即就清。
class McpQuietReset {
public:
  struct Deps {
    // 現在還不能清（說話中、表演中、還有排程馬上要跑）
    std::function<bool()> isBusy;
    std::function<void()> reset;
  };

  McpQuietReset(TimerHost& timers, Deps deps);

  // 任何一個 MCP 工具被呼叫。唯讀的 get_state／list_* 也算 —— AI 還在呼叫工具
  // 就代表它還在這一輪對話裡，不該把它上一步設好的表情抽掉。
  void onCommand();
  // 有活動（含 TTS 講完）：驅動中才重新給滿倒數，沒有 MCP 驅動時什麼都不做
  void onActivity();
  void stop();

  // MCP 正在驅動（下過指令、還沒復原）
  bool driving() const { return driving_; }
  bool pending() const { return inner_.pending(); }

private:
  // 復原真的發生了：先熄火再交棒，之後要等下一個 MCP 指令才會重新倒數
  void fire();

  bool driving_ = false;
  std::function<void()> reset_;
  IdleReset inner_;
};

// 閒置表演：一段時間沒有 MCP 指令與滑鼠點擊，就定期隨機換表情、播動作，
// 讓桌面夥伴不會長時間僵著。
//
// 跟 IdleReset 一樣只管計時，「忙不忙」「表演什麼」由呼叫端注入。
// 兩者刻意不共用：IdleReset 是一次性「倒數→觸發→休眠等下一次 touch」，
// 這裡是「倒數 idleMs → 進入表演 → 每 intervalMs 循環 → touch 才離開」；
// busy 時 IdleReset 重排整段 delay，這裡只跳過該輪、下一輪照排。
class IdlePerformer {
public:
  enum class State { Stopped, Waiting, Performing };

  struct Deps {
    // 進入表演前要閒置多久（毫秒）；0 或負值代表停用
    std::function<double()> idleMs;
    // 表演間隔（毫秒）；每輪排程時重新讀取，設定熱更新自然生效
    std::function<double()> intervalMs;
    // main 端就知道的忙碌（說話中、排程沒跑完）；為真時跳過該輪但不退出表演
    std::function<bool()> isBusy;
    std::function<void()> perform;
  };

  IdlePerformer(TimerHost& timers, Deps deps) : timers_(timers), deps_(std::move(deps)) {}
  ~IdlePerformer() { stop(); }

  // 有活動：離開表演狀態，重新倒數 idleMs。設定停用時等同 stop()。
  void touch();
  void stop();

  State state() const { return mode_; }

  // 設定變更後呼叫：停用就 stop，啟用中就 touch 重新倒數
  void refresh();

private:
  void cycle();
  void scheduleNext();

  TimerHost& timers_;
  Deps deps_;
  int timerId_ = -1;
  State mode_ = State::Stopped;
};

}  // namespace l2m
