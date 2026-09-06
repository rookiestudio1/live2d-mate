#include "day_period.h"

#include "string_util.h"

namespace l2m {

namespace {

const char* kPrefixes[] = {"morning", "afternoon", "evening", "night"};

// 行首是不是某個時段前綴（後面跟半形或全形冒號）；是的話回冒號後的內容
bool splitPrefixed(const std::string& line, std::string* prefix, std::string* rest) {
  for (const char* candidate : kPrefixes) {
    const size_t length = std::string(candidate).size();
    if (line.size() <= length) continue;
    if (!strutil::equalsInsensitive(line.substr(0, length), candidate)) continue;
    std::string tail = line.substr(length);
    // 半形 ":" 或全形 "："（UTF-8 三位元組）
    if (!tail.empty() && tail[0] == ':') {
      tail.erase(0, 1);
    } else if (tail.rfind("\xEF\xBC\x9A", 0) == 0) {
      tail.erase(0, 3);
    } else {
      continue;
    }
    *prefix = candidate;
    *rest = strutil::trim(tail);
    return true;
  }
  return false;
}

}  // namespace

DayPeriod dayPeriodFor(int hour) {
  // 界外值收斂到 0~23（呼叫端給的是 QTime 的小時，理論上不會界外）
  hour = ((hour % 24) + 24) % 24;
  if (hour >= 5 && hour <= 10) return DayPeriod::Morning;
  if (hour >= 11 && hour <= 17) return DayPeriod::Afternoon;
  if (hour >= 18 && hour <= 22) return DayPeriod::Evening;
  return DayPeriod::Night;
}

const char* dayPeriodPrefix(DayPeriod period) {
  switch (period) {
    case DayPeriod::Morning:
      return "morning";
    case DayPeriod::Afternoon:
      return "afternoon";
    case DayPeriod::Evening:
      return "evening";
    case DayPeriod::Night:
      return "night";
  }
  return "morning";
}

std::vector<std::string> greetingsForPeriod(const std::vector<std::string>& lines, DayPeriod period) {
  const std::string wanted = dayPeriodPrefix(period);
  std::vector<std::string> out;
  for (const auto& line : lines) {
    std::string prefix;
    std::string rest;
    if (splitPrefixed(line, &prefix, &rest)) {
      if (prefix == wanted && !rest.empty()) out.push_back(rest);
    } else {
      // 無前綴（或前綴打錯字）＝ 全時段有效
      out.push_back(line);
    }
  }
  return out;
}

}  // namespace l2m
