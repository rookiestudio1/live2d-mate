// Open-Meteo 的 URL 組裝與回應解析。
// query 少一個欄位、或把 precipitation_probability 打成 precipitation，症狀是
// 「桌寵從來不提醒下雨」而且沒有任何錯誤訊息 —— 只有這裡擋得住。
#include <QDateTime>
#include <QtTest>

#include "core/weather_http.h"

using namespace l2m;

namespace {

// 台北，UTC+8。逐小時三格：07:00 / 08:00 / 09:00 當地時間
const char* kForecastBody = R"json({
  "latitude": 25.0,
  "longitude": 121.5,
  "utc_offset_seconds": 28800,
  "timezone": "Asia/Taipei",
  "current": {
    "time": "2026-09-02T06:30",
    "temperature_2m": 30.4,
    "apparent_temperature": 34.2,
    "weather_code": 3,
    "wind_speed_10m": 11.5
  },
  "hourly": {
    "time": ["2026-09-02T07:00", "2026-09-02T08:00", "2026-09-02T09:00"],
    "temperature_2m": [29.1, 30.2, 31.4],
    "apparent_temperature": [33.0, 35.1, 37.2],
    "precipitation_probability": [10, 80, null],
    "weather_code": [3, 65, null],
    "wind_speed_10m": [12.0, 18.5, 20.0]
  }
})json";

}  // namespace

class TestWeatherHttp : public QObject {
  Q_OBJECT

private slots:
  // 三個欄位少一個就是整個功能靜默失效，所以逐一釘住
  void forecastUrlAsksForEverythingWeNeed() {
    const QString url = QString::fromStdString(buildForecastUrl(25.0330, 121.5654));
    QVERIFY(url.startsWith("https://api.open-meteo.com/v1/forecast?"));
    QVERIFY(url.contains("latitude=25.0330"));
    QVERIFY(url.contains("longitude=121.5654"));
    // 預警要的三樣：降水機率、天氣碼、體感溫度
    QVERIFY(url.contains("precipitation_probability"));
    QVERIFY(url.contains("hourly=") && url.contains("weather_code"));
    QVERIFY(url.contains("apparent_temperature"));
    QVERIFY(url.contains("wind_speed_10m"));
    // 當地時間才讀得懂去重鍵；攝氏是門檻的前提（見 weather_types.h）
    QVERIFY(url.contains("timezone=auto"));
    QVERIFY(url.contains("temperature_unit=celsius"));
    // 往回找連續事件起點要用今天已經過去的小時，所以不能只抓一天
    QVERIFY(url.contains("forecast_days=2"));
  }

  // 城市名要百分比編碼：非 ASCII 直接塞進 query 會讓整個請求壞掉
  void geocodeUrlEncodesTheName() {
    const QString url = QString::fromStdString(buildGeocodeUrl("台北", "zh"));
    QVERIFY(url.startsWith("https://geocoding-api.open-meteo.com/v1/search?"));
    QVERIFY(url.contains("name=%E5%8F%B0%E5%8C%97"));
    QVERIFY(url.contains("language=zh"));
    // 空白與 & 也要編掉，否則「New York」會被截成兩個參數
    QVERIFY(QString::fromStdString(buildGeocodeUrl("New York", "")).contains("name=New%20York"));
    QVERIFY(QString::fromStdString(buildGeocodeUrl("a b", "")).contains("language=en"));
  }

  void forecastParsesCurrentAndHourly() {
    std::string error;
    const auto snapshot = parseForecast(kForecastBody, &error);
    QVERIFY2(snapshot.has_value(), error.c_str());
    QVERIFY(snapshot->valid);
    QCOMPARE(snapshot->utcOffsetSec, 28800);
    QCOMPARE(snapshot->now.code, 3);
    QVERIFY(qFuzzyCompare(snapshot->now.temperature, 30.4));
    QVERIFY(qFuzzyCompare(snapshot->now.apparentTemperature, 34.2));
    QCOMPARE(snapshot->hourly.size(), size_t(3));
    QCOMPARE(snapshot->hourly[1].precipitationProbability, 80);
    QCOMPARE(snapshot->hourly[1].code, 65);
    QCOMPARE(snapshot->hourly[0].localTime, std::string("2026-09-02T07:00"));
  }

  // 時間是**當地時間、不帶時區後綴**，必須減掉 utc_offset_seconds 才是 UTC。
  // 少減這一步的話，設在別的時區的地點會整整差好幾小時 ——
  // 「兩小時後下雨」變成「明天下雨」，預警永遠不觸發
  void localTimesAreConvertedWithTheOffset() {
    const auto snapshot = parseForecast(kForecastBody, nullptr);
    QVERIFY(snapshot.has_value());
    // 台北 07:00 (+08:00) 就是 UTC 前一天 23:00；用 Qt 獨立算一次
    const QDateTime expected = QDateTime::fromString("2026-09-01T23:00:00Z", Qt::ISODate);
    QVERIFY(expected.isValid());
    QCOMPARE(snapshot->hourly[0].epochSec, static_cast<std::int64_t>(expected.toSecsSinceEpoch()));
    // 相鄰兩格剛好差一小時
    QCOMPARE(snapshot->hourly[1].epochSec - snapshot->hourly[0].epochSec, static_cast<std::int64_t>(3600));
  }

  // null 的天氣碼要變成「認不得」而不是 0（0 是晴天）——
  // 讀成晴天的話，缺資料的那幾小時會被當成好天氣
  void missingWeatherCodeIsNotClearSky() {
    const auto snapshot = parseForecast(kForecastBody, nullptr);
    QVERIFY(snapshot.has_value());
    QCOMPARE(snapshot->hourly[2].code, -1);
    QCOMPARE(snapshot->hourly[2].precipitationProbability, 0);
  }

  void forecastErrorsCarryTheReason() {
    std::string error;
    QVERIFY(!parseForecast(R"json({"error": true, "reason": "Latitude must be in range"})json", &error).has_value());
    QCOMPARE(error, std::string("Latitude must be in range"));

    error.clear();
    QVERIFY(!parseForecast("not json at all", &error).has_value());
    QVERIFY(!error.empty());

    error.clear();
    QVERIFY(!parseForecast(R"json({"latitude": 1, "current": {"time": "2026-09-02T06:30"}})json", &error).has_value());
    QVERIFY(!error.empty());
  }

  void geocodeTakesTheFirstResult() {
    std::string error;
    const auto place = parseGeocode(R"json({"results": [{"name": "Taipei", "latitude": 25.05, "longitude": 121.53}, {"name": "Taipei County"}]})json", &error);
    QVERIFY2(place.has_value(), error.c_str());
    QCOMPARE(place->name, std::string("Taipei"));
    QVERIFY(qFuzzyCompare(place->latitude, 25.05));
  }

  // 查無此地時 Open-Meteo 回 200 而且整個 results 欄位不存在（不是空陣列）
  void geocodeMissWithoutResultsField() {
    std::string error;
    QVERIFY(!parseGeocode(R"json({"generationtime_ms": 0.5})json", &error).has_value());
    QVERIFY(!error.empty());
  }

  void ipLocationNeedsCoordinates() {
    std::string error;
    // ipwho.is 成功時的形狀
    const auto place = parseIpLocation(R"json({"ip": "1.2.3.4", "success": true, "city": "Taipei", "region": "Taiwan", "latitude": 25.04, "longitude": 121.56})json", &error);
    QVERIFY2(place.has_value(), error.c_str());
    QCOMPARE(place->name, std::string("Taipei"));

    // 城市名缺了不算失敗（座標才是預報要用的），座標缺了才是
    const auto noCity = parseIpLocation(R"json({"success": true, "region": "Taiwan", "latitude": 25.04, "longitude": 121.56})json", nullptr);
    QVERIFY(noCity.has_value());
    QCOMPARE(noCity->name, std::string("Taiwan"));
    QVERIFY(!parseIpLocation(R"json({"success": true, "city": "Nowhere"})json", nullptr).has_value());
  }

  // ipwho.is 用 success:false ＋ message 回報失敗（不是 error:true ＋ reason）。
  // 認錯形狀的症狀是「座標缺失」這種泛用訊息蓋掉服務真正說的原因，
  // 使用者在設定頁只看得到一句對不上的話
  void ipLocationSurfacesTheServiceMessage() {
    std::string error;
    QVERIFY(!parseIpLocation(R"json({"ip": "999.999.999.999", "success": false, "message": "Invalid IP address"})json", &error).has_value());
    QCOMPARE(error, std::string("Invalid IP address"));
  }

  // 定位端點釘死在測試裡：**換掉它一定要是刻意的決定**。
  // 2026-09-03 踩過 —— 原本用的 ipapi.co 在 Cloudflare 後面，
  // 不表明身分的用戶端會拿到 403「Just a moment…」挑戰頁，
  // 而且免費層連打幾次就 429（見 core/weather_http.h 的檔頭）
  void ipLocationEndpointIsPinned() { QCOMPARE(std::string(ipLocationUrl()), std::string("https://ipwho.is/")); }

  // (0, 0) ＝ 未設定（幾內亞灣外海的取捨，見 weather_http.h）
  void locationIsConsideredUnsetAtOrigin() {
    WeatherConfig config;
    QVERIFY(!weatherHasLocation(config));
    QVERIFY(weatherConfigIssue(config) == std::nullopt);  // 自動偵測開著就不算設定不完整
    config.autoLocate = false;
    QVERIFY(weatherConfigIssue(config).has_value());
    config.latitude = 25.03;
    QVERIFY(weatherHasLocation(config));
    QVERIFY(weatherConfigIssue(config) == std::nullopt);
  }
};

QTEST_GUILESS_MAIN(TestWeatherHttp)
#include "test_weather_http.moc"
