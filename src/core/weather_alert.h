#pragma once

// 惡劣天氣預警的決策層（純函式；時間由呼叫端傳入，才測得到）。
//
// 這是整個天氣功能的靈魂。產品規則只有一條：**主動開口只留給「使用者會因此
// 改變行動」的天氣**。晴天、多雲、氣溫舒適永遠不觸發 —— 一隻沒事就報天氣的
// 桌寵，兩天內就會被關掉。
//
// 四條決策規則（每一條都對應一個真的會發生的壞情況）：
//
//  1. **已經開始的事件不提醒。** 找到候選小時之後往回走連續的同類小時，
//     若事件起點落在現在或更早，整段跳過。人看得到窗外正在下雨，
//     這時候開口只顯得遲鈍。
//
//  2. **去重鍵 ＝ 前綴 ＋ 事件起點的當地時間**（"rain@2026-09-02T15:00"）。
//     15:00~19:00 這場雨，13:30 與 14:30 算出來的起點都是 15:00 —— 鍵一樣就
//     不會重講。15:00 一過，鍵確實會變成 16:00，但那時規則 1 已經先把它擋下來
//     （事件已經開始）。兩條規則是一組的，只留一條都會讓同一場雨被唸到停為止。
//
//  3. **降水類永遠贏過體感類。** 同一個視窗裡「30 分鐘後下雨」與「30 分鐘後
//     很熱」同時成立時講下雨 —— 帶傘是行動，流汗不是。同類之內取最早的。
//
//  4. **講過的鍵是一個集合，不是一個字串。** 只留最後一個會無限乒乓：
//     視窗裡同時有「+1h 下雨」與「+3h 寒流」時，第一輪講雨並記下雨的鍵，
//     第二輪雨被擋掉於是講寒流、鍵被換成寒流的，第三輪雨又不在記錄裡了……
//     每 5 秒輪一次，直到第一件事真的開始為止。所以呼叫端要把**這次執行期間
//     講過的每一個鍵**都傳進來。只記「講過哪一場」，不記時間 ——
//     事件過去之後鍵自然對不上，不需要任何清理邏輯。
//     跨重啟只保留最後一個（config 的 weather.lastAlertKey）就夠：
//     重開之後最多把另一件還沒發生的事再講一次，不會回到無限重複。
//
// 風速門檻刻意寫死而不開設定：40 km/h 大約是蒲福 6 級（強風），
// 再細分的差別使用者感覺不出來，多一個旋鈕只是多一個要翻譯的字串。

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "weather_types.h"

namespace l2m {

enum class WeatherAlertKind { Rain, Snow, Thunderstorm, Heat, Cold, Wind };

// 體感溫度與風速的門檻（見標頭最後一段）。溫度一律攝氏。
inline constexpr double kHeatApparentC = 35;
inline constexpr double kColdApparentC = 5;
inline constexpr double kWindStrongKmh = 40;

struct WeatherAlert {
  WeatherAlertKind kind = WeatherAlertKind::Rain;
  // 去重鍵："rain@2026-09-02T15:00"（前綴 ＋ 事件起點的當地時間）
  std::string key;
  std::string startLocalTime;  // 事件起點，當地時間 ISO
  int minutesAhead = 0;        // 距離事件開始還有幾分鐘
  int probability = 0;         // 降水機率（體感類為 0）
  double temperature = 0;      // 事件起點的體感溫度
  double windSpeed = 0;        // 事件起點的風速 km/h
  int code = 0;                // 事件起點的 WMO code
  // 給 LLM 與日誌看的一句英文（面向 AI 的字串一律英文）
  std::string summary;
};

struct WeatherAlertOptions {
  int lookaheadMinutes = 120;  // 往前看多久
  // 降水機率門檻（%）。**預設 30 是拿真實資料校準過的，不是憑直覺訂的。**
  //
  // Open-Meteo 的 precipitation_probability 是系集離散度，跟它自己那份確定性的
  // weather_code **對不上**。2026-09-02 台北實測，同一份回應裡：
  //
  //   00:00  prob=73  precipitation=0    code=3（陰天）  ← 機率高但根本沒有雨
  //   22:00  prob=37  precipitation=0.2  code=51（毛毛雨）← 真的要下了
  //   23:00  prob=37  precipitation=0.9  code=53
  //
  // 門檻訂在「人對降雨機率的直覺」（60、70）的話，這一天**一次都不會觸發** ——
  // 真正要下雨的那兩小時只有 37%。擋掉 00:00 那種假警報的其實是
  // triggerFor 的**天氣碼必須是降水類**那一關，機率只負責過濾最弱的那些。
  // 所以門檻要跟著碼表的尺度走，訂低一點；使用者嫌吵再自己調高。
  int rainProbability = 30;
  // 只影響 summary 字串的單位。判斷門檻永遠是攝氏（見 weather_types.h）——
  // 讓門檻跟著單位走，等於 kHeatApparentC 要分兩套。
  bool fahrenheit = false;
};

// 角色卡 # Weather Alert 區塊的行首前綴，也是 WeatherAlertKind 的穩定字串 id。
// "rain" / "snow" / "thunder" / "hot" / "cold" / "wind"
const char* weatherAlertPrefix(WeatherAlertKind kind);

// 這一刻該不該開口。nullopt ＝ 不該（絕大多數的呼叫都是這個答案）。
// announcedKeys 是**已經真的講出去過**的每一則的 key（見規則 4）。
std::optional<WeatherAlert> evaluateWeatherAlert(const WeatherSnapshot& snapshot, std::int64_t nowEpochSec, const WeatherAlertOptions& options, const std::set<std::string>& announcedKeys);

// 從角色卡 # Weather Alert 區塊挑出「這一則適用」的候選。
// 規則與 core/day_period.h 的 greetingsForPeriod 一模一樣（前綴 ＋ 半形或全形冒號，
// 前綴打錯字視為無前綴 ＝ 全類型適用），刻意保持一致 ——
// 使用者學會一種寫法就通吃兩個區塊。
std::vector<std::string> weatherLinesFor(const std::vector<std::string>& lines, WeatherAlertKind kind);

// 送進 LLM prompt 的一行天氣背景（每一輪都帶，不只預警時）。
// snapshot 無效時回空字串 —— 呼叫端據此整段省略，不會出現
// "now: 0C, clear" 這種捏造的天氣。
std::string weatherContextLine(const WeatherSnapshot& snapshot, std::int64_t nowEpochSec, bool fahrenheit);

}  // namespace l2m
