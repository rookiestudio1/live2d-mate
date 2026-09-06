// WMO weather code 的分類與描述。碼表不連續，這裡逐一釘住相鄰但意義完全不同的那幾組。
#include <QtTest>

#include "core/weather_code.h"

using namespace l2m;

class TestWeatherCode : public QObject {
  Q_OBJECT

private slots:
  // 相鄰的號碼分屬不同天氣：65 大雨、66 凍雨、71 下雪。
  // 用區間 if 去猜的實作會在這裡整組錯掉
  void adjacentCodesAreDifferentWeather() {
    QCOMPARE(weatherKindFor(65), WeatherKind::Rain);
    QCOMPARE(weatherKindFor(66), WeatherKind::FreezingRain);
    QCOMPARE(weatherKindFor(67), WeatherKind::FreezingRain);
    QCOMPARE(weatherKindFor(71), WeatherKind::Snow);
    QCOMPARE(weatherKindFor(77), WeatherKind::Snow);
    QCOMPARE(weatherKindFor(80), WeatherKind::Rain);
    QCOMPARE(weatherKindFor(85), WeatherKind::Snow);
    QCOMPARE(weatherKindFor(95), WeatherKind::Thunderstorm);
    QCOMPARE(weatherKindFor(99), WeatherKind::Thunderstorm);
  }

  // 碼表裡的洞（4~44、46、47、58~60…）一律 Unknown，不能被吸進相鄰的分類。
  // 這條是「沒有資料」與「晴天」的分界：0 是晴天，缺值不能也讀成晴天
  void holesInTheTableAreUnknown() {
    for (const int code : {-1, 4, 20, 44, 46, 47, 49, 50, 52, 58, 60, 62, 68, 70, 72, 78, 79, 83, 84, 87, 90, 97, 100}) {
      QCOMPARE(weatherKindFor(code), WeatherKind::Unknown);
    }
    QCOMPARE(QString(weatherCodeDescription(-1)), QString("unknown"));
  }

  void clearAndCloudySplit() {
    QCOMPARE(weatherKindFor(0), WeatherKind::Clear);
    QCOMPARE(weatherKindFor(1), WeatherKind::Clear);
    QCOMPARE(weatherKindFor(2), WeatherKind::Cloudy);
    QCOMPARE(weatherKindFor(3), WeatherKind::Cloudy);
    QCOMPARE(weatherKindFor(45), WeatherKind::Fog);
  }

  // 「會讓人改變行動」的定義：降水類算，晴／多雲／霧不算
  void onlyPrecipitationCountsAsActionable() {
    QVERIFY(weatherKindIsPrecipitation(WeatherKind::Drizzle));
    QVERIFY(weatherKindIsPrecipitation(WeatherKind::Rain));
    QVERIFY(weatherKindIsPrecipitation(WeatherKind::FreezingRain));
    QVERIFY(weatherKindIsPrecipitation(WeatherKind::Snow));
    QVERIFY(weatherKindIsPrecipitation(WeatherKind::Thunderstorm));
    QVERIFY(!weatherKindIsPrecipitation(WeatherKind::Clear));
    QVERIFY(!weatherKindIsPrecipitation(WeatherKind::Cloudy));
    QVERIFY(!weatherKindIsPrecipitation(WeatherKind::Fog));
    QVERIFY(!weatherKindIsPrecipitation(WeatherKind::Unknown));
  }

  // 描述字串是要進 LLM prompt 的，強度必須分得出來
  void descriptionsCarryIntensity() {
    QCOMPARE(QString(weatherCodeDescription(61)), QString("light rain"));
    QCOMPARE(QString(weatherCodeDescription(65)), QString("heavy rain"));
    QCOMPARE(QString(weatherCodeDescription(71)), QString("light snow"));
    QCOMPARE(QString(weatherCodeDescription(75)), QString("heavy snow"));
  }
};

QTEST_GUILESS_MAIN(TestWeatherCode)
#include "test_weather_code.moc"
