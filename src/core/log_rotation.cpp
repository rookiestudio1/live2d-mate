#include "log_rotation.h"

#include <algorithm>

namespace l2m {
namespace {

void appendPadded(std::string& out, int value, size_t width) {
  const std::string digits = std::to_string(value);
  if (digits.size() < width) out.append(width - digits.size(), '0');
  out += digits;
}

bool allDigits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

int toInt(std::string_view text) {
  int value = 0;
  for (const char c : text) value = value * 10 + (c - '0');
  return value;
}

// Howard Hinnant, "chrono-Compatible Low-Level Date Algorithms" 的 days_from_civil。
// 回傳自 1970-01-01 起算的天數（可為負）。純整數算術，不經過 tm／mktime，
// 所以不受時區與 DST 影響 —— 這正是「跨月／跨年／閏年不會算錯」的來源。
long long daysFromCivil(int y, int m, int d) {
  y -= m <= 2;
  const long long era = (y >= 0 ? y : y - 399) / 400;
  const long long yoe = y - era * 400;                                   // [0, 399]
  const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;  // [0, 365]
  const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;           // [0, 146096]
  return era * 146097 + doe - 719468;
}

}  // namespace

std::string logFileName(const LogDate& date, int index) {
  std::string name = kLogFileStem;
  name += '-';
  appendPadded(name, date.year, 4);
  name += '-';
  appendPadded(name, date.month, 2);
  name += '-';
  appendPadded(name, date.day, 2);
  if (index > 0) {
    name += '.';
    name += std::to_string(index);
  }
  name += ".log";
  return name;
}

std::optional<std::pair<LogDate, int>> parseLogFileName(std::string_view name) {
  const std::string prefix = std::string(kLogFileStem) + '-';
  constexpr std::string_view kSuffix = ".log";
  if (name.size() <= prefix.size() + kSuffix.size()) return std::nullopt;
  if (name.substr(0, prefix.size()) != prefix) return std::nullopt;
  if (name.substr(name.size() - kSuffix.size()) != kSuffix) return std::nullopt;

  std::string_view middle = name.substr(prefix.size(), name.size() - prefix.size() - kSuffix.size());

  int index = 0;
  if (const size_t dot = middle.find('.'); dot != std::string_view::npos) {
    const std::string_view digits = middle.substr(dot + 1);
    // 序號位數設上限，免得 "live2d_mate-2026-08-26.999999999999.log" 這種
    // 惡意或亂寫的檔名把 toInt 撐爆成未定義行為
    if (!allDigits(digits) || digits.size() > 6) return std::nullopt;
    index = toInt(digits);
    if (index <= 0) return std::nullopt;  // 序號 0 不寫點，".0.log" 不是我們產的
    middle = middle.substr(0, dot);
  }

  if (middle.size() != 10 || middle[4] != '-' || middle[7] != '-') return std::nullopt;
  const std::string_view year = middle.substr(0, 4);
  const std::string_view month = middle.substr(5, 2);
  const std::string_view day = middle.substr(8, 2);
  if (!allDigits(year) || !allDigits(month) || !allDigits(day)) return std::nullopt;

  const LogDate date{toInt(year), toInt(month), toInt(day)};
  if (date.month < 1 || date.month > 12 || date.day < 1 || date.day > 31) return std::nullopt;
  return std::make_pair(date, index);
}

int latestLogIndex(const std::vector<std::string>& names, const LogDate& date) {
  int latest = -1;
  for (const std::string& name : names) {
    const auto parsed = parseLogFileName(name);
    if (!parsed || parsed->first != date) continue;
    latest = std::max(latest, parsed->second);
  }
  return latest;
}

int daysBetween(const LogDate& from, const LogDate& to) { return static_cast<int>(daysFromCivil(to.year, to.month, to.day) - daysFromCivil(from.year, from.month, from.day)); }

std::vector<std::string> expiredLogFiles(const std::vector<std::string>& names, const LogDate& today, int keepDays) {
  std::vector<std::string> expired;
  if (keepDays < 1) return expired;
  for (const std::string& name : names) {
    const auto parsed = parseLogFileName(name);
    if (!parsed) continue;  // 外來檔案不碰
    // 未來日期（使用者改過系統時間、或檔案從別台複製過來）一律留著：
    // daysBetween 會是負數，本來就進不了這個條件
    if (daysBetween(parsed->first, today) >= keepDays) expired.push_back(name);
  }
  return expired;
}

}  // namespace l2m
