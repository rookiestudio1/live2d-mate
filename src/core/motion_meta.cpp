#include "motion_meta.h"

#include <cstdint>
#include <vector>

#include "json_doc.h"

namespace l2m {

namespace {

// 與 Framework 的 CubismMotionSegmentType 對齊
constexpr int kSegmentBezier = 1;

struct Counts {
  int64_t curves = 0;
  int64_t segments = 0;
  int64_t points = 0;
};

// 走一遍 Curves 算實際數量；結構走不完回 false 並填 error。
// 規則必須與 CubismMotionJson::HasConsistency 完全一致（見標頭）。
bool countCurves(yyjson_val* curvesArr, Counts& out, std::string& error) {
  size_t curveIdx, curveMax;
  yyjson_val* curve;
  yyjson_arr_foreach(curvesArr, curveIdx, curveMax, curve) {
    ++out.curves;
    yyjson_val* segmentsVal = jsonu::get(curve, "Segments");
    if (!yyjson_is_arr(segmentsVal)) {
      error = "curve " + std::to_string(curveIdx) + " has no Segments array";
      return false;
    }

    // 攤成數字序列（yyjson 陣列隨機存取是 O(n)，先收集再走）
    std::vector<double> seg;
    seg.reserve(yyjson_arr_size(segmentsVal));
    size_t i, max;
    yyjson_val* num;
    yyjson_arr_foreach(segmentsVal, i, max, num) {
      if (!yyjson_is_num(num)) {
        error = "curve " + std::to_string(curveIdx) + " Segments has a non-number";
        return false;
      }
      seg.push_back(yyjson_get_num(num));
    }

    if (seg.empty()) continue;  // 空 curve：Parse 的迴圈不會跑，不產生點
    if (seg.size() < 2) {
      error = "curve " + std::to_string(curveIdx) + " Segments shorter than one point";
      return false;
    }

    // 開頭 2 個數字是起點
    size_t pos = 2;
    out.points += 1;
    while (pos < seg.size()) {
      const double type = seg[pos];
      size_t stride;
      int64_t points;
      if (type == 0.0 || type == 2.0 || type == 3.0) {  // 線性／階梯／反階梯
        stride = 3;
        points = 1;
      } else if (type == static_cast<double>(kSegmentBezier)) {
        stride = 7;
        points = 3;
      } else {
        error = "curve " + std::to_string(curveIdx) + " has unknown segment type " + std::to_string(type);
        return false;
      }
      if (pos + stride > seg.size()) {
        error = "curve " + std::to_string(curveIdx) + " Segments truncated mid-segment";
        return false;
      }
      out.points += points;
      out.segments += 1;
      pos += stride;
    }
  }
  return true;
}

// Meta 的整數欄位；缺少或非數字回 -1（一定與實際值不同，觸發改寫）
int64_t metaInt(yyjson_val* meta, const char* key) {
  yyjson_val* v = jsonu::get(meta, key);
  return v && yyjson_is_num(v) ? static_cast<int64_t>(yyjson_get_num(v)) : -1;
}

}  // namespace

MotionMetaFix fixMotionMetaCounts(const std::string& motionJson) {
  MotionMetaFix fix;

  const auto doc = jsonu::Doc::parse(motionJson);
  if (!doc) {
    fix.error = "JSON parse failed";
    return fix;
  }
  yyjson_val* root = doc->root();
  if (!yyjson_is_obj(root)) {
    fix.error = "root is not an object";
    return fix;
  }
  yyjson_val* curves = jsonu::get(root, "Curves");
  if (!yyjson_is_arr(curves)) {
    fix.error = "Curves array missing";
    return fix;
  }

  Counts actual;
  if (!countCurves(curves, actual, fix.error)) return fix;

  yyjson_val* meta = jsonu::get(root, "Meta");
  if (metaInt(meta, "CurveCount") == actual.curves && metaInt(meta, "TotalSegmentCount") == actual.segments && metaInt(meta, "TotalPointCount") == actual.points) {
    fix.result = MotionMetaResult::Ok;
    return fix;
  }

  // 深拷貝後只改 Meta 的三個數字，其餘一個位元組都不動
  jsonu::MutDoc mut;
  yyjson_mut_val* mutRoot = mut.copyOf(root);
  mut.setRoot(mutRoot);
  yyjson_mut_val* mutMeta = yyjson_mut_obj_get(mutRoot, "Meta");
  if (!mutMeta || !yyjson_mut_is_obj(mutMeta)) {
    mutMeta = yyjson_mut_obj(mut.get());
    yyjson_mut_obj_put(mutRoot, yyjson_mut_strcpy(mut.get(), "Meta"), mutMeta);
  }
  yyjson_mut_obj_put(mutMeta, yyjson_mut_strcpy(mut.get(), "CurveCount"), yyjson_mut_sint(mut.get(), actual.curves));
  yyjson_mut_obj_put(mutMeta, yyjson_mut_strcpy(mut.get(), "TotalSegmentCount"), yyjson_mut_sint(mut.get(), actual.segments));
  yyjson_mut_obj_put(mutMeta, yyjson_mut_strcpy(mut.get(), "TotalPointCount"), yyjson_mut_sint(mut.get(), actual.points));

  fix.result = MotionMetaResult::Fixed;
  fix.json = mut.write(true);
  return fix;
}

}  // namespace l2m
