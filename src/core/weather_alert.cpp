#include "weather_alert.h"

#include <cmath>
#include <string>
#include <utility>

#include "string_util.h"
#include "weather_code.h"

namespace l2m {

namespace {

// 背景資訊那一行往前看多久。刻意比預警的 lookahead 長很多：
// 預警要的是「馬上要發生」，背景資訊要的是「今天下午會怎樣」。
constexpr int kContextHorizonMinutes = 360;
// 背景資訊裡值得一提的降水機率下限。比預警門檻低 —— 這一行不會讓桌寵開口，
// 只是讓 LLM 在本來就要講話時知道「等下可能會下」。
constexpr int kContextMentionProbability = 20;

const char* kPrefixes[] = {"rain", "snow", "thunder", "hot", "cold", "wind"};

std::string formatTemp(double celsius, bool fahrenheit) {
  const double value = fahrenheit ? celsius * 9.0 / 5.0 + 32.0 : celsius;
  return std::to_string(static_cast<int>(std::lround(value))) + (fahrenheit ? "F" : "C");
}

// "2026-09-02T15:00" → "15:00"。給人看的字串裡日期是多餘的（都在今明兩天）
std::string clockOf(const std::string& localTime) {
  const size_t marker = localTime.find('T');
  return marker == std::string::npos ? localTime : localTime.substr(marker + 1);
}

bool isPrecipitationAlert(WeatherAlertKind kind) { return kind == WeatherAlertKind::Rain || kind == WeatherAlertKind::Snow || kind == WeatherAlertKind::Thunderstorm; }

// 這一小時觸發哪一種預警（沒有就 nullopt）。
//
// 認不得的 weather code 一律不觸發任何預警：那種格子通常是整排資料缺失，
// 而溫度的預設值是 0 —— 不擋的話「沒有資料」會被讀成「體感 0 度」而發出寒流預警。
std::optional<WeatherAlertKind> triggerFor(const WeatherHour& hour, const WeatherAlertOptions& options) {
  const WeatherKind kind = weatherKindFor(hour.code);
  if (kind == WeatherKind::Unknown) return std::nullopt;

  if (weatherKindIsPrecipitation(kind) && hour.precipitationProbability >= options.rainProbability) {
    switch (kind) {
      case WeatherKind::Thunderstorm:
        return WeatherAlertKind::Thunderstorm;
      case WeatherKind::Snow:
        return WeatherAlertKind::Snow;
      default:
        // 毛毛雨／雨／凍雨都當「雨」：使用者要做的事一樣（帶傘），
        // 分那麼細只會讓角色卡要多寫兩個前綴
        return WeatherAlertKind::Rain;
    }
  }
  if (hour.apparentTemperature >= kHeatApparentC) return WeatherAlertKind::Heat;
  if (hour.apparentTemperature <= kColdApparentC) return WeatherAlertKind::Cold;
  if (hour.windSpeed >= kWindStrongKmh) return WeatherAlertKind::Wind;
  return std::nullopt;
}

std::string summaryFor(WeatherAlertKind kind, const WeatherHour& hour, int minutesAhead, bool fahrenheit) {
  const std::string when = " around " + clockOf(hour.localTime) + " (in " + std::to_string(minutesAhead) + " min)";
  switch (kind) {
    case WeatherAlertKind::Rain:
    case WeatherAlertKind::Snow:
    case WeatherAlertKind::Thunderstorm:
      return std::string(weatherCodeDescription(hour.code)) + when + ", " + std::to_string(hour.precipitationProbability) + "% chance";
    case WeatherAlertKind::Heat:
      return "very hot: feels like " + formatTemp(hour.apparentTemperature, fahrenheit) + when;
    case WeatherAlertKind::Cold:
      return "very cold: feels like " + formatTemp(hour.apparentTemperature, fahrenheit) + when;
    case WeatherAlertKind::Wind:
      return "strong wind: " + std::to_string(static_cast<int>(std::lround(hour.windSpeed))) + " km/h" + when;
  }
  return "bad weather" + when;
}

WeatherAlert makeAlert(WeatherAlertKind kind, const WeatherHour& start, std::string key, std::int64_t nowEpochSec, const WeatherAlertOptions& options) {
  WeatherAlert alert;
  alert.kind = kind;
  alert.key = std::move(key);
  alert.startLocalTime = start.localTime;
  alert.minutesAhead = static_cast<int>((start.epochSec - nowEpochSec) / 60);
  alert.probability = start.precipitationProbability;
  alert.temperature = start.apparentTemperature;
  alert.windSpeed = start.windSpeed;
  alert.code = start.code;
  alert.summary = summaryFor(kind, start, alert.minutesAhead, options.fahrenheit);
  return alert;
}

// 行首是不是某個天氣前綴（後面跟半形或全形冒號）；是的話回冒號後的內容。
// 與 core/day_period.cpp 的 splitPrefixed 同一套規則（兩區塊的寫法要一致）
bool splitPrefixed(const std::string& line, std::string* prefix, std::string* rest) {
  for (const char* candidate : kPrefixes) {
    const size_t length = std::string(candidate).size();
    if (line.size() <= length) continue;
    if (!strutil::equalsInsensitive(line.substr(0, length), candidate)) continue;
    std::string tail = line.substr(length);
    if (!tail.empty() && tail[0] == ':') {
      tail.erase(0, 1);
    } else if (tail.rfind("\xEF\xBC\x9A", 0) == 0) {  // 全形冒號（UTF-8 三位元組）
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

const char* weatherAlertPrefix(WeatherAlertKind kind) {
  switch (kind) {
    case WeatherAlertKind::Rain:
      return "rain";
    case WeatherAlertKind::Snow:
      return "snow";
    case WeatherAlertKind::Thunderstorm:
      return "thunder";
    case WeatherAlertKind::Heat:
      return "hot";
    case WeatherAlertKind::Cold:
      return "cold";
    case WeatherAlertKind::Wind:
      return "wind";
  }
  return "rain";
}

std::optional<WeatherAlert> evaluateWeatherAlert(const WeatherSnapshot& snapshot, std::int64_t nowEpochSec, const WeatherAlertOptions& options, const std::set<std::string>& announcedKeys) {
  if (!snapshot.valid || snapshot.hourly.empty()) return std::nullopt;
  const std::int64_t windowEnd = nowEpochSec + static_cast<std::int64_t>(options.lookaheadMinutes) * 60;
  const auto& hours = snapshot.hourly;

  // 體感類的最早候選先留著：降水類永遠優先（規則 3），
  // 所以要把整個視窗掃完才知道有沒有更該講的事
  std::optional<WeatherAlert> comfort;

  for (size_t i = 0; i < hours.size(); ++i) {
    if (hours[i].epochSec <= nowEpochSec) continue;
    if (hours[i].epochSec > windowEnd) break;
    const auto kind = triggerFor(hours[i], options);
    if (!kind) continue;

    // 這一段連續同類事件的頭尾。頭可能落在過去（正在下的雨）；
    // 尾用來把整段跳過 —— 同一段裡的每一小時算出來的 key 都一樣，逐格重算只是白做
    size_t start = i;
    while (start > 0 && triggerFor(hours[start - 1], options) == kind) --start;
    size_t end = i;
    while (end + 1 < hours.size() && triggerFor(hours[end + 1], options) == kind) ++end;
    i = end;

    // 規則 1：事件已經開始 —— 窗外正在下的雨不需要人提醒
    if (hours[start].epochSec <= nowEpochSec) continue;
    // 規則 2／4：這一場講過了。比對的是**整個集合**而不是最後一個 ——
    // 只留最後一個時，兩件都還沒發生的壞天氣會互相把對方的記錄擠掉而無限乒乓
    const std::string key = std::string(weatherAlertPrefix(*kind)) + "@" + hours[start].localTime;
    if (announcedKeys.count(key) > 0) continue;

    WeatherAlert alert = makeAlert(*kind, hours[start], key, nowEpochSec, options);
    if (isPrecipitationAlert(*kind)) return alert;
    if (!comfort) comfort = std::move(alert);
  }
  return comfort;
}

std::vector<std::string> weatherLinesFor(const std::vector<std::string>& lines, WeatherAlertKind kind) {
  const std::string wanted = weatherAlertPrefix(kind);
  std::vector<std::string> out;
  for (const auto& line : lines) {
    std::string prefix;
    std::string rest;
    if (splitPrefixed(line, &prefix, &rest)) {
      if (prefix == wanted && !rest.empty()) out.push_back(rest);
    } else {
      // 無前綴（或前綴打錯字）＝ 所有天氣都適用
      out.push_back(line);
    }
  }
  return out;
}

std::string weatherContextLine(const WeatherSnapshot& snapshot, std::int64_t nowEpochSec, bool fahrenheit) {
  if (!snapshot.valid) return {};

  std::string out;
  if (!snapshot.locationName.empty()) out += snapshot.locationName + ", ";
  out += "now " + formatTemp(snapshot.now.temperature, fahrenheit);
  if (std::fabs(snapshot.now.apparentTemperature - snapshot.now.temperature) >= 1) {
    out += " (feels " + formatTemp(snapshot.now.apparentTemperature, fahrenheit) + ")";
  }
  out += ", ";
  out += weatherCodeDescription(snapshot.now.code);

  // 未來幾小時最有機會下的那一格。挑「機率最高」而不是「第一個」——
  // 20% 接著 90% 的時候，值得講的是 90% 那一格
  const std::int64_t horizonEnd = nowEpochSec + static_cast<std::int64_t>(kContextHorizonMinutes) * 60;
  const WeatherHour* peak = nullptr;
  for (const auto& hour : snapshot.hourly) {
    if (hour.epochSec <= nowEpochSec) continue;
    if (hour.epochSec > horizonEnd) break;
    if (!weatherKindIsPrecipitation(weatherKindFor(hour.code))) continue;
    if (hour.precipitationProbability < kContextMentionProbability) continue;
    if (!peak || hour.precipitationProbability > peak->precipitationProbability) peak = &hour;
  }
  const std::string horizon = std::to_string(kContextHorizonMinutes / 60) + "h";
  if (peak) {
    out += "; next " + horizon + ": " + weatherCodeDescription(peak->code) + " around " + clockOf(peak->localTime) + " (" + std::to_string(peak->precipitationProbability) + "%)";
  } else {
    out += "; next " + horizon + ": no rain expected";
  }
  return out;
}

}  // namespace l2m
