// 環境風的陣風包絡（core/ambient_wind.h）。
//
// 最重要的斷言是最後那一條「max - min 要有明顯差距」——
// Cubism 的 Wind 是力，恆定的力只會讓頭髮歪向一邊定住；
// 會飄的前提是風力持續起伏，gustDepth 被誤設成 0 時這條會當場紅燈。
#include <QtTest>

#include <algorithm>
#include <cmath>

#include "core/ambient_wind.h"

using namespace l2m;

namespace {

// 兩個週期（2700 / 1100）的最小公倍週期：取樣涵蓋一整輪，峰谷一定出現
constexpr double kFullCycleMs = 29700;

}  // namespace

class TestAmbientWind : public QObject {
  Q_OBJECT

private slots:
  // 關閉或強度為零時完全沒有輸出
  void disabledOrZeroStrengthIsZero() {
    const AmbientWindTuning tuning;
    for (double t : {0.0, 123.0, 4567.0}) {
      const WindVector off = ambientWind(tuning, false, WindDirection::Right, 1.0, t);
      QCOMPARE(off.x, 0.0);
      QCOMPARE(off.y, 0.0);
      const WindVector zero = ambientWind(tuning, true, WindDirection::Right, 0.0, t);
      QCOMPARE(zero.x, 0.0);
      QCOMPARE(zero.y, 0.0);
    }
  }

  // 陣風包絡恆為正：風向不會在週期中途反轉
  void directionNeverFlips() {
    const AmbientWindTuning tuning;
    for (double t = 0; t <= kFullCycleMs; t += 10) {
      QVERIFY(ambientWind(tuning, true, WindDirection::Left, 0.7, t).x < 0);
      QVERIFY(ambientWind(tuning, true, WindDirection::Right, 0.7, t).x > 0);
    }
  }

  // |x| 不超過 maxWind；y 恆為 0（風是水平的，垂直交給重力）
  void magnitudeIsBoundedAndHorizontal() {
    const AmbientWindTuning tuning;
    for (double t = 0; t <= kFullCycleMs; t += 10) {
      const WindVector w = ambientWind(tuning, true, WindDirection::Right, 1.0, t);
      QVERIFY(std::abs(w.x) <= tuning.maxWind + 1e-9);
      QCOMPARE(w.y, 0.0);
    }
  }

  // 純函數：同一個 nowMs 呼叫兩次結果完全相同
  void isPureFunctionOfTime() {
    const AmbientWindTuning tuning;
    const WindVector a = ambientWind(tuning, true, WindDirection::Right, 0.5, 777.0);
    const WindVector b = ambientWind(tuning, true, WindDirection::Right, 0.5, 777.0);
    QCOMPARE(a.x, b.x);
    QCOMPARE(a.y, b.y);
  }

  // strength=1 時包絡峰值 == maxWind、谷值 == maxWind * (1 - gustDepth)。
  // 兩個正弦要同時到頂／到底才碰得到端點：以 1 ms 逐點取樣一整個公倍週期，
  // t=22275 兩波同時 = +1、t=7425 同時 = -1（27k-11m 的整數解），所以端點一定被採到。
  void envelopePeakAndTroughMatchTuning() {
    const AmbientWindTuning tuning;
    double maxX = 0;
    double minX = tuning.maxWind * 2;
    for (double t = 0; t <= kFullCycleMs; t += 1) {
      const double x = ambientWind(tuning, true, WindDirection::Right, 1.0, t).x;
      maxX = std::max(maxX, x);
      minX = std::min(minX, x);
    }
    QVERIFY(std::abs(maxX - tuning.maxWind) < 1e-6);
    QVERIFY(std::abs(minX - tuning.maxWind * (1 - tuning.gustDepth)) < 1e-6);
  }

  // 「頭髮會飄不會定住」的真正斷言：一整個週期內風力要有明顯起伏
  void windActuallyVaries() {
    const AmbientWindTuning tuning;
    double maxX = 0;
    double minX = tuning.maxWind * 2;
    for (double t = 0; t <= kFullCycleMs; t += 10) {
      const double x = ambientWind(tuning, true, WindDirection::Right, 1.0, t).x;
      maxX = std::max(maxX, x);
      minX = std::min(minX, x);
    }
    // 預設 gustDepth 0.35 → 起伏約為峰值的 35%；抓一半當紅線就夠抓出「被設成 0」
    QVERIFY(maxX - minX > tuning.maxWind * tuning.gustDepth * 0.5);
  }
};

QTEST_GUILESS_MAIN(TestAmbientWind)
#include "test_ambient_wind.moc"
