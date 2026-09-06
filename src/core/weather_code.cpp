#include "weather_code.h"

namespace l2m {

namespace {

// WMO 4677 的子集（Open-Meteo 實際會回的那些）。**逐碼列舉，不用區間**：
// 理由寫在標頭 —— 這張表不連續，區間寫法會把洞裡的碼掃進錯誤的分類。
struct CodeEntry {
  int code;
  WeatherKind kind;
  const char* description;
};

constexpr CodeEntry kCodes[] = {
  {0, WeatherKind::Clear, "clear sky"},
  {1, WeatherKind::Clear, "mainly clear"},
  {2, WeatherKind::Cloudy, "partly cloudy"},
  {3, WeatherKind::Cloudy, "overcast"},
  {45, WeatherKind::Fog, "fog"},
  {48, WeatherKind::Fog, "freezing fog"},
  {51, WeatherKind::Drizzle, "light drizzle"},
  {53, WeatherKind::Drizzle, "drizzle"},
  {55, WeatherKind::Drizzle, "heavy drizzle"},
  {56, WeatherKind::FreezingRain, "light freezing drizzle"},
  {57, WeatherKind::FreezingRain, "freezing drizzle"},
  {61, WeatherKind::Rain, "light rain"},
  {63, WeatherKind::Rain, "rain"},
  {65, WeatherKind::Rain, "heavy rain"},
  {66, WeatherKind::FreezingRain, "light freezing rain"},
  {67, WeatherKind::FreezingRain, "freezing rain"},
  {71, WeatherKind::Snow, "light snow"},
  {73, WeatherKind::Snow, "snow"},
  {75, WeatherKind::Snow, "heavy snow"},
  {77, WeatherKind::Snow, "snow grains"},
  {80, WeatherKind::Rain, "light rain showers"},
  {81, WeatherKind::Rain, "rain showers"},
  {82, WeatherKind::Rain, "violent rain showers"},
  {85, WeatherKind::Snow, "light snow showers"},
  {86, WeatherKind::Snow, "heavy snow showers"},
  {95, WeatherKind::Thunderstorm, "thunderstorm"},
  {96, WeatherKind::Thunderstorm, "thunderstorm with hail"},
  {99, WeatherKind::Thunderstorm, "thunderstorm with heavy hail"},
};

const CodeEntry* findEntry(int code) {
  for (const auto& entry : kCodes) {
    if (entry.code == code) return &entry;
  }
  return nullptr;
}

}  // namespace

WeatherKind weatherKindFor(int code) {
  const CodeEntry* entry = findEntry(code);
  return entry ? entry->kind : WeatherKind::Unknown;
}

bool weatherKindIsPrecipitation(WeatherKind kind) {
  switch (kind) {
    case WeatherKind::Drizzle:
    case WeatherKind::Rain:
    case WeatherKind::FreezingRain:
    case WeatherKind::Snow:
    case WeatherKind::Thunderstorm:
      return true;
    case WeatherKind::Unknown:
    case WeatherKind::Clear:
    case WeatherKind::Cloudy:
    case WeatherKind::Fog:
      return false;
  }
  return false;
}

const char* weatherCodeDescription(int code) {
  const CodeEntry* entry = findEntry(code);
  return entry ? entry->description : "unknown";
}

}  // namespace l2m
