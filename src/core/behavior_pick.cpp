#include "behavior_pick.h"

#include <algorithm>

namespace l2m {

std::optional<std::string> BehaviorPicker::pick(const std::vector<PickCandidate>& candidates, double nowMs, double unit) {
  // 1. 權重 <= 0 直接出局
  std::vector<const PickCandidate*> eligible;
  for (const auto& candidate : candidates) {
    if (candidate.weight > 0) eligible.push_back(&candidate);
  }
  if (eligible.empty()) return std::nullopt;

  // 2. 不重複最近 N 個 —— 除非會把候選清空（只有一個候選時永遠回它）
  if (options_.noRepeatLast > 0) {
    std::vector<const PickCandidate*> fresh;
    for (const auto* candidate : eligible) {
      const bool recentlyUsed = std::find(recent_.begin(), recent_.end(), candidate->id) != recent_.end();
      if (!recentlyUsed) fresh.push_back(candidate);
    }
    if (!fresh.empty()) eligible = std::move(fresh);
  }

  // 3. 冷卻。全部都在冷卻時退回冷卻剩餘最短的那一個，不回 nullopt
  if (options_.cooldownMs > 0) {
    std::vector<const PickCandidate*> ready;
    const PickCandidate* soonest = nullptr;
    double soonestLastUsed = 0;
    for (const auto* candidate : eligible) {
      const auto it = lastUsed_.find(candidate->id);
      const bool cooling = it != lastUsed_.end() && nowMs - it->second < options_.cooldownMs;
      if (!cooling) {
        ready.push_back(candidate);
      } else if (!soonest || it->second < soonestLastUsed) {
        // 最早被用的那一個剩餘冷卻最短
        soonest = candidate;
        soonestLastUsed = it->second;
      }
    }
    if (ready.empty()) {
      lastUsed_[soonest->id] = nowMs;
      recent_.push_back(soonest->id);
      while (static_cast<int>(recent_.size()) > options_.noRepeatLast) recent_.pop_front();
      return soonest->id;
    }
    eligible = std::move(ready);
  }

  // 4. 加權隨機：unit ∈ [0,1) 映到總權重的位置
  double total = 0;
  for (const auto* candidate : eligible) total += candidate->weight;
  double target = std::clamp(unit, 0.0, 1.0) * total;
  const PickCandidate* chosen = eligible.back();  // 浮點誤差時的保底
  for (const auto* candidate : eligible) {
    target -= candidate->weight;
    if (target < 0) {
      chosen = candidate;
      break;
    }
  }

  lastUsed_[chosen->id] = nowMs;
  recent_.push_back(chosen->id);
  while (static_cast<int>(recent_.size()) > options_.noRepeatLast) recent_.pop_front();
  return chosen->id;
}

void BehaviorPicker::forget(const std::string& id) {
  lastUsed_.erase(id);
  recent_.erase(std::remove(recent_.begin(), recent_.end(), id), recent_.end());
}

void BehaviorPicker::reset() {
  lastUsed_.clear();
  recent_.clear();
}

std::optional<double> BehaviorPicker::lastUsedMs(const std::string& id) const {
  const auto it = lastUsed_.find(id);
  if (it == lastUsed_.end()) return std::nullopt;
  return it->second;
}

}  // namespace l2m
