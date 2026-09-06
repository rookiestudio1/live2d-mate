#include "weather_http.h"

#include <QDateTime>
#include <QString>
#include <QTimeZone>

#include <cstdio>

#include "json_doc.h"
#include "string_util.h"

namespace l2m {

namespace {

constexpr const char* kForecastBase = "https://api.open-meteo.com/v1/forecast";
constexpr const char* kGeocodeBase = "https://geocoding-api.open-meteo.com/v1/search";
constexpr const char* kIpLocation = "https://ipwho.is/";

// 百分比編碼。tts_http.cpp 有一份一模一樣的，刻意不共用 ——
// 那一支是 ${TEXT} 替換規則的一部分（改動要跟著自訂 TTS 的語意走），
// 這裡只是把城市名塞進 query。十行的重複比一個跨模組耦合便宜。
std::string urlEncode(const std::string& raw) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(raw.size() * 3);
  for (const unsigned char ch : raw) {
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
      out += static_cast<char>(ch);
    } else {
      out += '%';
      out += kHex[ch >> 4];
      out += kHex[ch & 0x0F];
    }
  }
  return out;
}

// 座標寫進 URL。小數四位約 11 公尺，遠超過天氣預報的網格解析度（1~11 公里），
// 再多位只是讓 URL 變長、快取命中率變差。
std::string formatCoord(double value) {
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.4f", value);
  return buffer;
}

// 資料來源給的是**當地時間、不帶時區後綴**的 ISO（"2026-09-02T15:00"）。
// 先當成 UTC 解析、再減掉 utc_offset_seconds，才是真正的 UTC 秒。
// 直接丟給 Qt 用本機時區解析是錯的：使用者可以把地點設在別的時區。
std::optional<std::int64_t> parseLocalIso(const std::string& text, int utcOffsetSec) {
  const QDateTime parsed = QDateTime::fromString(QString::fromStdString(text), Qt::ISODate);
  if (!parsed.isValid()) return std::nullopt;
  const QDateTime asUtc(parsed.date(), parsed.time(), QTimeZone::UTC);
  if (!asUtc.isValid()) return std::nullopt;
  return static_cast<std::int64_t>(asUtc.toSecsSinceEpoch()) - utcOffsetSec;
}

double numAt(yyjson_val* arr, size_t index, double fallback) {
  yyjson_val* value = arr ? yyjson_arr_get(arr, index) : nullptr;
  return (value && yyjson_is_num(value)) ? yyjson_get_num(value) : fallback;
}

double numField(yyjson_val* obj, const char* key, double fallback) {
  yyjson_val* value = jsonu::get(obj, key);
  return (value && yyjson_is_num(value)) ? yyjson_get_num(value) : fallback;
}

}  // namespace

std::optional<std::string> weatherConfigIssue(const WeatherConfig& config) {
  if (!weatherHasLocation(config) && !config.autoLocate) {
    return std::string("No location set. Enter a city name, or turn on automatic location detection.");
  }
  return std::nullopt;
}

bool weatherHasLocation(const WeatherConfig& config) { return config.latitude != 0 || config.longitude != 0; }

std::string buildForecastUrl(double latitude, double longitude) {
  std::string url = kForecastBase;
  url += "?latitude=" + formatCoord(latitude);
  url += "&longitude=" + formatCoord(longitude);
  url += "&current=temperature_2m,apparent_temperature,weather_code,wind_speed_10m";
  url += "&hourly=temperature_2m,apparent_temperature,precipitation_probability,weather_code,wind_speed_10m";
  // timezone=auto 讓回傳的時間字串是**當地時間**，去重鍵才讀得懂
  //（"rain@2026-09-02T15:00" 一看就知道是下午三點）
  url += "&timezone=auto&forecast_days=2&temperature_unit=celsius&wind_speed_unit=kmh";
  return url;
}

std::string buildGeocodeUrl(const std::string& name, const std::string& language) {
  std::string url = kGeocodeBase;
  url += "?name=" + urlEncode(strutil::trim(name));
  url += "&count=1&format=json";
  url += "&language=" + urlEncode(language.empty() ? "en" : language);
  return url;
}

const char* ipLocationUrl() { return kIpLocation; }

std::optional<WeatherSnapshot> parseForecast(const std::string& body, std::string* error) {
  const auto doc = jsonu::Doc::parse(body);
  if (!doc || !doc->root() || !yyjson_is_obj(doc->root())) {
    if (error) *error = "weather response is not valid JSON";
    return std::nullopt;
  }
  yyjson_val* root = doc->root();

  // Open-Meteo 的錯誤回應是 {"error":true,"reason":"..."}，狀態碼也是 4xx，
  // 但把 reason 撈出來才有辦法告訴使用者是座標錯還是參數錯
  yyjson_val* errorFlag = jsonu::get(root, "error");
  if (errorFlag && yyjson_is_true(errorFlag)) {
    if (error) *error = jsonu::getString(root, "reason", "weather service rejected the request");
    return std::nullopt;
  }

  WeatherSnapshot snapshot;
  snapshot.latitude = numField(root, "latitude", 0);
  snapshot.longitude = numField(root, "longitude", 0);
  snapshot.utcOffsetSec = static_cast<int>(numField(root, "utc_offset_seconds", 0));

  yyjson_val* current = jsonu::get(root, "current");
  if (!current || !yyjson_is_obj(current)) {
    if (error) *error = "weather response has no current conditions";
    return std::nullopt;
  }
  snapshot.now.temperature = numField(current, "temperature_2m", 0);
  snapshot.now.apparentTemperature = numField(current, "apparent_temperature", snapshot.now.temperature);
  snapshot.now.windSpeed = numField(current, "wind_speed_10m", 0);
  snapshot.now.code = static_cast<int>(numField(current, "weather_code", -1));
  if (const auto epoch = parseLocalIso(jsonu::getString(current, "time"), snapshot.utcOffsetSec)) {
    snapshot.fetchedEpochSec = *epoch;
  }

  yyjson_val* hourly = jsonu::get(root, "hourly");
  yyjson_val* times = jsonu::get(hourly, "time");
  if (!times || !yyjson_is_arr(times)) {
    if (error) *error = "weather response has no hourly forecast";
    return std::nullopt;
  }
  yyjson_val* temps = jsonu::get(hourly, "temperature_2m");
  yyjson_val* apparent = jsonu::get(hourly, "apparent_temperature");
  yyjson_val* probability = jsonu::get(hourly, "precipitation_probability");
  yyjson_val* codes = jsonu::get(hourly, "weather_code");
  yyjson_val* wind = jsonu::get(hourly, "wind_speed_10m");

  const size_t count = yyjson_arr_size(times);
  snapshot.hourly.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    WeatherHour hour;
    hour.localTime = jsonu::asString(yyjson_arr_get(times, i));
    // 時間解不開的那一格整格丟掉：epochSec 為 0 的格子會被當成 1970 年，
    // 一路排在最前面，讓「未來兩小時」的掃描每次都掃到它
    const auto epoch = parseLocalIso(hour.localTime, snapshot.utcOffsetSec);
    if (!epoch) continue;
    hour.epochSec = *epoch;
    hour.temperature = numAt(temps, i, 0);
    hour.apparentTemperature = numAt(apparent, i, hour.temperature);
    // 遠端的小時常常是 null（模式還沒算到）—— 當成 0，不是「一定不會下」，
    // 但那些小時本來就在 lookahead 之外
    hour.precipitationProbability = static_cast<int>(numAt(probability, i, 0));
    hour.windSpeed = numAt(wind, i, 0);
    // -1 ＝ 沒有碼，weatherKindFor 會回 Unknown（0 是「晴天」，不能拿來當缺值）
    hour.code = static_cast<int>(numAt(codes, i, -1));
    snapshot.hourly.push_back(std::move(hour));
  }
  if (snapshot.hourly.empty()) {
    if (error) *error = "weather response has no usable hourly rows";
    return std::nullopt;
  }

  snapshot.valid = true;
  return snapshot;
}

std::optional<GeoLocation> parseGeocode(const std::string& body, std::string* error) {
  const auto doc = jsonu::Doc::parse(body);
  if (!doc || !doc->root()) {
    if (error) *error = "geocoding response is not valid JSON";
    return std::nullopt;
  }
  yyjson_val* results = jsonu::get(doc->root(), "results");
  // 查無此地時 Open-Meteo 回 200 但整個 results 欄位不存在（不是空陣列）
  if (!results || !yyjson_is_arr(results) || yyjson_arr_size(results) == 0) {
    if (error) *error = "no place matched that name";
    return std::nullopt;
  }
  yyjson_val* first = yyjson_arr_get(results, 0);
  GeoLocation location;
  location.name = jsonu::getString(first, "name");
  location.latitude = numField(first, "latitude", 0);
  location.longitude = numField(first, "longitude", 0);
  if (location.name.empty()) {
    if (error) *error = "geocoding result has no place name";
    return std::nullopt;
  }
  return location;
}

std::optional<GeoLocation> parseIpLocation(const std::string& body, std::string* error) {
  const auto doc = jsonu::Doc::parse(body);
  if (!doc || !doc->root()) {
    if (error) *error = "location response is not valid JSON";
    return std::nullopt;
  }
  yyjson_val* root = doc->root();
  // ipwho.is 的失敗形狀是 success:false ＋ message（不是 Open-Meteo 那種
  // error:true ＋ reason）。認錯的話服務講的原因會被下面那句泛用的
  // 「沒有座標」蓋掉，使用者在設定頁只看得到一句對不上的話
  yyjson_val* success = jsonu::get(root, "success");
  if (success && yyjson_is_false(success)) {
    if (error) *error = jsonu::getString(root, "message", "location service rejected the request");
    return std::nullopt;
  }
  GeoLocation location;
  location.latitude = numField(root, "latitude", 0);
  location.longitude = numField(root, "longitude", 0);
  location.name = jsonu::getString(root, "city");
  if (location.latitude == 0 && location.longitude == 0) {
    if (error) *error = "location response has no coordinates";
    return std::nullopt;
  }
  // 城市名缺了不算失敗：座標才是預報要用的，名字只是顯示
  if (location.name.empty()) location.name = jsonu::getString(root, "region", "Unknown");
  return location;
}

}  // namespace l2m
