// 惡劣天氣預警的四條決策規則（core/weather_alert.h 的檔頭列的那四條）。
// 每一條都對應一個真的會發生的壞情況，所以逐條釘住。
#include <QtTest>

#include <set>

#include "core/weather_alert.h"

using namespace l2m;

namespace {

// 合成 snapshot 的基準時刻。用固定值而不是「現在」，測試才不會在某些時段才過
constexpr std::int64_t kBase = 1000000;
// 「現在」刻意落在整點之後 10 分鐘：真實情況就是這樣，而 hourly 的格子都在整點
constexpr std::int64_t kNow = kBase + 600;

// index 是相對 kBase 的小時偏移（可為負 —— 今天已經過去的小時）
WeatherHour makeHour(int index, int code, int probability, double apparent = 20, double wind = 5) {
  WeatherHour hour;
  hour.epochSec = kBase + static_cast<std::int64_t>(index) * 3600;
  // localTime 只要唯一且看得懂就好（去重鍵用的是它）
  hour.localTime = QString("2026-09-02T%1:00").arg(index + 12, 2, 10, QChar('0')).toStdString();
  hour.code = code;
  hour.precipitationProbability = probability;
  hour.temperature = apparent;
  hour.apparentTemperature = apparent;
  hour.windSpeed = wind;
  return hour;
}

WeatherSnapshot makeSnapshot(std::vector<WeatherHour> hours) {
  WeatherSnapshot snapshot;
  snapshot.valid = true;
  snapshot.locationName = "Taipei";
  snapshot.now.code = 3;
  snapshot.now.temperature = 24;
  snapshot.now.apparentTemperature = 25;
  snapshot.hourly = std::move(hours);
  return snapshot;
}

// 全部好天氣的一天（-2 ~ +8 小時）
std::vector<WeatherHour> calmDay() {
  std::vector<WeatherHour> hours;
  for (int i = -2; i <= 8; ++i) hours.push_back(makeHour(i, 3, 5));
  return hours;
}

WeatherAlertOptions defaultOptions() {
  WeatherAlertOptions options;
  options.lookaheadMinutes = 120;
  options.rainProbability = 60;
  return options;
}

}  // namespace

class TestWeatherAlert : public QObject {
  Q_OBJECT

private slots:
  // 絕大多數的呼叫都該回 nullopt。這是產品規則本身：沒事不要開口
  void calmWeatherStaysSilent() {
    QVERIFY(!evaluateWeatherAlert(makeSnapshot(calmDay()), kNow, defaultOptions(), {}).has_value());
    // 無效的 snapshot 更不能編一個出來（全零會被讀成攝氏 0 度、晴天）
    QVERIFY(!evaluateWeatherAlert(WeatherSnapshot{}, kNow, defaultOptions(), {}).has_value());
  }

  // 視窗內的降雨會觸發，並帶出「還有幾分鐘」與機率
  void rainWithinTheWindowFires() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 65, 80);  // +1h：大雨 80%
    hours[4] = makeHour(2, 65, 90);
    const auto alert = evaluateWeatherAlert(makeSnapshot(hours), kNow, defaultOptions(), {});
    QVERIFY(alert.has_value());
    QCOMPARE(alert->kind, WeatherAlertKind::Rain);
    QCOMPARE(alert->probability, 80);
    QCOMPARE(alert->minutesAhead, 50);  // 整點 + 60 分，現在是整點後 10 分
    QCOMPARE(alert->key, std::string("rain@2026-09-02T13:00"));
    QVERIFY(alert->summary.find("heavy rain") != std::string::npos);
  }

  // 視窗外的不算：兩小時後才下的雨，現在講了使用者也記不住
  void rainBeyondTheWindowIsIgnored() {
    auto hours = calmDay();
    hours[7] = makeHour(5, 65, 90);  // +5h
    QVERIFY(!evaluateWeatherAlert(makeSnapshot(hours), kNow, defaultOptions(), {}).has_value());
  }

  // 規則 1：窗外正在下的雨不需要人提醒。
  // 事件從 -1h 一路下到 +2h，往回走會走到過去 → 整段跳過
  void ongoingRainIsNotAnnounced() {
    auto hours = calmDay();
    for (int i = 1; i <= 4; ++i) hours[i] = makeHour(i - 2, 65, 90);  // -1h ~ +2h
    QVERIFY(!evaluateWeatherAlert(makeSnapshot(hours), kNow, defaultOptions(), {}).has_value());
  }

  // 規則 2：事件還沒開始的這段期間，鍵不能隨著查詢時刻改變 ——
  // 每輪都算出新鍵的話，去重就完全失效，同一場雨會被一路唸到停為止
  void keyIsStableWhileTheEventHasNotStarted() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 61, 70);
    hours[4] = makeHour(2, 61, 70);
    const auto snapshot = makeSnapshot(hours);
    const auto first = evaluateWeatherAlert(snapshot, kNow, defaultOptions(), {});
    QVERIFY(first.has_value());
    const auto halfHourLater = evaluateWeatherAlert(snapshot, kNow + 1800, defaultOptions(), {});
    QVERIFY(halfHourLater.has_value());
    QCOMPARE(halfHourLater->key, first->key);
    QVERIFY(halfHourLater->minutesAhead < first->minutesAhead);
  }

  // 規則 1 與規則 2 是一組的：事件一開始就由規則 1 收掉。
  // +1h 那場雨到了 +1.5h 已經在下，這時候不該再有任何預警
  void onceTheEventStartsRuleOneTakesOver() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 61, 70);
    hours[4] = makeHour(2, 61, 70);
    QVERIFY(!evaluateWeatherAlert(makeSnapshot(hours), kNow + 5400, defaultOptions(), {}).has_value());
  }

  // 規則 4：講過的那一場不再講（鍵存在 config，關掉重開也記得）
  void sameEventIsNeverAnnouncedTwice() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 65, 80);
    const auto snapshot = makeSnapshot(hours);
    const auto alert = evaluateWeatherAlert(snapshot, kNow, defaultOptions(), {});
    QVERIFY(alert.has_value());
    QVERIFY(!evaluateWeatherAlert(snapshot, kNow, defaultOptions(), {alert->key}).has_value());
  }

  // **兩件都還沒發生的壞天氣不能互相把對方的「講過」記錄擠掉。**
  // 只留最後一個鍵的實作會在這裡無限乒乓：講雨 → 記雨 → 雨被擋掉改講寒流
  //（雨的記錄被蓋掉）→ 雨又不在記錄裡於是再講一次雨 → …… 每 5 秒一輪，
  // 直到第一件事真的開始為止
  void announcedEventsDoNotPingPong() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 65, 80);   // +1h 大雨
    hours[5] = makeHour(3, 0, 0, 2);  // +3h 體感 2 度（寒流）
    WeatherAlertOptions options = defaultOptions();
    options.lookaheadMinutes = 300;
    const auto snapshot = makeSnapshot(hours);

    std::set<std::string> announced;
    const auto first = evaluateWeatherAlert(snapshot, kNow, options, announced);
    QVERIFY(first.has_value());
    QCOMPARE(first->kind, WeatherAlertKind::Rain);  // 規則 3：降水類先講
    announced.insert(first->key);

    const auto second = evaluateWeatherAlert(snapshot, kNow, options, announced);
    QVERIFY(second.has_value());
    QCOMPARE(second->kind, WeatherAlertKind::Cold);
    announced.insert(second->key);

    // 兩件都講過了就該安靜 —— 這一步是原本會無限重複的地方
    QVERIFY(!evaluateWeatherAlert(snapshot, kNow, options, announced).has_value());
  }

  // 但**下一場**還是要講：去重不能變成整天只講一次
  void aLaterSeparateEventStillFires() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 65, 80);  // +1h 雨
    hours[6] = makeHour(4, 73, 85);  // +4h 雪（中間隔著好天氣）
    WeatherAlertOptions options = defaultOptions();
    options.lookaheadMinutes = 400;
    const auto snapshot = makeSnapshot(hours);
    const auto rain = evaluateWeatherAlert(snapshot, kNow, options, {});
    QVERIFY(rain.has_value());
    QCOMPARE(rain->kind, WeatherAlertKind::Rain);
    const auto snow = evaluateWeatherAlert(snapshot, kNow, options, {rain->key});
    QVERIFY(snow.has_value());
    QCOMPARE(snow->kind, WeatherAlertKind::Snow);
  }

  // 規則 3：帶傘是行動，流汗不是 —— 降水類永遠贏過體感類，即使它比較晚發生
  void precipitationOutranksComfort() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 0, 0, 38);  // +1h 體感 38 度
    hours[5] = makeHour(3, 65, 80);    // +3h 大雨
    WeatherAlertOptions options = defaultOptions();
    options.lookaheadMinutes = 240;
    const auto alert = evaluateWeatherAlert(makeSnapshot(hours), kNow, options, {});
    QVERIFY(alert.has_value());
    QCOMPARE(alert->kind, WeatherAlertKind::Rain);
  }

  // 沒有降水時體感類才出場
  void comfortAlertsFireOnTheirOwn() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 0, 0, 38);
    const auto heat = evaluateWeatherAlert(makeSnapshot(hours), kNow, defaultOptions(), {});
    QVERIFY(heat.has_value());
    QCOMPARE(heat->kind, WeatherAlertKind::Heat);

    hours[3] = makeHour(1, 0, 0, 2);
    const auto cold = evaluateWeatherAlert(makeSnapshot(hours), kNow, defaultOptions(), {});
    QVERIFY(cold.has_value());
    QCOMPARE(cold->kind, WeatherAlertKind::Cold);

    hours[3] = makeHour(1, 0, 0, 20, 55);
    const auto wind = evaluateWeatherAlert(makeSnapshot(hours), kNow, defaultOptions(), {});
    QVERIFY(wind.has_value());
    QCOMPARE(wind->kind, WeatherAlertKind::Wind);
  }

  // 機率沒到門檻就不吵；門檻調低才吵
  void probabilityThresholdIsRespected() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 61, 40);
    QVERIFY(!evaluateWeatherAlert(makeSnapshot(hours), kNow, defaultOptions(), {}).has_value());
    WeatherAlertOptions loose = defaultOptions();
    loose.rainProbability = 30;
    QVERIFY(evaluateWeatherAlert(makeSnapshot(hours), kNow, loose, {}).has_value());
  }

  // **機率高但天氣碼不是降水類 → 不觸發。** 這是擋掉假警報的那一關：
  // Open-Meteo 的 precipitation_probability 是系集離散度，跟它自己的確定性
  // weather_code 對不上（2026-09-02 台北實測 00:00 是 prob=73、precipitation=0、
  // code=3）。只看機率的實作會在那種整天不下雨的陰天一直叫人帶傘
  void highProbabilityWithoutRainCodeStaysSilent() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 3, 95);  // 陰天，卻標了 95% 機率
    QVERIFY(!evaluateWeatherAlert(makeSnapshot(hours), kNow, defaultOptions(), {}).has_value());
  }

  // 反過來：機率不高但碼確實是毛毛雨 → 要觸發（同一份實測資料裡真正會下的那兩小時
  // 只有 37%，門檻訂在直覺的 60 會讓那天一次都不觸發）
  void modestProbabilityWithADrizzleCodeStillFires() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 51, 37);
    WeatherAlertOptions options = defaultOptions();
    options.rainProbability = 30;  // ＝ WeatherAlertOptions 的預設值
    const auto alert = evaluateWeatherAlert(makeSnapshot(hours), kNow, options, {});
    QVERIFY(alert.has_value());
    QCOMPARE(alert->kind, WeatherAlertKind::Rain);
  }

  // 認不得的天氣碼一律不觸發任何預警。
  // 沒有這條的話「整排資料缺失」會被讀成「體感 0 度」而發出寒流預警
  void missingDataNeverProducesAColdSnap() {
    auto hours = calmDay();
    hours[3] = makeHour(1, -1, 0, 0, 0);
    QVERIFY(!evaluateWeatherAlert(makeSnapshot(hours), kNow, defaultOptions(), {}).has_value());
  }

  // 華氏只改字串，不改門檻：38°C 照樣觸發 Heat，只是 summary 講 100F
  void fahrenheitOnlyChangesTheSummary() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 0, 0, 38);
    WeatherAlertOptions options = defaultOptions();
    options.fahrenheit = true;
    const auto alert = evaluateWeatherAlert(makeSnapshot(hours), kNow, options, {});
    QVERIFY(alert.has_value());
    QCOMPARE(alert->kind, WeatherAlertKind::Heat);
    QVERIFY(alert->summary.find("100F") != std::string::npos);
  }

  // 角色卡的前綴過濾與 # Greeting by Time 同一套規則（大小寫、全形冒號、無前綴全適用）
  void personaLinesFilterByPrefix() {
    const std::vector<std::string> lines{
      "rain: 快下雨了，帶傘", "SNOW: 要下雪囉", "thunder\xEF\xBC\x9A打雷了", "天氣要變了", "typo: 這行前綴打錯，視為全適用",
    };
    QCOMPARE(weatherLinesFor(lines, WeatherAlertKind::Rain), (std::vector<std::string>{"快下雨了，帶傘", "天氣要變了", "typo: 這行前綴打錯，視為全適用"}));
    QCOMPARE(weatherLinesFor(lines, WeatherAlertKind::Snow), (std::vector<std::string>{"要下雪囉", "天氣要變了", "typo: 這行前綴打錯，視為全適用"}));
    QCOMPARE(weatherLinesFor(lines, WeatherAlertKind::Thunderstorm), (std::vector<std::string>{"打雷了", "天氣要變了", "typo: 這行前綴打錯，視為全適用"}));
  }

  // 背景那一行：無效 snapshot 一律空字串，不能讓 LLM 拿全零的資料編天氣
  void contextLineIsEmptyWithoutData() { QVERIFY(weatherContextLine(WeatherSnapshot{}, kNow, false).empty()); }

  // 有資料時報現況，並挑未來幾小時「機率最高」的那一格（不是第一格）
  void contextLineReportsThePeakHour() {
    auto hours = calmDay();
    hours[3] = makeHour(1, 61, 30);
    hours[5] = makeHour(3, 65, 90);
    const std::string line = weatherContextLine(makeSnapshot(hours), kNow, false);
    QVERIFY(line.find("Taipei") != std::string::npos);
    QVERIFY(line.find("now 24C") != std::string::npos);
    QVERIFY(line.find("heavy rain") != std::string::npos);
    QVERIFY(line.find("(90%)") != std::string::npos);

    QVERIFY(weatherContextLine(makeSnapshot(calmDay()), kNow, false).find("no rain expected") != std::string::npos);
  }
};

QTEST_GUILESS_MAIN(TestWeatherAlert)
#include "test_weather_alert.moc"
