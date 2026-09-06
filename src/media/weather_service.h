#pragma once

// 天氣的非同步搬運：定時抓 Open-Meteo、必要時先做一次 IP 粗定位、
// 把結果放成一份 WeatherSnapshot 給行為大腦讀。
//
// 掛在 l2m_media 而不是開新 target，理由同 src/llm/：HttpJson 在這裡。
// 會錯的邏輯（URL 組裝、JSON 解析、預警判斷）全在 core/weather_*.h，
// 這一層只剩「什麼時候打、打完存哪裡」。
//
// 三個刻意的決定：
//
//  * **服務本身不碰 ConfigStore。** 自動定位成功時發 locationResolved，
//    由 main() 去寫 config —— 服務只讀設定，寫入的責任留在一個地方，
//    否則「服務寫 config → ConfigStore::changed → applyConfig → 服務再寫」
//    這種迴圈很容易在某個分支上真的繞起來。
//
//  * **關掉功能時把 snapshot 清成 invalid**，不是留著舊資料。留著的話
//    使用者關掉天氣之後，LLM 的 prompt 還會夾帶三小時前的天氣講個沒完。
//
//  * **失敗只記在 lastError_，不重試、不退避。** 下一次輪詢（預設 15 分鐘）
//    本來就會再打一次；桌寵沒有任何「非成功不可」的理由，多寫一套重試
//    只是多一組會出錯的狀態。

#include <QObject>
#include <QString>
#include <QTimer>

#include <cstdint>
#include <functional>
#include <string>

#include "core/config_schema.h"
#include "core/weather_http.h"
#include "core/weather_types.h"
#include "http_json.h"

namespace l2m {

class WeatherService : public QObject {
  Q_OBJECT

public:
  WeatherService(HttpJson& http, std::function<WeatherConfig()> getConfig, QObject* parent = nullptr);
  ~WeatherService() override;

  // 目前的天氣。valid 為 false 代表「還沒有資料」——
  // 所有讀取端都要先看它（全零的 snapshot 會被讀成攝氏 0 度、晴天）
  const WeatherSnapshot& snapshot() const { return snapshot_; }
  // 最近一次失敗的原因（英文，與設定頁的紅字同一句）；空＝沒有錯誤
  const std::string& lastError() const { return lastError_; }

  // 設定變更之後呼叫：重排輪詢、地點換了就立刻重抓。
  // 功能關著就停掉計時器並清空 snapshot。
  void applyConfig();

  // 城市名 → 座標（設定頁的地點「套用」按鈕）。
  // done(ok, location, error)；error 是要顯示給使用者看的英文。
  void resolveCity(const std::string& name, const std::string& language, std::function<void(bool, GeoLocation, std::string)> done);

signals:
  void snapshotChanged();
  // 抓取狀態或錯誤訊息變了（設定頁的狀態列）
  void statusChanged();
  // IP 粗定位成功。呼叫端負責寫回 config（見標頭第一點）
  void locationResolved(double latitude, double longitude, const QString& name);

private:
  void tick();
  void fetchForecast(double latitude, double longitude);
  void locateByIp();
  void setError(std::string error);

  HttpJson& http_;
  std::function<WeatherConfig()> getConfig_;
  QTimer timer_;
  WeatherSnapshot snapshot_;
  std::string lastError_;
  HttpJson::CallPtr forecastCall_;
  HttpJson::CallPtr locateCall_;
  // 「還在飛」要自己記。**不能用 CallPtr 判斷** —— HttpJson::Call::cancelled()
  // 只有明確呼叫 cancel() 才會為真，請求正常完成或逾時都不會動它
  //（media/http_json.cpp）。拿它當閘門的話，第一次請求結束之後
  // 那個 CallPtr 就永遠是「非空且沒被取消」，後面每一次都會被擋掉：
  // 開機時斷網 → 定位失敗 → 網路回來 → 再也不會重試，直到重開 app。
  bool locateInFlight_ = false;
  bool forecastInFlight_ = false;
  // 上一次抓預報的時刻（牆上時鐘毫秒；-1 ＝ 還沒抓過）。節流用，見 fetchForecast。
  std::int64_t lastForecastMs_ = -1;
  // 上一次真的抓過的座標：地點換了要立刻重抓，沒換就等下一輪
  double fetchedLatitude_ = 0;
  double fetchedLongitude_ = 0;
  // 上一次嘗試 IP 定位的時刻（單調毫秒）。定位一直失敗時不要每次
  // applyConfig 都打一輪 —— 設定頁上動一下滑桿就會發好幾次 changed
  std::int64_t lastLocateMs_ = -1;
};

}  // namespace l2m
