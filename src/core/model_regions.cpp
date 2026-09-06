#include "model_regions.h"

#include <algorithm>

namespace l2m {

namespace {

// 頭部比例的上下限。下限 0.16 是給極端細長的全身立繪（含大片裙擺／尾巴），
// 上限 0.5 是胸像 —— 再高就沒有身體可以分了。
constexpr double kHeadFractionMin = 0.16;
constexpr double kHeadFractionMax = 0.5;
// 分子：aspect 1.1（胸像）剛好落在上限、2.5（全身）落在 0.22
constexpr double kHeadFractionNumerator = 0.55;

// 頭以下剩餘高度的切法（胸 / 身 / 腿）。
// **胸口那一段刻意窄**：它是 touch_special 的入口（碧藍航線的「特殊觸摸」多半是
// 結婚後才解鎖的那一段），區間開太大的結果是整個上半身戳下去都播 special，
// touch_body 幾乎叫不出來。0.18 對全身立繪換算回來大約是全高的 22%～36%，
// 也就是胸線那一帶；腰腹以下歸 Body。
constexpr double kChestShare = 0.18;
constexpr double kBodyShare = 0.27;

}  // namespace

double headFractionFor(double aspect) {
  if (aspect <= 0) return kHeadFractionMax;
  return std::clamp(kHeadFractionNumerator / aspect, kHeadFractionMin, kHeadFractionMax);
}

BodyPart regionAt(const ModelBox& box, double viewX, double viewY) {
  (void)viewX;  // 橫向暫時不分（手／頭髮的左右差異對動作挑選沒有意義）
  if (!box.valid()) return BodyPart::Unknown;

  // y 軸向上：離頂端多遠 → 0（頭頂）..1（腳底）
  const double rel = std::clamp((box.top - viewY) / box.height(), 0.0, 1.0);

  const double head = headFractionFor(box.height() / box.width());
  const double rest = 1.0 - head;
  if (rel < head) return BodyPart::Head;
  if (rel < head + rest * kChestShare) return BodyPart::Chest;
  if (rel < head + rest * (kChestShare + kBodyShare)) return BodyPart::Body;
  return BodyPart::Leg;
}

}  // namespace l2m
