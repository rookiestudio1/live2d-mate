// 時段語意與 # Greeting by Time 的前綴解析。
#include <QtTest>

#include "core/day_period.h"

using namespace l2m;

class TestDayPeriod : public QObject {
  Q_OBJECT

private slots:
  void hoursMapToPeriods() {
    QCOMPARE(dayPeriodFor(5), DayPeriod::Morning);
    QCOMPARE(dayPeriodFor(10), DayPeriod::Morning);
    QCOMPARE(dayPeriodFor(11), DayPeriod::Afternoon);
    QCOMPARE(dayPeriodFor(17), DayPeriod::Afternoon);
    QCOMPARE(dayPeriodFor(18), DayPeriod::Evening);
    QCOMPARE(dayPeriodFor(22), DayPeriod::Evening);
    QCOMPARE(dayPeriodFor(23), DayPeriod::Night);
    QCOMPARE(dayPeriodFor(0), DayPeriod::Night);
    QCOMPARE(dayPeriodFor(4), DayPeriod::Night);
    // 界外值收斂而不是炸掉
    QCOMPARE(dayPeriodFor(24), DayPeriod::Night);
    QCOMPARE(dayPeriodFor(-1), DayPeriod::Night);
  }

  // 帶前綴的行只在對應時段出現（前綴被剝掉），無前綴的行全時段有效
  void prefixedLinesFilterByPeriod() {
    const std::vector<std::string> lines{
      "morning: 早安，老闆",
      "night: 該睡了",
      "隨時都能講",
    };
    const auto morning = greetingsForPeriod(lines, DayPeriod::Morning);
    QCOMPARE(morning, (std::vector<std::string>{"早安，老闆", "隨時都能講"}));
    const auto afternoon = greetingsForPeriod(lines, DayPeriod::Afternoon);
    QCOMPARE(afternoon, (std::vector<std::string>{"隨時都能講"}));
  }

  // 前綴大小寫不敏感；全形冒號也認
  void prefixIsForgivingAboutCaseAndColon() {
    const std::vector<std::string> lines{"Morning: hi", "EVENING\xEF\xBC\x9A晚上好"};
    QCOMPARE(greetingsForPeriod(lines, DayPeriod::Morning), (std::vector<std::string>{"hi"}));
    QCOMPARE(greetingsForPeriod(lines, DayPeriod::Evening), (std::vector<std::string>{"晚上好"}));
  }

  // 前綴打錯字的行視為無前綴（全時段有效）—— 靜默丟掉整行更難查
  void typoedPrefixFallsBackToAllPeriods() {
    const std::vector<std::string> lines{"mornning: 打錯了"};
    QCOMPARE(greetingsForPeriod(lines, DayPeriod::Night), (std::vector<std::string>{"mornning: 打錯了"}));
  }
};

QTEST_GUILESS_MAIN(TestDayPeriod)
#include "test_day_period.moc"
