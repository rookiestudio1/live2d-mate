#include "ellipse_text_fit.h"

#include <algorithm>
#include <cmath>

namespace l2m {

namespace {

constexpr double kPi = 3.14159265358979323846;

// 文字塊對稱地騎在橢圓中心上：第 i 行的上緣
std::vector<double> topsFor(double lineHeight, size_t lines) {
  std::vector<double> out;
  out.reserve(lines);
  const double blockTop = -static_cast<double>(lines) * lineHeight / 2;
  for (size_t i = 0; i < lines; ++i) out.push_back(blockTop + static_cast<double>(i) * lineHeight);
  return out;
}

// 給定行數，算出每一行允許的寬度。夾到 minLineWidth 只是為了不讓 measure
// 拿到 0 或負數而永遠斷不完 —— 真的太窄要靠外層把整顆橢圓放大。
std::vector<double> widthsFor(double a, double b, double lineHeight, const std::vector<double>& tops, const EllipseTextTuning& tuning) {
  std::vector<double> out;
  out.reserve(tops.size());
  for (double top : tops) {
    const double half = ellipseHalfWidthAt(a, b, top, top + lineHeight);
    out.push_back(std::max(2 * half - 2 * tuning.paddingX, tuning.minLineWidth));
  }
  return out;
}

}  // namespace

double ellipseHalfWidthAt(double semiMajor, double semiMinor, double yTop, double yBottom) {
  if (semiMajor <= 0 || semiMinor <= 0) return 0;
  // 一行是一條有厚度的帶子，兩個角都要在橢圓裡，所以用離中心較遠的那一邊算
  const double y = std::max(std::abs(yTop), std::abs(yBottom));
  if (y >= semiMinor) return 0;
  const double ratio = y / semiMinor;
  return semiMajor * std::sqrt(1 - ratio * ratio);
}

EllipseTextLayout fitTextInEllipse(double singleLineWidth, double lineHeight, const EllipseTextMeasure& measure, const EllipseTextTuning& tuning) {
  const double height = std::max(lineHeight, 1.0);
  const double single = std::max(singleLineWidth, 0.0);
  const double aspect = std::max(tuning.aspect, 0.1);
  const double growStep = std::max(tuning.growStep, 1.01);
  const double capA = std::max(tuning.maxSemiMajor, 1.0);

  // 種子橢圓：π·a·b ＝ 文字的墨水面積 ÷ seedFill，再套 a ＝ aspect·b。
  // 一定要偏小（見標頭 ③），所以只拿 W₁·L 這個下界去估，不含內距。
  double b = std::sqrt(std::max(single, 1.0) * height / (kPi * aspect * std::max(tuning.seedFill, 0.05)));
  b = std::max(b, std::max(tuning.minSemiMinor, height / 2 + tuning.paddingY));

  EllipseTextLayout out;
  for (int grow = 0;; ++grow) {
    const double a = std::min(aspect * b, capA);

    // 行數收斂：從一行開始。行數變多 → 文字塊變高 → 上下那幾行更窄 → 行數只會
    // 再變多，單調遞增所以不會震盪（見標頭 ②）。
    size_t lines = 1;
    std::vector<double> tops = topsFor(height, lines);
    std::vector<double> widths = widthsFor(a, b, height, tops, tuning);
    std::vector<double> actual = measure ? measure(widths) : std::vector<double>{};
    for (int round = 0; round < tuning.maxRelayout && actual.size() != lines; ++round) {
      lines = std::max<size_t>(1, actual.size());
      tops = topsFor(height, lines);
      widths = widthsFor(a, b, height, tops, tuning);
      actual = measure ? measure(widths) : std::vector<double>{};
    }
    if (actual.empty()) actual.push_back(0);

    const bool converged = actual.size() == lines;
    // 沒收斂（正常情況碰不到）時，交出去的寬度表仍然要跟 measure 真的看到的
    // 那一組一致 —— 多出來的行照契約補最後一個寬度。上緣則照實際行數重算，
    // 至少保住垂直置中。
    while (widths.size() < actual.size()) widths.push_back(widths.empty() ? tuning.minLineWidth : widths.back());
    widths.resize(actual.size());
    tops = topsFor(height, actual.size());

    bool fits = converged && static_cast<double>(actual.size()) * height + tuning.paddingY * 2 <= 2 * b + 1e-9;
    for (size_t i = 0; fits && i < actual.size(); ++i) {
      if (actual[i] > widths[i] + 1e-6) fits = false;
    }

    out.ellipse = {2 * a, 2 * b};
    out.lineWidths = std::move(widths);
    out.lineTops = std::move(tops);
    out.lineHeight = height;
    out.overflow = !fits;

    if (fits || grow >= tuning.maxGrow) return out;
    b *= growStep;
  }
}

}  // namespace l2m
