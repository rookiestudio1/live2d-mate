#pragma once

// 未觸發排程的帳本。
//
// 修的是一個具體的 bug：AppController::isBusy() 原本定義為「還有排程沒跑完就算忙」，
// 而 MCP 的 schedule 延遲上限是 6 小時 —— AI 只要排一個長排程，閒置復原與閒置表演
// 就雙雙停擺整整 6 小時。正確的語意是「忙」只涵蓋**馬上要發生**的事：
// 6 小時後才到期的排程不該擋住現在的表演，30 秒後就要跑的才該。
//
// 只記 id 與到期時刻，計時器本身仍由 TimerHost 管 —— 這裡是純資料，
// 時間由呼叫端傳入（production 用 QElapsedTimer，測試餵固定值），才測得到。

#include <utility>
#include <vector>

namespace l2m {

// 「馬上要發生」的視野。60 秒是猜的數字，理由：它同時小於預設
// performIntervalMs（60000，太長會讓長排程繼續凍結表演）、也遠小於預設
// resetMs（120000，太短的話 AI 排 90 秒後的動作，中間可能被 resetToIdle
// 清掉它剛設好的表情）。
inline constexpr double kScheduleBusyHorizonMs = 60000;

class ScheduleBook {
public:
  // 記一筆排程。同一個 id 重複 add 是覆蓋（計時器 id 不會重複，防禦性語意）。
  void add(int id, double dueAtMs);
  void remove(int id);

  // 還沒觸發的全部 id（解構時逐一 clearTimeout 用）
  std::vector<int> ids() const;

  // 視野內（含已過期還沒跑的）有沒有排程要觸發
  bool busyWithin(double nowMs, double horizonMs) const;

  bool empty() const { return entries_.empty(); }

private:
  // id → 到期時刻。筆數個位數（MCP 長呼叫上限 4），線性掃就好
  std::vector<std::pair<int, double>> entries_;
};

}  // namespace l2m
