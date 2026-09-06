#include "motion_builder.h"

#include <yyjson.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>

namespace l2m {

namespace {

// Cubism 的線性段型別編號。這裡只用線性，AI 要曲線可以自己多切幾個影格。
constexpr double kLinear = 0;

constexpr double kDefaultFadeMs = 200;

// 毫秒轉秒，順便修掉浮點誤差 —— Segments 裡塞 0.30000000000000004 很難看也難測
double toSeconds(double ms) { return std::round(ms) / 1000; }

struct Point {
  double t = 0;
  double v = 0;
};

}  // namespace

Motion3 buildMotion3(const std::vector<Keyframe>& keyframes, const BuildMotionOptions& options) {
  if (keyframes.size() < 2) {
    throw std::runtime_error("A motion needs at least two keyframes to have a timeline");
  }
  for (const auto& frame : keyframes) {
    if (frame.at < 0) throw std::runtime_error("Keyframe times must not be negative");
  }

  std::vector<Keyframe> sorted = keyframes;
  std::stable_sort(sorted.begin(), sorted.end(), [](const Keyframe& a, const Keyframe& b) { return a.at < b.at; });
  const double durationMs = sorted.back().at;
  if (durationMs <= 0) {
    throw std::runtime_error("All keyframes are at the same time, so the motion duration would be zero");
  }

  // 依參數收點。保留「第一次出現」的順序，產出的曲線順序才穩定可測。
  std::vector<std::pair<std::string, std::vector<Point>>> points;
  auto findPoints = [&points](const std::string& id) -> std::vector<Point>* {
    for (auto& [key, list] : points) {
      if (key == id) return &list;
    }
    return nullptr;
  };
  for (const auto& frame : sorted) {
    for (const auto& [id, value] : frame.params) {
      if (!std::isfinite(value)) continue;
      std::vector<Point>* list = findPoints(id);
      if (!list) {
        points.emplace_back(id, std::vector<Point>{});
        list = &points.back().second;
      }
      // 同一個參數在同一毫秒被寫兩次時，後寫的贏
      if (!list->empty() && list->back().t == frame.at) list->pop_back();
      list->push_back({frame.at, value});
    }
  }

  if (points.empty()) {
    throw std::runtime_error("The keyframes contain no parameters to animate");
  }

  Motion3 motion;
  int totalSegments = 0;
  int totalPoints = 0;

  for (auto& [id, list] : points) {
    // 頭尾補平，保證每條曲線都有兩個以上的點（零段曲線會讓 Cubism 算不出值）
    if (list.front().t > 0) list.insert(list.begin(), {0, list.front().v});
    if (list.back().t < durationMs) list.push_back({durationMs, list.back().v});

    Motion3Curve curve;
    curve.id = id;
    curve.segments = {toSeconds(list[0].t), list[0].v};
    for (size_t i = 1; i < list.size(); ++i) {
      curve.segments.push_back(kLinear);
      curve.segments.push_back(toSeconds(list[i].t));
      curve.segments.push_back(list[i].v);
    }

    motion.curves.push_back(std::move(curve));
    totalSegments += static_cast<int>(list.size()) - 1;
    totalPoints += static_cast<int>(list.size());
  }

  motion.meta.duration = toSeconds(durationMs);
  motion.meta.fps = 30;
  motion.meta.loop = options.loop;
  motion.meta.areBeziersRestricted = true;
  motion.meta.curveCount = static_cast<int>(motion.curves.size());
  motion.meta.totalSegmentCount = totalSegments;
  motion.meta.totalPointCount = totalPoints;
  motion.meta.userDataCount = 0;
  motion.meta.totalUserDataSize = 0;
  motion.meta.fadeInTime = toSeconds(options.fadeInMs.value_or(kDefaultFadeMs));
  motion.meta.fadeOutTime = toSeconds(options.fadeOutMs.value_or(kDefaultFadeMs));
  return motion;
}

std::string toMotion3Json(const Motion3& motion) {
  yyjson_mut_doc* doc = yyjson_mut_doc_new(nullptr);
  yyjson_mut_val* root = yyjson_mut_obj(doc);
  yyjson_mut_doc_set_root(doc, root);

  yyjson_mut_obj_add_int(doc, root, "Version", 3);

  yyjson_mut_val* meta = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_val(doc, root, "Meta", meta);
  yyjson_mut_obj_add_real(doc, meta, "Duration", motion.meta.duration);
  yyjson_mut_obj_add_int(doc, meta, "Fps", motion.meta.fps);
  yyjson_mut_obj_add_bool(doc, meta, "Loop", motion.meta.loop);
  yyjson_mut_obj_add_bool(doc, meta, "AreBeziersRestricted", motion.meta.areBeziersRestricted);
  yyjson_mut_obj_add_int(doc, meta, "CurveCount", motion.meta.curveCount);
  yyjson_mut_obj_add_int(doc, meta, "TotalSegmentCount", motion.meta.totalSegmentCount);
  yyjson_mut_obj_add_int(doc, meta, "TotalPointCount", motion.meta.totalPointCount);
  yyjson_mut_obj_add_int(doc, meta, "UserDataCount", motion.meta.userDataCount);
  yyjson_mut_obj_add_int(doc, meta, "TotalUserDataSize", motion.meta.totalUserDataSize);
  yyjson_mut_obj_add_real(doc, meta, "FadeInTime", motion.meta.fadeInTime);
  yyjson_mut_obj_add_real(doc, meta, "FadeOutTime", motion.meta.fadeOutTime);

  yyjson_mut_val* curves = yyjson_mut_arr(doc);
  yyjson_mut_obj_add_val(doc, root, "Curves", curves);
  for (const auto& curve : motion.curves) {
    yyjson_mut_val* c = yyjson_mut_arr_add_obj(doc, curves);
    yyjson_mut_obj_add_str(doc, c, "Target", "Parameter");
    yyjson_mut_obj_add_strcpy(doc, c, "Id", curve.id.c_str());
    yyjson_mut_val* segments = yyjson_mut_arr(doc);
    yyjson_mut_obj_add_val(doc, c, "Segments", segments);
    for (double n : curve.segments) yyjson_mut_arr_add_real(doc, segments, n);
  }

  // pretty-print：CubismJson 的數字解析只接受「換行或逗號」作結尾，緊湊格式會被判錯
  char* raw = yyjson_mut_write(doc, YYJSON_WRITE_PRETTY_TWO_SPACES, nullptr);
  std::string result = raw ? raw : "";
  if (raw) free(raw);
  yyjson_mut_doc_free(doc);
  return result;
}

int nextAiMotionSlot(std::optional<int> playingSlot) { return playingSlot.has_value() && *playingSlot == 0 ? 1 : 0; }

}  // namespace l2m
