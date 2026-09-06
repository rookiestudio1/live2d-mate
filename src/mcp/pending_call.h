#pragma once

// 一次跨執行緒的工具呼叫。
//
// httplib 的工作執行緒建立它、投遞給 GUI 執行緒，然後在這裡等結果；
// GUI 執行緒可以當場填答（99% 的工具是同步的），也可以先扣住、之後在
// 非同步完成點才填（speak wait=true、perform）。
//
// 鐵則：**GUI 執行緒永遠不阻塞**。所以跨執行緒投遞用 QueuedConnection，
// 被擋住的是 httplib 的 worker —— 那正是 worker pool 存在的意義。
//
// 等待端有硬性期限，過期就把自己標成已放棄；晚到的 complete() 會被丟掉，
// 不會寫進已經回收的緩衝區。

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

namespace l2m {

class PendingCall {
public:
  PendingCall(std::string tool, std::string argsJson);

  const std::string& tool() const { return tool_; }
  const std::string& argsJson() const { return argsJson_; }

  // GUI 執行緒：填結果並喚醒等待端。重複呼叫或已放棄時是 no-op。
  void complete(std::string outputJson);

  // httplib 執行緒：等到期限為止。逾時回 false 並把自己標成已放棄。
  bool wait(std::chrono::milliseconds timeout);

  // 只有 wait() 回 true 之後讀才有意義
  const std::string& output() const { return output_; }

  // 關閉時用：讓所有還在等的呼叫立刻失敗，不必等滿 185 秒
  bool abandoned() const;

private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool done_ = false;
  bool abandoned_ = false;
  std::string tool_;
  std::string argsJson_;
  std::string output_;
};

using PendingCallPtr = std::shared_ptr<PendingCall>;

}  // namespace l2m
