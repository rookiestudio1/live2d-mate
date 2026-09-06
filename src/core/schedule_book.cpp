#include "schedule_book.h"

#include <algorithm>

namespace l2m {

void ScheduleBook::add(int id, double dueAtMs) {
  for (auto& entry : entries_) {
    if (entry.first == id) {
      entry.second = dueAtMs;
      return;
    }
  }
  entries_.emplace_back(id, dueAtMs);
}

void ScheduleBook::remove(int id) {
  entries_.erase(std::remove_if(entries_.begin(), entries_.end(), [id](const auto& entry) { return entry.first == id; }), entries_.end());
}

std::vector<int> ScheduleBook::ids() const {
  std::vector<int> out;
  out.reserve(entries_.size());
  for (const auto& entry : entries_) out.push_back(entry.first);
  return out;
}

bool ScheduleBook::busyWithin(double nowMs, double horizonMs) const {
  return std::any_of(entries_.begin(), entries_.end(), [nowMs, horizonMs](const auto& entry) {
    // 已過期還沒跑的也算忙：它就在下一輪事件迴圈
    return entry.second <= nowMs + horizonMs;
  });
}

}  // namespace l2m
