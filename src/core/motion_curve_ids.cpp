#include "motion_curve_ids.h"

#include <algorithm>
#include <cstring>

#include "json_doc.h"

namespace l2m {

namespace {

void addUnique(std::vector<std::string>& out, const char* id) {
  if (!id || *id == '\0') return;
  if (std::find(out.begin(), out.end(), id) != out.end()) return;
  out.emplace_back(id);
}

}  // namespace

MotionDrivenIds motionDrivenIds(const std::string& motionJson) {
  MotionDrivenIds out;

  const auto doc = jsonu::Doc::parse(motionJson);
  if (!doc) return out;
  yyjson_val* curves = jsonu::get(doc->root(), "Curves");
  if (!yyjson_is_arr(curves)) return out;

  size_t idx, max;
  yyjson_val* curve;
  yyjson_arr_foreach(curves, idx, max, curve) {
    const char* target = yyjson_get_str(jsonu::get(curve, "Target"));
    const char* id = yyjson_get_str(jsonu::get(curve, "Id"));
    if (!target) continue;
    if (std::strcmp(target, "Parameter") == 0) {
      addUnique(out.parameterIds, id);
    } else if (std::strcmp(target, "PartOpacity") == 0) {
      addUnique(out.partIds, id);
    }
  }
  return out;
}

}  // namespace l2m
