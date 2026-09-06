// log 檔名的組裝／解析、序號推進與保留天數。
//
// 這一支釘住的是「哪些檔案該刪」：少刪只是佔空間，多刪就是把使用者昨天的
// 當機現場砍掉了，所以邊界（剛好第 3 天、未來日期、外來檔名）全部要有斷言。
#include <QtTest>

#include <string>
#include <vector>

#include "core/log_rotation.h"

using namespace l2m;

class TestLogRotation : public QObject {
  Q_OBJECT

private slots:
  // 序號 0 不寫點，其餘寫在副檔名前面
  void nameOmitsZeroIndex() {
    QCOMPARE(logFileName({2026, 8, 26}, 0), std::string("live2d_mate-2026-08-26.log"));
    QCOMPARE(logFileName({2026, 8, 26}, 3), std::string("live2d_mate-2026-08-26.3.log"));
  }

  // 月與日補零，年補到四位
  void namePadsFields() {
    QCOMPARE(logFileName({2026, 1, 2}, 0), std::string("live2d_mate-2026-01-02.log"));
    QCOMPARE(logFileName({999, 12, 31}, 0), std::string("live2d_mate-0999-12-31.log"));
  }

  // 組裝與解析要能往返
  void parseRoundTrips() {
    for (const int index : {0, 1, 7, 42}) {
      const LogDate date{2028, 2, 29};  // 閏日
      const auto parsed = parseLogFileName(logFileName(date, index));
      QVERIFY(parsed.has_value());
      QVERIFY(parsed->first == date);
      QCOMPARE(parsed->second, index);
    }
  }

  // logs/ 裡的外來檔案一律認不得，才不會被當成過期檔刪掉
  void parseRejectsForeignNames() {
    const char* rejected[] = {
      "readme.txt",
      "live2d_mate.log",                     // 沒有日期
      "live2d_mate-2026-08-26.txt",          // 副檔名不對
      "other_app-2026-08-26.log",            // 前綴不對
      "live2d_mate-2026-8-26.log",           // 月份沒補零
      "live2d_mate-2026-13-01.log",          // 月份超範圍
      "live2d_mate-2026-08-32.log",          // 日超範圍
      "live2d_mate-2026-08-00.log",          // 日為 0
      "live2d_mate-20260826.log",            // 少了分隔線
      "live2d_mate-2026-08-26.0.log",        // 序號 0 不該寫點
      "live2d_mate-2026-08-26.x.log",        // 序號不是數字
      "live2d_mate-2026-08-26..log",         // 空序號
      "live2d_mate-2026-08-26.1234567.log",  // 序號位數超出上限
      "live2d_mate-.log",
      ".log",
      "",
    };
    for (const char* name : rejected) {
      QVERIFY2(!parseLogFileName(name).has_value(), name);
    }
  }

  // 那一天一個檔都沒有時回 -1，有的話回最大序號
  void latestIndexPicksMax() {
    const std::vector<std::string> names = {
      "live2d_mate-2026-08-26.log",    // 0
      "live2d_mate-2026-08-26.2.log",  // 2
      "live2d_mate-2026-08-26.1.log",  // 1
      "live2d_mate-2026-08-25.9.log",  // 別天，不算
      "readme.txt",                    // 外來，不算
    };
    QCOMPARE(latestLogIndex(names, {2026, 8, 26}), 2);
    QCOMPARE(latestLogIndex(names, {2026, 8, 25}), 9);
    QCOMPARE(latestLogIndex(names, {2026, 8, 24}), -1);
    QCOMPARE(latestLogIndex({}, {2026, 8, 26}), -1);
  }

  void daysBetweenSameDayIsZero() { QCOMPARE(daysBetween({2026, 8, 26}, {2026, 8, 26}), 0); }

  // 跨月、跨年、以及反向（負數）
  void daysBetweenCrossesBoundaries() {
    QCOMPARE(daysBetween({2026, 8, 31}, {2026, 9, 1}), 1);
    QCOMPARE(daysBetween({2026, 12, 31}, {2027, 1, 1}), 1);
    QCOMPARE(daysBetween({2026, 1, 1}, {2027, 1, 1}), 365);
    QCOMPARE(daysBetween({2026, 9, 1}, {2026, 8, 31}), -1);
  }

  // 閏年：2028 是閏年（能被 4 整除），2100 不是（能被 100 整除但不能被 400）
  void daysBetweenHandlesLeapYears() {
    QCOMPARE(daysBetween({2028, 2, 28}, {2028, 3, 1}), 2);  // 中間有 2/29
    QCOMPARE(daysBetween({2027, 2, 28}, {2027, 3, 1}), 1);  // 平年
    QCOMPARE(daysBetween({2028, 1, 1}, {2029, 1, 1}), 366);
    QCOMPARE(daysBetween({2100, 2, 28}, {2100, 3, 1}), 1);  // 2100 不是閏年
    QCOMPARE(daysBetween({2000, 2, 28}, {2000, 3, 1}), 2);  // 2000 是閏年
  }

  // keepDays = 3 就是「留今天與前兩天」，第 3 天整整齊齊被刪
  void expiredKeepsExactlyThreeDays() {
    const std::vector<std::string> names = {
      "live2d_mate-2026-08-26.log",  // 今天
      "live2d_mate-2026-08-25.log",  // 昨天
      "live2d_mate-2026-08-24.log",  // 前天
      "live2d_mate-2026-08-23.log",  // 第 3 天，要刪
      "live2d_mate-2026-08-01.log",  // 更早，要刪
    };
    const auto expired = expiredLogFiles(names, {2026, 8, 26}, 3);
    QCOMPARE(expired.size(), size_t(2));
    QCOMPARE(expired[0], std::string("live2d_mate-2026-08-23.log"));
    QCOMPARE(expired[1], std::string("live2d_mate-2026-08-01.log"));
  }

  // 同一天的所有序號一起處理，不會只刪掉其中一個
  void expiredCoversEveryIndexOfADay() {
    const std::vector<std::string> names = {
      "live2d_mate-2026-08-20.log",
      "live2d_mate-2026-08-20.1.log",
      "live2d_mate-2026-08-20.2.log",
    };
    QCOMPARE(expiredLogFiles(names, {2026, 8, 26}, 3).size(), size_t(3));
  }

  // 外來檔案永遠不進刪除清單
  void expiredIgnoresForeignFiles() {
    const std::vector<std::string> names = {"readme.txt", "crash-2020-01-01.dmp", "live2d_mate-2020-01-01.log"};
    const auto expired = expiredLogFiles(names, {2026, 8, 26}, 3);
    QCOMPARE(expired.size(), size_t(1));
    QCOMPARE(expired[0], std::string("live2d_mate-2020-01-01.log"));
  }

  // 未來日期（使用者調過系統時間、或檔案從別台複製來）一律留著
  void expiredKeepsFutureDates() {
    const std::vector<std::string> names = {"live2d_mate-2027-01-01.log"};
    QVERIFY(expiredLogFiles(names, {2026, 8, 26}, 3).empty());
  }

  // keepDays 小於 1 是設定錯誤，寧可一個都不刪也不要把整個目錄清空
  void expiredRefusesNonPositiveKeepDays() {
    const std::vector<std::string> names = {"live2d_mate-2020-01-01.log"};
    QVERIFY(expiredLogFiles(names, {2026, 8, 26}, 0).empty());
    QVERIFY(expiredLogFiles(names, {2026, 8, 26}, -1).empty());
  }
};

QTEST_APPLESS_MAIN(TestLogRotation)
#include "test_log_rotation.moc"
