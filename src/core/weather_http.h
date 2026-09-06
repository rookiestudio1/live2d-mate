#pragma once

// Open-Meteo 的請求組裝與回應解析（純函式，不碰網路）。
// 非同步搬運在 src/media/weather_service.h。
//
// 放 core 的理由跟 core/tts_http.h、core/llm_http.h 是同一條：query 少一個
// 欄位、或把 `precipitation_probability` 打成 `precipitation`，症狀是
// 「桌寵從來不提醒下雨」—— 沒有任何錯誤訊息，只有沉默。這種東西只有測試釘得住。
//
// 為什麼是 Open-Meteo：免金鑰、免註冊，一支 GET 就有逐小時降水機率。
// 桌寵這種產品最重要的是「使用者什麼都不用設定就能用」，任何要先去申請
// API key 的來源都會讓這個功能對九成的人不存在。
//
// **一律抓攝氏**（見 weather_types.h）；華氏只在組描述字串時換算。

#include <optional>
#include <string>

#include "config_schema.h"
#include "weather_types.h"

namespace l2m {

// 城市名 → 座標的查詢結果，也是 IP 粗定位的結果型別（兩者要填的欄位一樣）。
struct GeoLocation {
  std::string name;
  double latitude = 0;
  double longitude = 0;
};

// 設定不完整的原因（設定頁的紅字與日誌共用同一句，比照 llmConfigIssue）。
// nullopt ＝ 設定齊全。
std::optional<std::string> weatherConfigIssue(const WeatherConfig& config);

// 座標是不是已經設定過。
// **(0, 0) 當成「未設定」**：那個點在幾內亞灣外海，沒有人住在那裡，而多一個
// `hasLocation` 布林要跨 config、UI、patch 三處維護。這是刻意的取捨。
bool weatherHasLocation(const WeatherConfig& config);

// 逐小時預報。forecast_days=2 —— 只要 lookahead 內的那幾小時，但**必須含
// 今天已經過去的小時**（往回找連續降雨事件的起點要用，見 weather_alert.h）。
std::string buildForecastUrl(double latitude, double longitude);

// 城市名 → 座標。language 讓「台北」「東京」用母語也查得到；空字串退回 "en"。
std::string buildGeocodeUrl(const std::string& name, const std::string& language);

// IP 粗定位（免金鑰）。只在使用者沒填地點、又開著自動偵測時打一次，
// 結果寫回 config 就不再打 —— 失敗不是錯誤，只是「這次沒定位到」。
//
// **端點是 ipwho.is，而且換掉它一定要是刻意的決定**（tests/test_weather_http.cpp
// 把它釘住了）。第一版用的是 ipapi.co，2026-09-03 在使用者機器上實測踩到：
//
//   User-Agent: Mozilla/5.0   → 403，body 是 Cloudflare 的「Just a moment…」挑戰頁
//   User-Agent: live2d_mate/… → 200
//   任何 UA，連打四五次      → 429 {"error":true,"reason":"RateLimited"}
//
// 也就是說它同時卡在兩件事上：Cloudflare 的 bot 防護會擋掉「不像瀏覽器、
// 又不表明身分」的用戶端（Qt 的 QNAM 預設一個 User-Agent 都不送），
// 而免費層的爆量限制緊到連正常重啟幾次都會踩到。
// 對一個賣點是「使用者什麼都不用設定就能用」的功能來說，這兩件事都是致命的。
//
// ipwho.is 同樣免金鑰免註冊，實測**完全不帶 User-Agent 也回 200**、
// 連打五次也沒有被限流。回應的欄位名（latitude / longitude / city）與前一家相同，
// 只有失敗形狀不同（success:false ＋ message）。
const char* ipLocationUrl();

std::optional<WeatherSnapshot> parseForecast(const std::string& body, std::string* error);
std::optional<GeoLocation> parseGeocode(const std::string& body, std::string* error);
std::optional<GeoLocation> parseIpLocation(const std::string& body, std::string* error);

}  // namespace l2m
