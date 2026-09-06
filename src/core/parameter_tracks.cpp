#include "parameter_tracks.h"

#include <algorithm>

namespace l2m {

namespace {

// 釋放與自動歸位的淡出時間。
//
// 原本是 200 ms 線性，在畫面上跟瞬斷分不出來 —— 閒置復原把 surprised 收掉時，
// 使用者看到的是「嘴巴立刻閉上」。500 ms 才看得出是慢慢收回去，再長就會拖到
// 下一個動作的淡入。
constexpr double kReleaseFadeMs = 500;

double lerp(double from, double to, double t) { return from + (to - from) * t; }

// 0 ~ 1 的進度；duration 是 0 時直接算到位
double progress(double elapsed, double durationMs) {
  if (durationMs <= 0) return 1;
  return std::min(std::max(elapsed / durationMs, 0.0), 1.0);
}

// 淡回專用的緩動：兩端斜率都是 0 的 smoothstep。
//
// 只加在淡回、不加在補間：淡回的起點是一個「一直被釘著不動」的值，線性會在
// 放掉的那一瞬間憑空生出速度，即使拉長時間看起來仍然像被抽掉而不是放開。
// 補間那頭有呼叫端自己給的 durationMs，語意是「花這麼久走到那個值」，
// 不該由這一層改它的手感。
// 半程仍然剛好是 0.5（0.25 × 2），所以既有測試釘的中點值不受影響。
double smoothstep(double t) { return t * t * (3 - 2 * t); }

}  // namespace

std::vector<std::string> ParameterTracks::ids() const {
  std::vector<std::string> result;
  result.reserve(tracks_.size());
  for (const auto& [id, track] : tracks_) result.push_back(id);
  return result;
}

void ParameterTracks::set(const std::vector<SetParameterRequest>& requests, const ProbeFn& probe, double now) {
  for (const auto& request : requests) {
    const ParameterProbe current = probe(request.id);
    Track track;
    track.from = current.value;
    track.to = request.value;
    track.startAt = now;
    track.durationMs = std::max(request.durationMs.value_or(0.0), 0.0);
    track.holdMs = request.holdMs;
    track.base = current.base;
    track.mode = request.mode;
    track.releaseAt = std::nullopt;
    track.releaseFrom = 0;
    tracks_[request.id] = track;
  }
}

void ParameterTracks::release(const std::optional<std::vector<std::string>>& ids, double now) {
  const std::vector<std::string> targets = ids.has_value() ? *ids : this->ids();
  for (const auto& id : targets) {
    auto it = tracks_.find(id);
    if (it == tracks_.end() || it->second.releaseAt.has_value()) continue;
    it->second.releaseFrom = valueOf(it->second, now);
    it->second.releaseAt = now;
  }
}

std::vector<ActiveParameter> ParameterTracks::sample(double now) {
  std::vector<ActiveParameter> active;
  for (auto it = tracks_.begin(); it != tracks_.end();) {
    Track& track = it->second;

    // 撐完了就轉入淡回階段
    if (!track.releaseAt.has_value() && track.holdMs.has_value()) {
      const double settledAt = track.startAt + track.durationMs + *track.holdMs;
      if (now >= settledAt) {
        track.releaseFrom = track.to;
        track.releaseAt = settledAt;
      }
    }

    const bool finished = track.releaseAt.has_value() && now >= *track.releaseAt + kReleaseFadeMs;
    if (finished && track.settled) {
      it = tracks_.erase(it);
      continue;
    }
    if (finished) track.settled = true;

    active.push_back({it->first, valueOf(track, now), track.mode});
    ++it;
  }
  return active;
}

double ParameterTracks::valueOf(const Track& track, double now) const {
  if (track.releaseAt.has_value()) {
    return lerp(track.releaseFrom, track.base, smoothstep(progress(now - *track.releaseAt, kReleaseFadeMs)));
  }
  return lerp(track.from, track.to, progress(now - track.startAt, track.durationMs));
}

}  // namespace l2m
