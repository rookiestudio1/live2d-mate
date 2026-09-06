#include "weather_service.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QNetworkRequest>
#include <QPointer>
#include <QUrl>

#include <utility>

namespace l2m {

namespace {

constexpr int kForecastTimeoutMs = 15000;
constexpr int kGeocodeTimeoutMs = 15000;
constexpr int kLocateTimeoutMs = 10000;
// IP 定位失敗之後至少隔這麼久才再試一次。設定頁上拖一下滑桿就會發好幾次
// ConfigStore::changed，沒有這道閘就會對免費的定位服務連打好幾發
constexpr std::int64_t kLocateCooldownMs = 60000;
// 同一個地點的預報重抓也要節流，理由跟上面一樣但觸發點更常見：
// ConfigStore::changed 每一發都會叫 applyConfig()，而**滾輪縮放角色時
// 每一格都會寫 config**（windows/window_manager.cpp 的 applyScale）。
// 抓取失敗期間轉一下滾輪就是一串請求，而且每一發都會 cancel 掉前一發，
// 於是那次抓取永遠完成不了。
constexpr std::int64_t kForecastCooldownMs = 60000;

// 天氣相關請求一律帶自己的 User-Agent。
//
// **Qt 的 QNetworkAccessManager 預設一個 User-Agent 都不送**，而「不像瀏覽器、
// 又不表明身分」正是 Cloudflare 那類 bot 防護會擋下來的請求形狀 —— 第一版用的
// ipapi.co 就是這樣回 403 的（經過寫在 core/weather_http.h）。換掉端點是主要的
// 修法，這一條是另一半：一個會打第三方公開 API 的程式本來就該說自己是誰，
// 被限流時對方也才有辦法只擋我們而不是整段 IP。
QNetworkRequest weatherRequest(const std::string& url) {
  QNetworkRequest request{QUrl(QString::fromStdString(url))};
  const QString version = QCoreApplication::applicationVersion();
  request.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("live2d_mate/%1").arg(version.isEmpty() ? QStringLiteral("dev") : version));
  return request;
}

}  // namespace

WeatherService::WeatherService(HttpJson& http, std::function<WeatherConfig()> getConfig, QObject* parent) : QObject(parent), http_(http), getConfig_(std::move(getConfig)) {
  timer_.setSingleShot(false);
  connect(&timer_, &QTimer::timeout, this, &WeatherService::tick);
}

WeatherService::~WeatherService() {
  // 飛在半路的請求要取消：回呼落地時 this 已經不在了
  if (forecastCall_) forecastCall_->cancel();
  if (locateCall_) locateCall_->cancel();
}

void WeatherService::applyConfig() {
  const WeatherConfig config = getConfig_ ? getConfig_() : WeatherConfig{};

  if (!config.enabled) {
    timer_.stop();
    if (forecastCall_) forecastCall_->cancel();
    if (locateCall_) locateCall_->cancel();
    // cancel() 保證不會再回呼，所以旗標要在這裡自己清
    forecastInFlight_ = false;
    locateInFlight_ = false;
    // 關掉就把資料清乾淨（見標頭第二點）
    if (snapshot_.valid) {
      snapshot_ = WeatherSnapshot{};
      emit snapshotChanged();
    }
    setError({});
    return;
  }

  // **QTimer::setInterval 會重啟執行中的計時器**，所以只有值真的變了才設 ——
  // 無條件設的話，任何比輪詢間隔更頻繁的設定變動（滾輪縮放就是）
  // 都會把下一次輪詢一直往後推，snapshot 靜靜地過期
  if (timer_.interval() != config.pollIntervalMs) timer_.setInterval(config.pollIntervalMs);
  if (!timer_.isActive()) timer_.start();

  // 地點換了（或還沒抓過）就立刻抓一次，不等下一輪 15 分鐘
  const bool locationChanged = config.latitude != fetchedLatitude_ || config.longitude != fetchedLongitude_;
  if (!snapshot_.valid || locationChanged) tick();
}

void WeatherService::tick() {
  const WeatherConfig config = getConfig_ ? getConfig_() : WeatherConfig{};
  if (!config.enabled) return;

  if (!weatherHasLocation(config)) {
    if (config.autoLocate) {
      locateByIp();
    } else if (const auto issue = weatherConfigIssue(config)) {
      setError(*issue);
    }
    return;
  }
  fetchForecast(config.latitude, config.longitude);
}

void WeatherService::locateByIp() {
  if (locateInFlight_) return;
  const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
  if (lastLocateMs_ >= 0 && now - lastLocateMs_ < kLocateCooldownMs) return;
  lastLocateMs_ = now;

  locateInFlight_ = true;
  QPointer<WeatherService> self(this);
  locateCall_ = http_.get(weatherRequest(ipLocationUrl()), kLocateTimeoutMs, [self](HttpJson::Reply reply) {
    if (!self) return;
    self->locateInFlight_ = false;
    if (!reply.ok()) {
      self->setError(reply.transportError.empty() ? "location service returned HTTP " + std::to_string(reply.status) : reply.transportError);
      return;
    }
    std::string error;
    const auto location = parseIpLocation(reply.bodyText(), &error);
    if (!location) {
      self->setError(error);
      return;
    }
    qDebug() << "[weather] IP 粗定位:" << QString::fromStdString(location->name) << location->latitude << location->longitude;
    self->setError({});
    // 寫回 config 是呼叫端的事；config 一改就會回頭呼叫 applyConfig() 抓預報
    emit self->locationResolved(location->latitude, location->longitude, QString::fromStdString(location->name));
  });
}

void WeatherService::fetchForecast(double latitude, double longitude) {
  // 換地點是使用者剛按下「套用」，一定要立刻重抓；同一個地點的重抓才節流
  const bool sameLocation = latitude == fetchedLatitude_ && longitude == fetchedLongitude_;
  const std::int64_t now = QDateTime::currentMSecsSinceEpoch();
  if (sameLocation) {
    if (forecastInFlight_) return;
    if (lastForecastMs_ >= 0 && now - lastForecastMs_ < kForecastCooldownMs) return;
  }

  if (forecastCall_) forecastCall_->cancel();
  fetchedLatitude_ = latitude;
  fetchedLongitude_ = longitude;
  lastForecastMs_ = now;

  forecastInFlight_ = true;
  QPointer<WeatherService> self(this);
  forecastCall_ = http_.get(weatherRequest(buildForecastUrl(latitude, longitude)), kForecastTimeoutMs, [self](HttpJson::Reply reply) {
    if (!self) return;
    self->forecastInFlight_ = false;
    if (!reply.ok() && reply.body.empty()) {
      self->setError(reply.transportError.empty() ? "weather service returned HTTP " + std::to_string(reply.status) : reply.transportError);
      return;
    }
    // 4xx 的 body 是一份帶 reason 的 JSON，值得解出來（parseForecast 認得那個形狀）
    std::string error;
    auto snapshot = parseForecast(reply.bodyText(), &error);
    if (!snapshot) {
      // 非 2xx 又解不出 reason（例如代理伺服器回一頁 HTML）：光說「不是合法 JSON」
      // 會讓人往資料格式的方向查，狀態碼才是線索
      self->setError(reply.ok() ? error : "HTTP " + std::to_string(reply.status) + ": " + error);
      return;
    }
    // 顯示名一律用設定裡那一份：預報 API 不回地名，
    // 而使用者自己填的「家」比 geocoding 回的行政區名有意義
    snapshot->locationName = self->getConfig_ ? self->getConfig_().locationName : std::string();
    self->snapshot_ = std::move(*snapshot);
    self->setError({});
    emit self->snapshotChanged();
  });
}

void WeatherService::resolveCity(const std::string& name, const std::string& language, std::function<void(bool, GeoLocation, std::string)> done) {
  // 這一支刻意不記進 forecastCall_／locateCall_：它由設定頁的按鈕驅動，
  // 生命週期跟輪詢無關，取消輪詢不該把使用者正在等的查詢一起砍掉。
  // 回呼也不需要 this 的護衛 —— 它完全不碰服務，只把結果轉給 done，
  // 而 done 那一邊（設定分頁）自己有 QPointer 護著
  http_.get(weatherRequest(buildGeocodeUrl(name, language)), kGeocodeTimeoutMs, [done](HttpJson::Reply reply) {
    if (!done) return;
    if (!reply.ok()) {
      done(false, {}, reply.transportError.empty() ? "geocoding service returned HTTP " + std::to_string(reply.status) : reply.transportError);
      return;
    }
    std::string error;
    const auto location = parseGeocode(reply.bodyText(), &error);
    if (!location) {
      done(false, {}, error);
      return;
    }
    done(true, *location, {});
  });
}

void WeatherService::setError(std::string error) {
  if (error == lastError_) return;
  if (!error.empty()) qWarning() << "[weather] 取得天氣失敗:" << QString::fromStdString(error);
  lastError_ = std::move(error);
  emit statusChanged();
}

}  // namespace l2m
