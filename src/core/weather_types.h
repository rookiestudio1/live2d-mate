#pragma once

// 天氣資料的純資料型別。
//
// 時間一律存兩份：`localTime` 是資料來源給的當地時間字串（"2026-09-02T15:00"），
// 只拿來當**去重鍵**與顯示；比較與算差值一律用 `epochSec`（UTC 秒）。
// 只留字串的話跨日、跨時區的比較會靜默出錯，只留 epoch 則去重鍵會隨時區飄。
//
// 溫度一律攝氏，**不論使用者選哪個單位**：門檻（core/weather_alert.h 的
// kHeatApparentC 等）是寫死的常數，讓抓回來的數字跟著單位變，等於每個門檻
// 都要跟著分兩套。華氏只在組字串給人／AI 看的那一刻換算。

#include <cstdint>
#include <string>
#include <vector>

namespace l2m {

// WMO weather code（0..99）的語意分類。原始碼表有一堆洞（4~44、46、47…都不存在），
// 認不得的一律 Unknown —— 猜一個最接近的分類會讓「下雪講成起霧」這種錯默默發生。
enum class WeatherKind { Unknown, Clear, Cloudy, Fog, Drizzle, Rain, FreezingRain, Snow, Thunderstorm };

// 逐小時預報的一格。
struct WeatherHour {
  std::string localTime;             // 當地時間 ISO（"2026-09-02T15:00"）
  std::int64_t epochSec = 0;         // 同一時刻的 UTC 秒
  double temperature = 0;            // °C
  double apparentTemperature = 0;    // °C，體感
  int precipitationProbability = 0;  // 0..100
  double windSpeed = 0;              // km/h（10 公尺高）
  int code = 0;                      // WMO weather code
};

// 現在的天氣。刻意跟 WeatherHour 分開：來源的 current 區塊沒有降水機率，
// 硬塞成同一個型別會讓「機率 0」看起來像「不會下雨」。
struct WeatherNow {
  double temperature = 0;
  double apparentTemperature = 0;
  double windSpeed = 0;
  int code = 0;
};

struct WeatherSnapshot {
  // false ＝ 還沒抓到過任何資料（或最近一次失敗）。所有讀取端都要先看這一格：
  // 全零的 snapshot 會被解讀成「攝氏 0 度、晴天」。
  bool valid = false;
  std::string locationName;  // 顯示用（使用者填的或 geocoding 回的）
  double latitude = 0;
  double longitude = 0;
  int utcOffsetSec = 0;
  WeatherNow now;
  std::vector<WeatherHour> hourly;  // **依時間遞增**，含今天已經過去的小時
                                    //（往回找連續事件的起點要用，見 weather_alert.h）
  std::int64_t fetchedEpochSec = 0;
};

}  // namespace l2m
