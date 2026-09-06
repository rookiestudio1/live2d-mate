// 把文字排進橢圓：上下短、中間長。
// 會出錯的地方全是只有肉眼才抓得到的 —— 某一行的角戳出橢圓、文字塊沒有垂直置中、
// 交出去的寬度表與實際斷出來的行數對不上（＝量出來的跟畫出來的不是同一件事）、
// 塞不下時橢圓沒有跟著長大。所以這裡把「重放同一組寬度會得到同一組行」當成核心契約。
#include <QtTest>

#include <algorithm>
#include <cmath>
#include <vector>

#include "core/ellipse_text_fit.h"

using namespace l2m;

namespace {

constexpr double kSqrt2 = 1.41421356237309504880;

bool near(double actual, double expected, double tolerance = 1e-9) { return std::abs(actual - expected) <= tolerance; }

// 契約：斷出來的行數超過 widths 時，多出來的行一律用最後一個寬度
double allowedWidth(const std::vector<double>& widths, size_t index) {
  if (widths.empty()) return 0;
  return index < widths.size() ? widths[index] : widths.back();
}

// 假字型：等寬、任意位置可斷（中日文的行為）。回傳每一行實際佔掉的寬度。
EllipseTextMeasure monospace(int chars, double charWidth = 10) {
  return [chars, charWidth](const std::vector<double>& widths) {
    std::vector<double> lines;
    int remaining = chars;
    while (remaining > 0) {
      // 一行至少吃一個字，否則寬度被夾到 0 時會無窮迴圈
      int take = std::max(1, static_cast<int>(std::floor(allowedWidth(widths, lines.size()) / charWidth)));
      take = std::min(take, remaining);
      lines.push_back(take * charWidth);
      remaining -= take;
    }
    if (lines.empty()) lines.push_back(0);
    return lines;
  };
}

// 一段永遠斷不開的長 token（網址、檔案路徑）：不管給多窄，都是這麼寬的一行
EllipseTextMeasure unbreakable(double width) {
  return [width](const std::vector<double>&) { return std::vector<double>{width}; };
}

// 點（相對橢圓中心）在橢圓裡嗎
bool insideEllipse(double x, double y, const BubbleSize& ellipse, double slack = 1e-6) {
  const double a = ellipse.width / 2;
  const double b = ellipse.height / 2;
  return (x * x) / (a * a) + (y * y) / (b * b) <= 1 + slack;
}

}  // namespace

class TestEllipseTextFit : public QObject {
  Q_OBJECT

private slots:
  // ── 半寬公式 ──

  // 橢圓最寬的地方在中間，越往上下越窄 —— 「上下短中間長」就是這條
  void halfWidthIsWidestAcrossTheCentre() {
    QVERIFY(ellipseHalfWidthAt(100, 50, -1, 1) > ellipseHalfWidthAt(100, 50, 24, 26));
    QVERIFY(ellipseHalfWidthAt(100, 50, 24, 26) > ellipseHalfWidthAt(100, 50, 44, 46));
  }

  // 一行字是一條有厚度的帶子，兩個角都要在橢圓裡，所以取離中心較遠的那一邊
  void halfWidthTakesTheEdgeFartherFromTheCentre() {
    QVERIFY(near(ellipseHalfWidthAt(100, 50, 10, 30), 100 * std::sqrt(1 - 0.36)));
    QVERIFY(near(ellipseHalfWidthAt(100, 50, 10, 30), ellipseHalfWidthAt(100, 50, -30, -10)));
  }

  // 整條帶子都在橢圓外時回 0，不是負數也不是 NaN
  void halfWidthVanishesBeyondThePole() {
    QCOMPARE(ellipseHalfWidthAt(100, 50, 50, 60), 0.0);
    QCOMPARE(ellipseHalfWidthAt(100, 50, -80, -60), 0.0);
  }

  // ── 排版 ──

  // 這次改動的重點：中間那幾行比頭尾長，而且上下對稱
  void middleLinesAreWiderThanTopAndBottom() {
    const EllipseTextLayout fit = fitTextInEllipse(900, 17, monospace(90));
    QVERIFY(fit.lineWidths.size() >= 3);
    const size_t middle = fit.lineWidths.size() / 2;
    QVERIFY(fit.lineWidths.front() < fit.lineWidths[middle]);
    QVERIFY(fit.lineWidths.back() < fit.lineWidths[middle]);
    QVERIFY(near(fit.lineWidths.front(), fit.lineWidths.back()));
  }

  // 每一行的四個角（含內距）都要落在橢圓裡，否則字會壓在黑框上
  void everyLineCornerStaysInsideTheEllipse() {
    const EllipseTextTuning tuning;
    const EllipseTextLayout fit = fitTextInEllipse(900, 17, monospace(90), tuning);
    QVERIFY(!fit.lineWidths.empty());
    QCOMPARE(fit.lineTops.size(), fit.lineWidths.size());
    for (size_t i = 0; i < fit.lineWidths.size(); ++i) {
      const double halfBox = fit.lineWidths[i] / 2 + tuning.paddingX;
      const double top = fit.lineTops[i];
      const double bottom = top + fit.lineHeight;
      QVERIFY(insideEllipse(halfBox, top, fit.ellipse));
      QVERIFY(insideEllipse(halfBox, bottom, fit.ellipse));
      QVERIFY(insideEllipse(-halfBox, top, fit.ellipse));
      QVERIFY(insideEllipse(-halfBox, bottom, fit.ellipse));
    }
  }

  // 文字塊要對稱地騎在橢圓中心上，不然整段字會偏上或偏下
  void theTextBlockIsCentredOnTheEllipse() {
    const EllipseTextLayout fit = fitTextInEllipse(900, 17, monospace(90));
    const double top = fit.lineTops.front();
    const double bottom = fit.lineTops.back() + fit.lineHeight;
    QVERIFY(near(top, -bottom));
  }

  // 核心契約：把交出去的寬度表原封不動餵回去，要得到一模一樣的行數與行寬。
  // 呼叫端就是照這組寬度重畫的，對不上就是「量的跟畫的不同」。
  void replayingTheWidthsReproducesTheSameLines() {
    const EllipseTextMeasure measure = monospace(90);
    const EllipseTextLayout fit = fitTextInEllipse(900, 17, measure);
    const std::vector<double> replay = measure(fit.lineWidths);
    QCOMPARE(replay.size(), fit.lineWidths.size());
    for (size_t i = 0; i < replay.size(); ++i) QVERIFY(replay[i] <= fit.lineWidths[i] + 1e-6);
  }

  // 行數乘行高（加上下內距）要塞得進橢圓的高度
  void theWholeTextBlockFitsInsideTheEllipseHeight() {
    const EllipseTextTuning tuning;
    const EllipseTextLayout fit = fitTextInEllipse(900, 17, monospace(90), tuning);
    const double block = static_cast<double>(fit.lineWidths.size()) * fit.lineHeight;
    QVERIFY(block + tuning.paddingY * 2 <= fit.ellipse.height + 1e-6);
    QVERIFY(!fit.overflow);
  }

  // 短句就該是一行 —— 上下短中間長不能讓一句「你好」被拆成兩行
  void aShortLineStaysOnOneLine() {
    const EllipseTextLayout fit = fitTextInEllipse(50, 17, monospace(5));
    QCOMPARE(fit.lineWidths.size(), size_t(1));
  }

  void longerTextGivesABiggerEllipse() {
    const EllipseTextLayout small = fitTextInEllipse(200, 17, monospace(20));
    const EllipseTextLayout big = fitTextInEllipse(2000, 17, monospace(200));
    QVERIFY(big.ellipse.width > small.ellipse.width);
    QVERIFY(big.ellipse.height > small.ellipse.height);
  }

  // 長寬比是設定給的，放大時整顆等比長大
  void theEllipseKeepsTheTargetAspect() {
    EllipseTextTuning tuning;
    tuning.aspect = 1.5;
    const EllipseTextLayout fit = fitTextInEllipse(900, 17, monospace(90), tuning);
    QVERIFY(near(fit.ellipse.width / fit.ellipse.height, 1.5));
  }

  // 做這件事的理由：老規則是「矩形外接橢圓」，四個角整整浪費 36% 的面積
  void shapedTextNeedsLessAreaThanTheCircumscribedRectangle() {
    const EllipseTextTuning tuning;
    const EllipseTextLayout fit = fitTextInEllipse(900, 17, monospace(90), tuning);

    // 老規則：換行寬度 √(2·W₁·L)、行數 ⌈W₁/w⌉、橢圓半徑各乘 √2（core/bubble_shape.h ①②）
    const double wrap = std::sqrt(2 * 900 * 17);
    const double lines = std::ceil(900 / wrap);
    const double oldWidth = (wrap + tuning.paddingX * 2) * kSqrt2;
    const double oldHeight = (lines * 17 + tuning.paddingY * 2) * kSqrt2;
    QVERIFY(fit.ellipse.width * fit.ellipse.height < oldWidth * oldHeight);
  }

  // 斷不開的長 token：橢圓要長到放得下，而不是讓它被裁掉
  void anUnbreakableTokenWidensTheEllipse() {
    const EllipseTextTuning tuning;
    const EllipseTextLayout fit = fitTextInEllipse(400, 17, unbreakable(400), tuning);
    QVERIFY(!fit.overflow);
    QVERIFY(fit.lineWidths.front() >= 400);
    QVERIFY(fit.ellipse.width >= 400 + tuning.paddingX * 2);
  }

  // 長到上限還是塞不下就要照實回報，呼叫端才知道要用不裁切的方式畫
  void overflowIsReportedWhenGrowthRunsOut() {
    EllipseTextTuning tuning;
    tuning.maxGrow = 0;
    const EllipseTextLayout fit = fitTextInEllipse(400, 17, unbreakable(400), tuning);
    QVERIFY(fit.overflow);
    QVERIFY(!fit.lineWidths.empty());
  }

  // 橢圓寬度有上限：超長的一段話該往下長，不是橫著長出螢幕
  void veryLongTextGrowsTallerInsteadOfWiderThanTheCap() {
    EllipseTextTuning tuning;
    tuning.maxSemiMajor = 120;
    const EllipseTextLayout fit = fitTextInEllipse(6000, 17, monospace(600), tuning);
    QVERIFY(fit.ellipse.width <= 240 + 1e-6);
    QVERIFY(fit.ellipse.height > fit.ellipse.width);
  }

  // 退化輸入不能吐出 NaN 或零尺寸：那會讓氣泡靜靜地整顆消失
  void degenerateInputStaysFinite() {
    const EllipseTextLayout fit = fitTextInEllipse(0, 0, monospace(0));
    QVERIFY(std::isfinite(fit.ellipse.width) && fit.ellipse.width > 0);
    QVERIFY(std::isfinite(fit.ellipse.height) && fit.ellipse.height > 0);
    QVERIFY(fit.lineHeight > 0);
    QVERIFY(!fit.lineWidths.empty());
    for (double width : fit.lineWidths) QVERIFY(std::isfinite(width) && width >= 0);
    for (double top : fit.lineTops) QVERIFY(std::isfinite(top));
  }
};

QTEST_APPLESS_MAIN(TestEllipseTextFit)
#include "test_ellipse_text_fit.moc"
