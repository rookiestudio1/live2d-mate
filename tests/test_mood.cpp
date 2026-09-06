// 心情（只往正向偏、幾分鐘退回）與熟悉度級距（只增不減）。
#include <QtTest>

#include "core/mood.h"

using namespace l2m;

class TestMood : public QObject {
  Q_OBJECT

private slots:
  // nudge 之後開心一陣子，decayMs 過了自動退回
  void cheerfulnessDecays() {
    MoodState mood({/*decayMs=*/300000});
    QCOMPARE(mood.cheerful(0), false);
    mood.nudge(1000);
    QCOMPARE(mood.cheerful(1000), true);
    QCOMPARE(mood.cheerful(300000), true);
    QCOMPARE(mood.cheerful(301001), false);
    // 再摸一次又開心起來 —— 沒有任何「負面」路徑
    mood.nudge(400000);
    QCOMPARE(mood.cheerful(400001), true);
  }

  // 級距單調遞增，門檻 10 / 50 / 200
  void familiarityLevelsAreMonotonic() {
    QCOMPARE(familiarityLevel(0), 0);
    QCOMPARE(familiarityLevel(9), 0);
    QCOMPARE(familiarityLevel(10), 1);
    QCOMPARE(familiarityLevel(49), 1);
    QCOMPARE(familiarityLevel(50), 2);
    QCOMPARE(familiarityLevel(199), 2);
    QCOMPARE(familiarityLevel(200), 3);
    QCOMPARE(familiarityLevel(1000000), 3);
  }
};

QTEST_GUILESS_MAIN(TestMood)
#include "test_mood.moc"
