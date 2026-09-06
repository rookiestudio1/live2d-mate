#include "pending_call.h"

#include <utility>

namespace l2m {

PendingCall::PendingCall(std::string tool, std::string argsJson) : tool_(std::move(tool)), argsJson_(std::move(argsJson)) {}

void PendingCall::complete(std::string outputJson) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // 已經回答過、或等待端已經放生，寫進去也沒人看
    if (done_ || abandoned_) return;
    output_ = std::move(outputJson);
    done_ = true;
  }
  cv_.notify_all();
}

bool PendingCall::wait(std::chrono::milliseconds timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  const bool ok = cv_.wait_for(lock, timeout, [this] { return done_; });
  if (!ok) abandoned_ = true;
  return ok;
}

bool PendingCall::abandoned() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return abandoned_;
}

}  // namespace l2m
