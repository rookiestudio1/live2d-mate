#include "general_page.h"

#include <QApplication>
#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QDesktopServices>
#include <QHideEvent>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <utility>

#include "../../app/app_controller.h"
#include "core/config_patch.h"
#include "core/config_schema.h"
#include "core/i18n.h"
#include "core/settings_layout.h"
#include "core/weather_http.h"
#include "ui_general_page.h"

namespace l2m {

namespace {

// 預覽氣泡在最後一次調整之後還撐多久。與狀態列的 1500 ms 取同一個節奏。
constexpr int kBubblePreviewHoldMs = 1500;

}  // namespace

GeneralPage::GeneralPage(const SettingsContext& context, BubbleDeps deps, WeatherDeps weatherDeps, QWidget* parent)
  : QWidget(parent), ctx_(context), deps_(std::move(deps)), weatherDeps_(std::move(weatherDeps)), ui_(std::make_unique<Ui::GeneralPage>()) {
  ui_->setupUi(this);
  // 捲動區的底色跟著分頁走（settings_context.h 說明了為什麼是關 autoFillBackground）
  blendScrollAreaBackground(ui_->scrollArea);
  buildInteractionToggles();

  // 上下限與 schema 的驗證共用 core/config_schema.h 的常數（見標頭註解）
  ui_->bubbleOffsetY->setRange(kMinBubbleOffsetY, kMaxBubbleOffsetY);
  ui_->bubbleOffsetY->setKeyboardTracking(false);

  // 閒置的三個毫秒數以秒顯示（毫秒沒有人調得動），range 綁 schema 的具名常數
  ui_->idleResetSec->setRange(kMinIdleResetMs / 1000, kMaxIdleResetMs / 1000);
  ui_->idlePerformAfterSec->setRange(kMinIdlePerformAfterMs / 1000, kMaxIdlePerformAfterMs / 1000);
  ui_->idlePerformIntervalSec->setRange(kMinIdlePerformIntervalMs / 1000, kMaxIdlePerformIntervalMs / 1000);
  for (QSpinBox* box : {ui_->idleResetSec, ui_->idlePerformAfterSec, ui_->idlePerformIntervalSec}) {
    box->setKeyboardTracking(false);
  }

  // 上下限與 schema 的驗證共用常數，兩邊界限才不會各走各的
  ui_->weatherRainProbability->setRange(kMinWeatherRainProbability, kMaxWeatherRainProbability);
  ui_->weatherLookahead->setRange(kMinWeatherLookaheadMinutes, kMaxWeatherLookaheadMinutes);
  for (QSpinBox* box : {ui_->weatherRainProbability, ui_->weatherLookahead}) box->setKeyboardTracking(false);

  windStrengthDebounce_ = new QTimer(this);
  windStrengthDebounce_->setSingleShot(true);
  windStrengthDebounce_->setInterval(300);
  connect(windStrengthDebounce_, &QTimer::timeout, this, [this] {
    if (!ctx_.applyPatch(numberPatch("wind", "strength", ui_->windStrength->value() / 100.0))) {
      refresh();
    }
  });

  previewHold_ = new QTimer(this);
  previewHold_->setSingleShot(true);
  previewHold_->setInterval(kBubblePreviewHoldMs);
  connect(previewHold_, &QTimer::timeout, this, [this] {
    if (deps_.hidePreview) deps_.hidePreview();
  });

  // 停用硬體加速是 Windows 透明視窗黑底的解法；macOS 沒這個問題，
  // 關掉只會掉到軟體渲染、角色嚴重掉幀，所以不給選。
#ifdef Q_OS_MAC
  ui_->disableHwAccel->setVisible(false);
#endif

  connect(ui_->showCharacter, &QCheckBox::toggled, this, [this](bool value) {
    if (updating_) return;
    ctx_.run(ctx_.controller->setVisible(value));
  });
  connect(ui_->alwaysOnTop, &QCheckBox::toggled, this, [this](bool value) {
    if (updating_) return;
    ctx_.run(ctx_.controller->setAlwaysOnTop(value));
  });
  connect(ui_->openAtLogin, &QCheckBox::toggled, this, [this](bool value) {
    if (updating_) return;
    ctx_.controller->setOpenAtLogin(value);
  });
  connect(ui_->disableHwAccel, &QCheckBox::toggled, this, [this](bool value) {
    if (updating_) return;
    if (!ctx_.applyPatch(boolPatch("app", "disableHardwareAcceleration", value))) {
      refresh();
      return;
    }
    QMessageBox box(QMessageBox::Information, tr2("dialog.restartRequired.title"), tr2(value ? "dialog.restartRequired.hwAccelDisabled" : "dialog.restartRequired.hwAccelEnabled"), QMessageBox::Ok,
                    this);
    box.exec();
  });
  connect(ui_->bubbleOffsetY, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
    if (updating_) return;
    applyBubbleOffset(value);
  });

  // 氣泡陰影三檔（off / hard / soft）。項目文字在 retranslate 重建，data 是穩定 id
  //（core/bubble_shape.h 的 bubbleShadowIds()，也就是 schema 的允許值）
  connect(ui_->bubbleShadow, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    if (updating_) return;
    const QString value = ui_->bubbleShadow->currentData().toString();
    if (value.isEmpty()) return;
    if (!ctx_.applyPatch(stringPatch("tts", "bubbleShadow", value.toStdString()))) {
      refresh();
      return;
    }
    showBubblePreview();
  });

  // 閒置的三個秒數：寫入時乘回毫秒；改完要 applyConfig（裡面的 refreshIdle
  // 讓新的秒數立刻生效，不必等下一輪計時）
  const auto connectIdleSpin = [this](QSpinBox* box, const char* field) {
    connect(box, qOverload<int>(&QSpinBox::valueChanged), this, [this, field](int seconds) {
      if (updating_) return;
      if (!ctx_.applyPatch(numberPatch("idle", field, seconds * 1000.0))) {
        refresh();
        return;
      }
      ctx_.controller->applyConfig();
    });
  };
  connectIdleSpin(ui_->idleResetSec, "resetMs");
  connectIdleSpin(ui_->idlePerformAfterSec, "performAfterMs");
  connectIdleSpin(ui_->idlePerformIntervalSec, "performIntervalMs");

  // 自主台詞三檔（off / bubble / voice）。項目文字在 retranslate 重建，data 是穩定 id
  connect(ui_->autonomySpeech, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    if (updating_) return;
    const QString value = ui_->autonomySpeech->currentData().toString();
    if (value.isEmpty()) return;
    if (!ctx_.applyPatch(stringPatch("autonomy", "speech", value.toStdString()))) {
      refresh();
    }
  });

  // 環境風：開關與方向即時套用；強度走 300 ms 防抖（建構子上方的計時器）。
  // 執行期不需要另外通知 —— ambientWindProvider 每幀讀 config，寫進去就生效。
  connect(ui_->windEnabled, &QCheckBox::toggled, this, [this](bool value) {
    if (updating_) return;
    if (!ctx_.applyPatch(boolPatch("wind", "enabled", value))) refresh();
  });
  connect(ui_->windDirection, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    if (updating_) return;
    const QString value = ui_->windDirection->currentData().toString();
    if (value.isEmpty()) return;
    if (!ctx_.applyPatch(stringPatch("wind", "direction", value.toStdString()))) refresh();
  });
  connect(ui_->windStrength, &QSlider::valueChanged, this, [this](int) {
    if (updating_) return;
    windStrengthDebounce_->start();
  });
  // 天氣：除了地點欄位（要打 geocoding，見標頭）其餘全部即時套用。
  // 每一項改完都要 applyWeatherConfig() —— WeatherService 只在被通知時才重排
  // 輪詢，寫進 config 不會自己生效
  connect(ui_->weatherEnabled, &QCheckBox::toggled, this, [this](bool value) {
    if (updating_) return;
    if (!ctx_.applyPatch(boolPatch("weather", "enabled", value))) refresh();
  });
  connect(ui_->weatherAutoLocate, &QCheckBox::toggled, this, [this](bool value) {
    if (updating_) return;
    if (!ctx_.applyPatch(boolPatch("weather", "autoLocate", value))) refresh();
  });
  connect(ui_->weatherAlertEnabled, &QCheckBox::toggled, this, [this](bool value) {
    if (updating_) return;
    if (!ctx_.applyPatch(boolPatch("weather", "alertEnabled", value))) refresh();
  });
  connect(ui_->weatherUnit, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    if (updating_) return;
    const QString value = ui_->weatherUnit->currentData().toString();
    if (value.isEmpty()) return;
    if (!ctx_.applyPatch(stringPatch("weather", "unit", value.toStdString()))) refresh();
  });
  connect(ui_->weatherRainProbability, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
    if (updating_) return;
    if (!ctx_.applyPatch(numberPatch("weather", "rainProbability", value))) refresh();
  });
  connect(ui_->weatherLookahead, qOverload<int>(&QSpinBox::valueChanged), this, [this](int value) {
    if (updating_) return;
    if (!ctx_.applyPatch(numberPatch("weather", "lookaheadMinutes", value))) refresh();
  });
  connect(ui_->weatherApply, &QPushButton::clicked, this, &GeneralPage::applyWeatherLocation);
  // Enter 等同按下套用：輸入框配按鈕時使用者十次有九次直接敲 Enter
  connect(ui_->weatherLocation, &QLineEdit::returnPressed, this, &GeneralPage::applyWeatherLocation);

  connect(ui_->language, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    if (updating_) return;
    applyLanguage();
  });
  connect(ui_->openModelsFolder, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(ctx_.controller->modelsDir().u8string()))); });
  connect(ui_->openConfig, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(ctx_.controller->config().path().u8string()))); });

  connect(&ctx_.controller->config(), &ConfigStore::changed, this, &GeneralPage::refreshIfActive);
  connect(ctx_.controller, &AppController::stateChanged, this, &GeneralPage::refreshIfActive);
}

GeneralPage::~GeneralPage() = default;

QString GeneralPage::tr2(const char* key) const { return QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), key)); }

void GeneralPage::buildInteractionToggles() {
  // 開關表在 core/settings_layout.h，測試會驗每個 section/field 真的寫得進去、
  // 每個 labelKey 五個語系都翻得出來。這裡只負責把它畫出來。
  // 兩欄排列（先左後右、逐列往下）：開關已經多到單欄會把分頁撐得老長。
  const auto& specs = interactionToggles();
  for (size_t i = 0; i < specs.size(); ++i) {
    const auto& spec = specs[i];
    auto* box = new QCheckBox(ui_->interactionGroup);
    ui_->interactionLayout->addWidget(box, int(i / 2), int(i % 2));
    interactionBoxes_.push_back(box);

    const std::string section = spec.section;
    const std::string field = spec.field;
    connect(box, &QCheckBox::toggled, this, [this, section, field](bool value) {
      if (updating_) return;
      if (!ctx_.applyPatch(boolPatch(section, field, value))) {
        // 沒寫進去就把畫面拉回設定的現值，不能讓兩邊不一致
        refresh();
        return;
      }
      ctx_.controller->applyConfig();
    });
  }
}

void GeneralPage::applyBubbleOffset(int value) {
  if (!ctx_.applyPatch(numberPatch("tts", "bubbleOffsetY", value))) {
    // 沒寫進去就把畫面拉回設定的現值，不能讓兩邊不一致
    refresh();
    return;
  }
  showBubblePreview();
}

void GeneralPage::showBubblePreview() {
  // 這些設定看不到就調不了：氣泡平常只在說話時出現，所以借它顯示一句預覽。
  // 正在說台詞的時候不搶 —— 真的台詞本來就看得到效果，搶過來反而把話蓋掉。
  if (!deps_.showPreview) return;
  if (deps_.speaking && deps_.speaking()) return;
  deps_.showPreview(tr2("settings.general.bubblePreview"));
  previewHold_->start();
}

void GeneralPage::applyWeatherLocation() {
  if (!weatherDeps_.resolveCity || weatherResolving_) return;
  const std::string name = ui_->weatherLocation->text().trimmed().toStdString();
  if (name.empty()) {
    // 清空地點 ＝ 把座標歸零，回到「還沒定位」（自動偵測開著的話下一輪會補上）
    if (!ctx_.applyPatch(weatherLocationPatch(0, 0, ""))) refresh();
    return;
  }

  weatherResolving_ = true;
  ui_->weatherApply->setEnabled(false);
  ctx_.setStatus(tr2("settings.general.weatherResolving"));

  // 回呼隔一趟網路往返才落地，那時分頁可能已經被關掉（或整個視窗解構）
  QPointer<GeneralPage> self(this);
  weatherDeps_.resolveCity(name, [self](bool ok, double latitude, double longitude, std::string resolved, std::string error) {
    if (!self) return;
    self->weatherResolving_ = false;
    if (!ok) {
      self->ctx_.setStatus(QString::fromStdString(error));
      self->refresh();
      return;
    }
    // 三個欄位整包送（見標頭）。顯示名用 geocoding 回的標準寫法 ——
    // 使用者打「taipei」，欄位裡最好變成「Taipei」，那是查到了的證據
    if (!self->ctx_.applyPatch(weatherLocationPatch(latitude, longitude, resolved))) {
      self->refresh();
      return;
    }
    self->ctx_.setStatus(QString::fromStdString(resolved));
    self->refresh();
  });
}

void GeneralPage::refreshWeatherStatus() {
  const AppConfig& cfg = ctx_.controller->config().get();
  QString text;
  bool problem = false;
  if (!cfg.weather.enabled) {
    text.clear();
  } else if (const std::string error = weatherDeps_.lastError ? weatherDeps_.lastError() : std::string(); !error.empty()) {
    text = QString::fromStdString(error);
    problem = true;
  } else if (const auto issue = weatherConfigIssue(cfg.weather)) {
    text = QString::fromStdString(*issue);
    problem = true;
  } else if (weatherDeps_.summary) {
    const std::string summary = weatherDeps_.summary();
    text = summary.empty() ? tr2("settings.general.weatherNoData") : QString::fromStdString(summary);
  }
  ui_->weatherStatus->setText(text);
  // 紅字的用法與角色分頁的字數計數器一致
  QPalette palette = ui_->weatherStatus->palette();
  palette.setColor(QPalette::WindowText, problem ? QColor(Qt::red) : QApplication::palette().color(QPalette::WindowText));
  ui_->weatherStatus->setPalette(palette);
}

void GeneralPage::applyLanguage() {
  const QString value = ui_->language->currentData().toString();
  if (value.isEmpty()) return;
  if (!ctx_.applyPatch(stringPatch("app", "locale", value.toStdString()))) {
    refresh();
    return;
  }
  // 語系是「馬上要看到」的設定：整個視窗與系統匣的文字一起重設，
  // 不必像舊版那樣關掉再開
  ctx_.retranslateAll();
}

void GeneralPage::retranslate() {
  ui_->windowGroup->setTitle(tr2("settings.general.section.window"));
  ui_->showCharacter->setText(tr2("settings.general.showCharacter"));
  ui_->alwaysOnTop->setText(tr2("settings.general.alwaysOnTop"));

  ui_->bubbleGroup->setTitle(tr2("settings.general.section.bubble"));
  ui_->bubbleOffsetLabel->setText(tr2("settings.general.bubbleOffsetY"));
  ui_->bubbleShadowLabel->setText(tr2("settings.general.bubbleShadow"));
  {
    ScopedUpdate guard(updating_);
    const QString selected = ui_->bubbleShadow->currentData().toString();
    clearComboItems(ui_->bubbleShadow);
    ui_->bubbleShadow->addItem(tr2("settings.general.bubbleShadowOff"), QStringLiteral("off"));
    ui_->bubbleShadow->addItem(tr2("settings.general.bubbleShadowHard"), QStringLiteral("hard"));
    ui_->bubbleShadow->addItem(tr2("settings.general.bubbleShadowSoft"), QStringLiteral("soft"));
    const int index = ui_->bubbleShadow->findData(selected);
    if (index >= 0) ui_->bubbleShadow->setCurrentIndex(index);
  }

  ui_->interactionGroup->setTitle(tr2("settings.general.section.interaction"));
  const auto& toggles = interactionToggles();
  for (size_t i = 0; i < interactionBoxes_.size() && i < toggles.size(); ++i) {
    interactionBoxes_[i]->setText(tr2(toggles[i].labelKey));
  }

  ui_->idleGroup->setTitle(tr2("settings.general.section.idle"));
  ui_->idleResetLabel->setText(tr2("settings.general.idleResetSec"));
  ui_->idlePerformAfterLabel->setText(tr2("settings.general.idlePerformAfterSec"));
  ui_->idlePerformIntervalLabel->setText(tr2("settings.general.idlePerformIntervalSec"));
  ui_->autonomySpeechLabel->setText(tr2("settings.general.autonomySpeech"));
  {
    ScopedUpdate guard(updating_);
    const QString selected = ui_->autonomySpeech->currentData().toString();
    clearComboItems(ui_->autonomySpeech);
    ui_->autonomySpeech->addItem(tr2("settings.general.speechOff"), QStringLiteral("off"));
    ui_->autonomySpeech->addItem(tr2("settings.general.speechBubble"), QStringLiteral("bubble"));
    ui_->autonomySpeech->addItem(tr2("settings.general.speechVoice"), QStringLiteral("voice"));
    const int index = ui_->autonomySpeech->findData(selected);
    if (index >= 0) ui_->autonomySpeech->setCurrentIndex(index);
  }

  ui_->windGroup->setTitle(tr2("settings.general.section.wind"));
  ui_->windEnabled->setText(tr2("settings.general.windEnabled"));
  ui_->windDirectionLabel->setText(tr2("settings.general.windDirection"));
  ui_->windStrengthLabel->setText(tr2("settings.general.windStrength"));
  {
    // 方向下拉的項目文字要跟著語系重建；data 是穩定的英文 id
    ScopedUpdate guard(updating_);
    const QString selected = ui_->windDirection->currentData().toString();
    clearComboItems(ui_->windDirection);
    ui_->windDirection->addItem(tr2("settings.general.windLeft"), QStringLiteral("left"));
    ui_->windDirection->addItem(tr2("settings.general.windRight"), QStringLiteral("right"));
    const int index = ui_->windDirection->findData(selected);
    if (index >= 0) ui_->windDirection->setCurrentIndex(index);
  }

  ui_->weatherGroup->setTitle(tr2("settings.general.section.weather"));
  ui_->weatherEnabled->setText(tr2("settings.general.weatherEnabled"));
  ui_->weatherLocationLabel->setText(tr2("settings.general.weatherLocation"));
  ui_->weatherLocation->setPlaceholderText(tr2("settings.general.weatherLocationHint"));
  ui_->weatherApply->setText(tr2("settings.general.weatherApply"));
  ui_->weatherAutoLocate->setText(tr2("settings.general.weatherAutoLocate"));
  ui_->weatherUnitLabel->setText(tr2("settings.general.weatherUnit"));
  ui_->weatherAlertEnabled->setText(tr2("settings.general.weatherAlertEnabled"));
  ui_->weatherRainLabel->setText(tr2("settings.general.weatherRainProbability"));
  ui_->weatherLookaheadLabel->setText(tr2("settings.general.weatherLookahead"));
  ui_->weatherLookahead->setSuffix(tr2("settings.general.weatherLookaheadSuffix"));
  {
    // 單位下拉的項目文字跟著語系重建；data 是穩定的英文 id
    ScopedUpdate guard(updating_);
    const QString selected = ui_->weatherUnit->currentData().toString();
    clearComboItems(ui_->weatherUnit);
    ui_->weatherUnit->addItem(tr2("settings.general.weatherUnitC"), QStringLiteral("c"));
    ui_->weatherUnit->addItem(tr2("settings.general.weatherUnitF"), QStringLiteral("f"));
    const int index = ui_->weatherUnit->findData(selected);
    if (index >= 0) ui_->weatherUnit->setCurrentIndex(index);
  }
  refreshWeatherStatus();

  ui_->systemGroup->setTitle(tr2("settings.general.section.system"));
  ui_->openAtLogin->setText(tr2("settings.general.openAtLogin"));
  ui_->disableHwAccel->setText(tr2("settings.general.disableHwAccel"));
  ui_->languageLabel->setText(tr2("settings.general.language"));
  ui_->openModelsFolder->setText(tr2("settings.general.openModelsFolder"));
  ui_->openConfig->setText(tr2("settings.general.openConfig"));

  // 語言下拉的項目文字也要重建：「跟隨系統」會翻譯，語言名一律用自稱名不翻譯
  ScopedUpdate guard(updating_);
  const QString selected = ui_->language->currentData().toString();
  clearComboItems(ui_->language);
  ui_->language->addItem(tr2("settings.general.languageAuto"), QStringLiteral("auto"));
  for (const auto& locale : supportedLocales()) {
    ui_->language->addItem(QString::fromStdString(i18n::localeAutonyms().at(locale)), QString::fromStdString(locale));
  }
  const int index = ui_->language->findData(selected);
  ui_->language->setCurrentIndex(index >= 0 ? index : 0);
}

void GeneralPage::refresh() {
  ScopedUpdate guard(updating_);
  const AppConfig& cfg = ctx_.controller->config().get();

  ui_->showCharacter->setChecked(ctx_.controller->isVisible());
  ui_->alwaysOnTop->setChecked(cfg.app.alwaysOnTop);
  ui_->openAtLogin->setChecked(cfg.app.openAtLogin);
  ui_->disableHwAccel->setChecked(cfg.app.disableHardwareAcceleration);

  const auto& toggles = interactionToggles();
  for (size_t i = 0; i < interactionBoxes_.size() && i < toggles.size(); ++i) {
    interactionBoxes_[i]->setChecked(toggleValue(cfg, toggles[i]));
  }

  ui_->idleResetSec->setValue(cfg.idle.resetMs / 1000);
  ui_->idlePerformAfterSec->setValue(cfg.idle.performAfterMs / 1000);
  ui_->idlePerformIntervalSec->setValue(cfg.idle.performIntervalMs / 1000);
  // 對應的開關關著時秒數沒有東西可以套，灰掉比留著能改但沒反應好
  ui_->idleResetSec->setEnabled(cfg.idle.autoReset);
  ui_->idleResetLabel->setEnabled(cfg.idle.autoReset);
  ui_->idlePerformAfterSec->setEnabled(cfg.idle.perform);
  ui_->idlePerformAfterLabel->setEnabled(cfg.idle.perform);
  ui_->idlePerformIntervalSec->setEnabled(cfg.idle.perform);
  ui_->idlePerformIntervalLabel->setEnabled(cfg.idle.perform);

  const int speechIndex = ui_->autonomySpeech->findData(QString::fromStdString(cfg.autonomy.speech));
  if (speechIndex >= 0) ui_->autonomySpeech->setCurrentIndex(speechIndex);

  ui_->windEnabled->setChecked(cfg.wind.enabled);
  const int windIndex = ui_->windDirection->findData(QString::fromStdString(cfg.wind.direction));
  if (windIndex >= 0) ui_->windDirection->setCurrentIndex(windIndex);
  ui_->windStrength->setValue(static_cast<int>(std::lround(cfg.wind.strength * 100)));
  ui_->windDirection->setEnabled(cfg.wind.enabled);
  ui_->windDirectionLabel->setEnabled(cfg.wind.enabled);
  ui_->windStrength->setEnabled(cfg.wind.enabled);
  ui_->windStrengthLabel->setEnabled(cfg.wind.enabled);

  ui_->weatherEnabled->setChecked(cfg.weather.enabled);
  ui_->weatherAutoLocate->setChecked(cfg.weather.autoLocate);
  ui_->weatherAlertEnabled->setChecked(cfg.weather.alertEnabled);
  ui_->weatherRainProbability->setValue(cfg.weather.rainProbability);
  ui_->weatherLookahead->setValue(cfg.weather.lookaheadMinutes);
  const int unitIndex = ui_->weatherUnit->findData(QString::fromStdString(cfg.weather.unit));
  if (unitIndex >= 0) ui_->weatherUnit->setCurrentIndex(unitIndex);
  // 地點欄位不是即時套用的，所以只在使用者沒在編輯時才覆寫（避免打到一半被洗掉）
  if (!ui_->weatherLocation->hasFocus()) ui_->weatherLocation->setText(QString::fromStdString(cfg.weather.locationName));
  // 功能關著時整組灰掉，比留著能改但沒反應好（比照閒置與環境風那兩組）
  for (QWidget* widget : {static_cast<QWidget*>(ui_->weatherLocationLabel), static_cast<QWidget*>(ui_->weatherLocation), static_cast<QWidget*>(ui_->weatherAutoLocate),
                          static_cast<QWidget*>(ui_->weatherUnitLabel), static_cast<QWidget*>(ui_->weatherUnit), static_cast<QWidget*>(ui_->weatherAlertEnabled)}) {
    widget->setEnabled(cfg.weather.enabled);
  }
  ui_->weatherApply->setEnabled(cfg.weather.enabled && !weatherResolving_);
  // 門檻只在預警開著時才有東西可以套
  const bool alertOn = cfg.weather.enabled && cfg.weather.alertEnabled;
  for (QWidget* widget : {static_cast<QWidget*>(ui_->weatherRainLabel), static_cast<QWidget*>(ui_->weatherRainProbability), static_cast<QWidget*>(ui_->weatherLookaheadLabel),
                          static_cast<QWidget*>(ui_->weatherLookahead)}) {
    widget->setEnabled(alertOn);
  }
  refreshWeatherStatus();

  ui_->bubbleOffsetY->setValue(cfg.tts.bubbleOffsetY);
  const int shadowIndex = ui_->bubbleShadow->findData(QString::fromStdString(cfg.tts.bubbleShadow));
  if (shadowIndex >= 0) ui_->bubbleShadow->setCurrentIndex(shadowIndex);
  // 氣泡關掉的時候這一組設定沒有東西可以套，灰掉比留著能改但沒反應好
  for (QWidget* widget :
       {static_cast<QWidget*>(ui_->bubbleOffsetY), static_cast<QWidget*>(ui_->bubbleOffsetLabel), static_cast<QWidget*>(ui_->bubbleShadow), static_cast<QWidget*>(ui_->bubbleShadowLabel)}) {
    widget->setEnabled(cfg.tts.showBubble);
  }

  const int index = ui_->language->findData(QString::fromStdString(cfg.app.locale));
  if (index >= 0) ui_->language->setCurrentIndex(index);
}

void GeneralPage::refreshIfActive() {
  if (!isVisible()) return;
  refresh();
}

void GeneralPage::hideEvent(QHideEvent* event) {
  previewHold_->stop();
  if (deps_.hidePreview) deps_.hidePreview();
  QWidget::hideEvent(event);
}

}  // namespace l2m
