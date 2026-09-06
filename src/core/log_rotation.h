#pragma once

// log 檔的命名、輪替序號與保留天數 —— 全部是純字串與整數算術，不碰檔案系統。
//
// 放在 l2m_core 而不是 app/logging.cpp：那支要 include spdlog、要開檔案，
// 只連 l2m_core 的 QTest 摸不到它。而「哪些檔案該刪」正是最不能猜錯的一段 ——
// 少刪只是佔空間，多刪就是把使用者昨天的當機現場砍掉了。
//
// 檔名格式：live2d_mate-2026-08-26.log      （序號 0）
//           live2d_mate-2026-08-26.3.log    （序號 3，同一天寫滿上限後往上加）
//
// 序號刻意**只增不減**，不做 spdlog rotating_file_sink 那種 .1→.2 的整批改名：
// 那在多行程下會互相搶同一批檔名，而且跟日期併用時「.1 是比較新還是比較舊」
// 根本講不清楚。只增不減的話，同一天內序號越大就是越新，一眼就懂。

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace l2m {

// 日誌檔名裡的日期。刻意不用 QDate／std::chrono::year_month_day：
// 前者把 l2m_core 綁到 Qt 的日期型別，後者要 C++20，而這裡只需要三個整數。
struct LogDate {
  int year = 0;
  int month = 0;
  int day = 0;

  friend bool operator==(const LogDate& a, const LogDate& b) { return a.year == b.year && a.month == b.month && a.day == b.day; }
  friend bool operator!=(const LogDate& a, const LogDate& b) { return !(a == b); }
};

// 檔名前綴。與執行檔同名，使用者在 logs/ 裡一眼認得出是誰寫的
inline constexpr const char* kLogFileStem = "live2d_mate";

// 副檔名前面那一段的組裝。index 0 不加序號
std::string logFileName(const LogDate& date, int index);

// 解析檔名，回 {日期, 序號}。認不得就回 nullopt ——
// logs/ 裡的外來檔案（使用者自己丟的、別的工具寫的）一律不碰
std::optional<std::pair<LogDate, int>> parseLogFileName(std::string_view name);

// names 之中屬於 date 的最大序號；那天一個檔都沒有時回 -1
int latestLogIndex(const std::vector<std::string>& names, const LogDate& date);

// from 到 to 相差幾天（to 比較晚就是正的）。
// 用 Howard Hinnant 的 days_from_civil：純整數算術，不經過 tm／mktime，
// 所以不受時區與 DST 影響，也就不會在跨月／跨年／閏年上出錯
int daysBetween(const LogDate& from, const LogDate& to);

// 該刪掉的檔名。keepDays = 3 就是「留今天與前兩天」，
// 也就是 daysBetween(檔案日期, today) >= 3 的通通刪。
// 解析不出來的檔名不會出現在結果裡（見 parseLogFileName）
std::vector<std::string> expiredLogFiles(const std::vector<std::string>& names, const LogDate& today, int keepDays);

}  // namespace l2m
