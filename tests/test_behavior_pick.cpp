// 挑選規則：加權隨機 ＋ 冷卻 ＋ 不重複最近 N 個。
// 時間與亂數都由測試注入，冷卻不必靠肉眼盯 5 分鐘。
#include <QtTest>

#include "core/behavior_pick.h"

using namespace l2m;

class TestBehaviorPick : public QObject {
  Q_OBJECT

private slots:
  // 權重 2:1、unit=0.6 落在第一個（0.6 × 3 = 1.8 < 2）
  void weightBiasesSelection() {
    BehaviorPicker picker;
    const std::vector<PickCandidate> candidates{{"a", 2}, {"b", 1}};
    QCOMPARE(picker.pick(candidates, 0, 0.6), std::optional<std::string>("a"));
    // 0.7 × 3 = 2.1 > 2 → 落到第二個
    picker.reset();
    QCOMPARE(picker.pick(candidates, 0, 0.7), std::optional<std::string>("b"));
  }

  // 冷卻內不再被挑：a 剛用過，unit 指著 a 也會挑到 b
  void cooldownExcludesRecentPick() {
    BehaviorPicker picker({/*cooldownMs=*/60000, /*noRepeatLast=*/0});
    const std::vector<PickCandidate> candidates{{"a", 1}, {"b", 1}};
    QCOMPARE(picker.pick(candidates, 0, 0.0), std::optional<std::string>("a"));
    QCOMPARE(picker.pick(candidates, 1000, 0.0), std::optional<std::string>("b"));
    // 冷卻過了就回得來
    QCOMPARE(picker.pick(candidates, 70000, 0.0), std::optional<std::string>("a"));
  }

  // 全部都在冷卻時仍回一個（冷卻剩餘最短的那一個）而非 nullopt
  void allCoolingStillReturnsSomething() {
    BehaviorPicker picker({/*cooldownMs=*/60000, /*noRepeatLast=*/0});
    const std::vector<PickCandidate> candidates{{"a", 1}, {"b", 1}};
    picker.pick(candidates, 0, 0.0);     // a
    picker.pick(candidates, 1000, 0.0);  // b
    // 兩個都在冷卻；a 用得比較早，剩餘冷卻最短
    QCOMPARE(picker.pick(candidates, 2000, 0.0), std::optional<std::string>("a"));
  }

  // noRepeatLast=2：連挑三次不重複
  void noRepeatAvoidsRecentTwo() {
    BehaviorPicker picker({/*cooldownMs=*/0, /*noRepeatLast=*/2});
    const std::vector<PickCandidate> candidates{{"a", 1}, {"b", 1}, {"c", 1}};
    const auto first = picker.pick(candidates, 0, 0.0);
    const auto second = picker.pick(candidates, 1, 0.0);
    const auto third = picker.pick(candidates, 2, 0.0);
    QVERIFY(first != second);
    QVERIFY(second != third);
    QVERIFY(first != third);
  }

  // 只有一個候選時永遠回它 —— 不重複規則不該讓它整段安靜下來
  void singleCandidateAlwaysReturned() {
    BehaviorPicker picker({/*cooldownMs=*/0, /*noRepeatLast=*/2});
    const std::vector<PickCandidate> candidates{{"only", 1}};
    for (int i = 0; i < 5; ++i) {
      QCOMPARE(picker.pick(candidates, i, 0.5), std::optional<std::string>("only"));
    }
  }

  // weight <= 0 被排除；全部被排除時才回 nullopt
  void nonPositiveWeightExcluded() {
    BehaviorPicker picker;
    const std::vector<PickCandidate> candidates{{"dead", 0}, {"alive", 1}};
    QCOMPARE(picker.pick(candidates, 0, 0.99), std::optional<std::string>("alive"));
    const std::vector<PickCandidate> allDead{{"x", 0}, {"y", -1}};
    QCOMPARE(picker.pick(allDead, 0, 0.5), std::optional<std::string>());
  }

  // 挑中即記錄；forget 抹掉紀錄後冷卻不再擋它
  void lastUsedIsRecordedAndForgettable() {
    BehaviorPicker picker({/*cooldownMs=*/60000, /*noRepeatLast=*/0});
    const std::vector<PickCandidate> candidates{{"a", 1}, {"b", 1}};
    picker.pick(candidates, 1234, 0.0);
    QCOMPARE(picker.lastUsedMs("a"), std::optional<double>(1234));
    picker.forget("a");
    QCOMPARE(picker.lastUsedMs("a"), std::optional<double>());
    QCOMPARE(picker.pick(candidates, 1235, 0.0), std::optional<std::string>("a"));
  }
};

QTEST_GUILESS_MAIN(TestBehaviorPick)
#include "test_behavior_pick.moc"
