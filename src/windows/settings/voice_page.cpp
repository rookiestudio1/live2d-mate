#include "voice_page.h"

#include <QApplication>
#include <QColor>
#include <QComboBox>
#include <QLabel>
#include <QLineEdit>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QTextCursor>
#include <QTimer>

#include <cmath>

#include "../../app/app_controller.h"
#include "core/config_patch.h"
#include "core/config_schema.h"
#include "core/i18n.h"
#include "core/tts_http.h"
#include "core/voice_list.h"
#include "ui_voice_page.h"

namespace l2m {

namespace {

// 滑桿是整數，設定是小數：語速用百分比換算（0.5~2 ↔ 50~200）
constexpr double kSliderScale = 100.0;
// 音量滑桿 0~100% 映射到增益 0~2：50% = 原始音量（增益 1）。
// 顯示仍是滑桿值本身（0~100%），使用者看到的是「音量百分比」不是增益倍率。
constexpr double kVolumeSliderScale = 50.0;
// 停手多久才寫設定。ConfigStore 自己還有 300 ms 的寫檔防抖，這裡擋的是
// 「拖一次滑桿發出幾十次 patch」造成的重複驗證與訊號風暴。
constexpr int kSliderDebounceMs = 300;
// 自訂端點沒有語音清單 API，語音下拉在這個引擎底下要停用（rebuildVoiceCombo）
constexpr const char* kCustomEngineId = "custom";
// 參數樣板裡代表「這裡放要唸的句子」的標記（與 core/tts_http.cpp 同一個字串）
constexpr const char* kTextToken = "${TEXT}";

}  // namespace

VoicePage::VoicePage(const SettingsContext& context, VoiceDeps deps, QWidget* parent) : QWidget(parent), ctx_(context), deps_(std::move(deps)), ui_(std::make_unique<Ui::VoicePage>()) {
  ui_->setupUi(this);

  sliderDebounce_ = new QTimer(this);
  sliderDebounce_->setSingleShot(true);
  sliderDebounce_->setInterval(kSliderDebounceMs);
  connect(sliderDebounce_, &QTimer::timeout, this, &VoicePage::applySliders);

  connect(ui_->engine, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    if (updating_) return;
    const QString id = ui_->engine->currentData().toString();
    if (id.isEmpty()) return;
    // 換引擎一定要同時把 voice 清成 null：語音 id 是引擎專屬的
    if (!ctx_.applyPatch(ttsEnginePatch(id.toStdString()))) {
      refresh();
      return;
    }
    ctx_.controller->applyConfig();
    // 這裡不必回灌自定義語音的表單：不 dirty 的話表單本來就等於設定（回灌是 no-op），
    // dirty 的話那是使用者還沒套用的編輯，切個引擎不該把它沖掉。
    refreshTtsInfo();
  });

  connect(ui_->voice, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    if (updating_) return;
    const QVariant data = ui_->voice->currentData();
    // 第一項是「引擎預設」，userData 為空
    const std::string patch = data.toString().isEmpty() ? nullPatch("tts", "voice") : stringPatch("tts", "voice", data.toString().toStdString());
    if (!ctx_.applyPatch(patch)) {
      refresh();
      return;
    }
    ctx_.controller->applyConfig();
  });

  const auto onSlider = [this](int) {
    updateSliderLabels();
    if (updating_) return;
    sliderDebounce_->start();
  };
  connect(ui_->rate, &QSlider::valueChanged, this, onSlider);
  connect(ui_->volume, &QSlider::valueChanged, this, onSlider);
  // 放開就立刻寫，不必再等防抖
  connect(ui_->rate, &QSlider::sliderReleased, this, &VoicePage::applySliders);
  connect(ui_->volume, &QSlider::sliderReleased, this, &VoicePage::applySliders);

  // 兩顆測試按鈕只差 thinking 一個旗標。「測試思考」走的是 MCP think 工具的同一條路
  //（思考泡泡＋帶殘響的聲音＋嘴巴不動），所以按下去看到與聽到的就是 AI 呼叫
  // think 時的樣子 —— 那三件事湊在一起對不對，只有真的播一次才知道。
  const auto speakSample = [this](bool thinking) {
    // 清空輸入框等於「用預設那句」，不是「唸一段空白」
    QString text = ui_->testText->text().trimmed();
    if (text.isEmpty()) {
      text = sampleDefault_;
      ui_->testText->setText(text);
    }
    AppController::SpeakRequest request;
    request.text = text.toStdString();
    request.thinking = thinking;
    QPointer<VoicePage> self(this);
    ctx_.controller->speak(request, [self](CommandResult result) {
      if (self) self->ctx_.run(result);
    });
  };
  connect(ui_->testButton, &QPushButton::clicked, this, [speakSample] { speakSample(false); });
  connect(ui_->testThinkButton, &QPushButton::clicked, this, [speakSample] { speakSample(true); });
  connect(ui_->stopButton, &QPushButton::clicked, this, [this] { ctx_.controller->stopSpeaking(); });
  connect(ui_->redetectButton, &QPushButton::clicked, this, &VoicePage::redetect);

  // 位址欄位與自訂欄位一樣**不即時寫設定**（理由見標頭的第 5 點）：
  // 編輯只更新「套用」按鈕的亮暗，真正寫進 config 的路徑只有 applyServers 一條。
  const auto onServerEdited = [this] {
    if (updating_) return;
    updateServerApplyState();
  };
  connect(ui_->gptsovitsUrl, &QLineEdit::textChanged, this, onServerEdited);
  connect(ui_->voiceboxUrl, &QLineEdit::textChanged, this, onServerEdited);
  // 打完位址直接按 Enter 就套用 —— 不然使用者很容易以為打完就生效了
  connect(ui_->gptsovitsUrl, &QLineEdit::returnPressed, this, &VoicePage::applyServers);
  connect(ui_->voiceboxUrl, &QLineEdit::returnPressed, this, &VoicePage::applyServers);
  connect(ui_->serverApply, &QPushButton::clicked, this, &VoicePage::applyServers);

  // 自訂欄位**不即時寫設定**（理由見標頭的第 4 點）：編輯只更新紅字與「套用」
  // 按鈕的亮暗，真正寫進 config 的路徑只有 customApply 一條。
  const auto onCustomEdited = [this] {
    if (updating_) return;
    updateCustomApplyState();
  };
  connect(ui_->customUrl, &QLineEdit::textChanged, this, onCustomEdited);
  connect(ui_->customParams, &QPlainTextEdit::textChanged, this, onCustomEdited);
  connect(ui_->customHeaders, &QPlainTextEdit::textChanged, this, onCustomEdited);

  connect(ui_->customMethod, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int) {
    // 方法換了，參數的範例形狀也跟著換（表單 vs JSON）。這一行要在 updating_
    // 的早退之前 —— 程式化選回某一項時 placeholder 也得跟著對。
    updateCustomPlaceholders();
    if (updating_) return;
    updateCustomApplyState();
  });

  connect(ui_->customApply, &QPushButton::clicked, this, &VoicePage::applyCustom);

  connect(ui_->insertTextToken, &QPushButton::clicked, this, [this] {
    // 有選取就直接取代 —— 手打 ${TEXT} 很容易變成 {TEXT}／$TEXT／${Text}，
    // 而那幾種全部都會靜默失效（端點只會收到空字串）
    QTextCursor cursor = ui_->customParams->textCursor();
    cursor.insertText(QString::fromUtf8(kTextToken));
    ui_->customParams->setTextCursor(cursor);
    ui_->customParams->setFocus();
  });

  connect(&ctx_.controller->config(), &ConfigStore::changed, this, &VoicePage::refreshIfActive);
}

VoicePage::~VoicePage() = default;

QString VoicePage::tr2(const char* key) const { return QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), key)); }

void VoicePage::refreshTtsInfo() {
  const std::string engine = ctx_.controller->config().get().tts.engine;
  voiceRequestEngine_ = engine;
  requestedTtsInfo_ = true;

  // QPointer 護衛：清單可能在使用者關掉視窗之後才回來
  QPointer<VoicePage> self(this);
  if (deps_.listEngines) {
    deps_.listEngines([self](std::vector<TtsEngineInfo> engines) {
      if (!self) return;
      self->engines_ = std::move(engines);
      self->rebuildEngineCombo();
    });
  }
  if (deps_.listVoices) {
    deps_.listVoices([self, engine](std::vector<VoiceInfo> voices) {
      if (!self) return;
      // 使用者在等待期間又換了引擎 —— 舊引擎的清單不能蓋掉新的
      if (self->voiceRequestEngine_ != engine) return;
      self->voices_ = std::move(voices);
      self->rebuildVoiceCombo();
    });
  }
}

void VoicePage::redetect() {
  if (deps_.resetVoiceCache) deps_.resetVoiceCache();
  engines_.clear();
  voices_.clear();
  rebuildEngineCombo();
  rebuildVoiceCombo();
  refreshTtsInfo();
}

void VoicePage::rebuildEngineCombo() {
  ScopedUpdate guard(updating_);
  const AppConfig& cfg = ctx_.controller->config().get();

  clearComboItems(ui_->engine);
  if (engines_.empty()) {
    ui_->engine->addItem(tr2("settings.voice.detecting"), QString());
    ui_->engine->setEnabled(false);
    return;
  }

  ui_->engine->setEnabled(true);
  for (const auto& engine : engines_) {
    const char* key = engineLabelKey(engine.id);
    const QString base = key ? tr2(key) : QString::fromStdString(engine.name);
    const QString label = engine.available ? base : base + tr2("settings.voice.unavailableSuffix");
    // 不可用的引擎**照樣選得動**，只在標籤後面標出來（理由見標頭第 6 點）：
    // 偵測只是「現在連不連得上」的快照，使用者常常是先填位址再去開服務。
    ui_->engine->addItem(label, QString::fromStdString(engine.id));
  }

  const int index = ui_->engine->findData(QString::fromStdString(cfg.tts.engine));
  if (index >= 0) ui_->engine->setCurrentIndex(index);
}

void VoicePage::rebuildVoiceCombo() {
  ScopedUpdate guard(updating_);
  const AppConfig& cfg = ctx_.controller->config().get();

  clearComboItems(ui_->voice);
  if (cfg.tts.engine == kCustomEngineId) {
    // 自訂端點沒有語音清單 API，音色是使用者寫在參數樣板裡的。
    // 顯示「沒有可用語音」會讓人以為壞掉了，所以改成明講規則。
    ui_->voice->addItem(tr2("settings.voice.engineDefault"), QString());
    ui_->voice->setEnabled(false);
    ui_->voiceStatus->setText(tr2("settings.voice.custom.voiceNote"));
    return;
  }
  const std::vector<VoiceInfo> shown = orderVoicesForDisplay(voices_, ctx_.controller->uiLocale());

  if (shown.empty()) {
    // 清單空的只有兩種可能：還沒問過（顯示「偵測中」），或是問過了真的沒有。
    // orderVoicesForDisplay 全落空時會退回原順序的前幾個，所以 shown 空就代表 voices_ 空。
    const bool detecting = !requestedTtsInfo_;
    const QString text = detecting ? tr2("settings.voice.detecting") : tr2("settings.voice.noVoices");
    ui_->voice->addItem(text, QString());
    ui_->voice->setEnabled(false);
    ui_->voiceStatus->setText(text);
    return;
  }

  ui_->voice->setEnabled(true);
  ui_->voiceStatus->clear();
  ui_->voice->addItem(tr2("settings.voice.engineDefault"), QString());
  for (const auto& voice : shown) {
    ui_->voice->addItem(QStringLiteral("%1 · %2").arg(QString::fromStdString(voice.locale), QString::fromStdString(voice.name)), QString::fromStdString(voice.id));
  }

  const int index = cfg.tts.voice ? ui_->voice->findData(QString::fromStdString(*cfg.tts.voice)) : 0;
  ui_->voice->setCurrentIndex(index >= 0 ? index : 0);
}

void VoicePage::updateSliderLabels() {
  ui_->rateValue->setText(QStringLiteral("%1x").arg(ui_->rate->value() / kSliderScale, 0, 'g', 3));
  ui_->volumeValue->setText(QStringLiteral("%1%").arg(ui_->volume->value()));
}

void VoicePage::applySliders() {
  sliderDebounce_->stop();
  if (updating_) return;
  const AppConfig& cfg = ctx_.controller->config().get();
  const double rate = ui_->rate->value() / kSliderScale;
  const double volume = ui_->volume->value() / kVolumeSliderScale;

  ConfigPatch patch("tts");
  bool dirty = false;
  if (std::abs(cfg.tts.rate - rate) > 0.0001) {
    patch.number("rate", rate);
    dirty = true;
  }
  if (std::abs(cfg.tts.volume - volume) > 0.0001) {
    patch.number("volume", volume);
    dirty = true;
  }
  if (!dirty) return;
  if (!ctx_.applyPatch(patch.json())) {
    refresh();
    return;
  }
  ctx_.controller->applyConfig();
}

TtsConfig VoicePage::serverFromForm() const {
  // 從 config 的那一份開始複製：presets、timeoutMs 這些沒有 UI 的欄位才不會被
  // 整包 patch 洗掉（ConfigStore::patch 的內層是整個覆蓋）
  TtsConfig tts = ctx_.controller->config().get().tts;
  tts.gptsovits.baseUrl = ui_->gptsovitsUrl->text().trimmed().toStdString();
  tts.voicebox.baseUrl = ui_->voiceboxUrl->text().trimmed().toStdString();
  return tts;
}

bool VoicePage::serverDirty() const { return !ttsLocalServerFieldsEqual(serverFromForm(), ctx_.controller->config().get().tts); }

void VoicePage::reloadServerForm() {
  ScopedUpdate guard(updating_);
  const TtsConfig& tts = ctx_.controller->config().get().tts;
  ui_->gptsovitsUrl->setText(QString::fromStdString(tts.gptsovits.baseUrl));
  ui_->voiceboxUrl->setText(QString::fromStdString(tts.voicebox.baseUrl));
}

void VoicePage::applyServers() {
  if (updating_ || !serverLoaded_) return;
  const TtsConfig next = serverFromForm();
  // 按鈕沒 dirty 就是灰的，理論上進不來；Enter 觸發時還是擋一下
  if (ttsLocalServerFieldsEqual(next, ctx_.controller->config().get().tts)) return;
  if (!ctx_.applyPatch(ttsLocalServerPatch(next))) {
    // 失敗時把表單復原成設定現值（SettingsContext::applyPatch 的契約）
    reloadServerForm();
    updateServerApplyState();
    return;
  }
  ctx_.controller->applyConfig();
  updateServerApplyState();
  // 換了主機，可用性與語音清單都得重問 —— 舊快取會讓「（不可用）」永遠掛著
  redetect();
}

void VoicePage::updateServerApplyState() {
  // serverLoaded_ 也要看：retranslate() 會在第一次 refresh() 之前跑，那時表單
  // 還是空的，「空表單 != 設定」會算成 dirty（同 updateCustomApplyState）
  ui_->serverApply->setEnabled(serverLoaded_ && serverDirty());
}

void VoicePage::rebuildMethodCombo() {
  ScopedUpdate guard(updating_);
  // 選回**目前選的那一項**而不是 config 的值：retranslate() 也會走這裡，
  // 從 config 重讀等於換個語系就把使用者還沒套用的方法選擇洗掉。
  // 第一次進來時 currentData 是空的，由 reloadCustomForm() 負責選到設定的值。
  const QString selected = ui_->customMethod->currentData().toString();

  clearComboItems(ui_->customMethod);
  // userData 就是寫進 config 的字串（config_schema.cpp 的 customTtsMethods 那份 enum）
  ui_->customMethod->addItem(tr2("settings.voice.custom.methodGet"), QStringLiteral("get"));
  ui_->customMethod->addItem(tr2("settings.voice.custom.methodPost"), QStringLiteral("post"));
  ui_->customMethod->addItem(tr2("settings.voice.custom.methodPostJson"), QStringLiteral("post-json"));

  const int index = ui_->customMethod->findData(selected);
  ui_->customMethod->setCurrentIndex(index >= 0 ? index : 0);
}

void VoicePage::reloadCustomForm() {
  ScopedUpdate guard(updating_);
  const CustomTtsConfig& custom = ctx_.controller->config().get().tts.custom;
  ui_->customUrl->setText(QString::fromStdString(custom.url));
  ui_->customParams->setPlainText(QString::fromStdString(custom.params));
  ui_->customHeaders->setPlainText(QString::fromStdString(custom.headers));
  const int index = ui_->customMethod->findData(QString::fromStdString(custom.method));
  ui_->customMethod->setCurrentIndex(index >= 0 ? index : 0);
}

CustomTtsConfig VoicePage::customFromForm() const {
  // 從 config 的那一份開始複製，timeoutMs 這種沒有 UI 的欄位才不會被洗掉 ——
  // ConfigStore::patch 的內層是整個覆蓋，少送一個欄位就會被補回預設值
  CustomTtsConfig custom = ctx_.controller->config().get().tts.custom;
  custom.url = ui_->customUrl->text().toStdString();
  custom.params = ui_->customParams->toPlainText().toStdString();
  custom.headers = ui_->customHeaders->toPlainText().toStdString();
  const QString method = ui_->customMethod->currentData().toString();
  if (!method.isEmpty()) custom.method = method.toStdString();
  return custom;
}

bool VoicePage::customDirty() const { return !customTtsFieldsEqual(customFromForm(), ctx_.controller->config().get().tts.custom); }

void VoicePage::applyCustom() {
  if (updating_ || !customLoaded_) return;
  const CustomTtsConfig next = customFromForm();
  // 按鈕沒 dirty 就是灰的，理論上進不來；鍵盤觸發與程式化呼叫還是擋一下
  if (customTtsFieldsEqual(next, ctx_.controller->config().get().tts.custom)) return;
  if (!ctx_.applyPatch(ttsCustomPatch(next))) {
    // 失敗時把表單復原成設定現值 —— 畫面和實際設定不一致比顯示錯誤更糟
    // （SettingsContext::applyPatch 的契約）
    reloadCustomForm();
    updateCustomApplyState();
    return;
  }
  ctx_.controller->applyConfig();
  // 套用完就不 dirty 了，按鈕變灰
  updateCustomApplyState();
}

void VoicePage::updateCustomHint() {
  // 用輸入框當下的值而不是 config：防抖還沒到期就要給回饋。
  // 錯誤字串直接借合成路徑的那一句，設定頁與「測試語音」失敗訊息才不會兩套說法。
  const std::string error = buildCustomTtsRequest(customFromForm(), "test").error;
  ui_->customHint->setText(error.empty() ? tr2("settings.voice.custom.hint") : QString::fromStdString(error));
  QPalette palette = ui_->customHint->palette();
  palette.setColor(QPalette::WindowText, error.empty() ? QApplication::palette().color(QPalette::WindowText) : QColor(Qt::red));
  ui_->customHint->setPalette(palette);
}

void VoicePage::updateCustomPlaceholders() {
  const bool json = ui_->customMethod->currentData().toString() == QLatin1String("post-json");
  ui_->customParams->setPlaceholderText(tr2(json ? "settings.voice.custom.paramsJsonPlaceholder" : "settings.voice.custom.paramsFormPlaceholder"));
}

void VoicePage::updateCustomApplyState() {
  updateCustomHint();
  // 設定不完整（缺 URL 或 ${TEXT}）不擋套用，只出紅字：使用者常常先貼 URL
  // 再回頭補參數，擋下來只會逼人在一次編輯裡湊齊。
  // customLoaded_ 也要看：retranslate() 會在第一次 refresh() 之前跑，那時表單還是空的，
  // 「空表單 != 設定」會算成 dirty —— 按鈕亮著但 applyCustom() 擋著不動最難解釋。
  ui_->customApply->setEnabled(customLoaded_ && customDirty());
}

void VoicePage::retranslate() {
  ui_->engineLabel->setText(tr2("settings.voice.engine"));
  ui_->voiceLabel->setText(tr2("settings.voice.voice"));
  ui_->rateLabel->setText(tr2("settings.voice.rate"));
  ui_->volumeLabel->setText(tr2("settings.voice.volume"));
  ui_->testTextLabel->setText(tr2("settings.voice.testText"));

  // 換語系時只有「使用者沒動過」才換掉試聽文字；他自己打的句子不能被洗掉
  const QString sample = tr2("settings.voice.sampleText");
  if (ui_->testText->text().isEmpty() || ui_->testText->text() == sampleDefault_) {
    ui_->testText->setText(sample);
  }
  sampleDefault_ = sample;
  ui_->testText->setPlaceholderText(sample);

  ui_->testButton->setText(tr2("settings.voice.test"));
  ui_->testThinkButton->setText(tr2("settings.voice.testThink"));
  ui_->stopButton->setText(tr2("settings.voice.stop"));
  ui_->redetectButton->setText(tr2("settings.voice.redetect"));

  ui_->serverGroup->setTitle(tr2("settings.voice.section.server"));
  // 兩個標籤直接借引擎的顯示名，「叫什麼」只有一個真相來源
  ui_->gptsovitsUrlLabel->setText(tr2("tts.engine.gptsovits"));
  ui_->voiceboxUrlLabel->setText(tr2("tts.engine.voicebox"));
  // 預設位址不進 i18n：URL 每個語系都一樣，抄五份只是多五個會走鐘的地方
  ui_->gptsovitsUrl->setPlaceholderText(QString::fromUtf8(kDefaultGptSovitsUrl));
  ui_->voiceboxUrl->setPlaceholderText(QString::fromUtf8(kDefaultVoiceboxUrl));
  ui_->serverHint->setText(tr2("settings.voice.server.hint"));
  ui_->serverApply->setText(tr2("settings.voice.server.apply"));
  updateServerApplyState();

  ui_->customGroup->setTitle(tr2("settings.voice.section.custom"));
  ui_->customUrlLabel->setText(tr2("settings.voice.custom.url"));
  ui_->customUrl->setPlaceholderText(tr2("settings.voice.custom.urlPlaceholder"));
  ui_->customMethodLabel->setText(tr2("settings.voice.custom.method"));
  ui_->customParamsLabel->setText(tr2("settings.voice.custom.params"));
  ui_->customHeadersLabel->setText(tr2("settings.voice.custom.headers"));
  ui_->customHeaders->setPlaceholderText(tr2("settings.voice.custom.headersPlaceholder"));
  ui_->insertTextToken->setText(tr2("settings.voice.custom.insertToken"));
  ui_->customApply->setText(tr2("settings.voice.custom.apply"));
  rebuildMethodCombo();
  updateCustomPlaceholders();
  updateCustomApplyState();

  rebuildEngineCombo();
  rebuildVoiceCombo();
}

void VoicePage::refresh() {
  {
    ScopedUpdate guard(updating_);
    const AppConfig& cfg = ctx_.controller->config().get();
    ui_->rate->setValue(static_cast<int>(std::lround(cfg.tts.rate * kSliderScale)));
    ui_->volume->setValue(static_cast<int>(std::lround(cfg.tts.volume * kVolumeSliderScale)));
    updateSliderLabels();
  }
  // 位址欄位與自訂欄位一樣**只在第一次載入時**回灌，理由見下一段註解
  if (!serverLoaded_) {
    reloadServerForm();
    serverLoaded_ = true;
  }
  updateServerApplyState();

  rebuildMethodCombo();
  // 自訂欄位**只在第一次載入時**回灌。無條件回灌的話，滑桿、引擎、語音的每一次
  // patch 都會（ConfigStore::changed 是同步的）繞回這裡，把使用者打到一半的內容
  // 沖掉、順便把 QPlainTextEdit 的游標打回開頭。之後表單由使用者掌控，
  // 按「套用」才寫進設定。
  if (!customLoaded_) {
    reloadCustomForm();
    customLoaded_ = true;
  }
  updateCustomPlaceholders();
  updateCustomApplyState();

  rebuildEngineCombo();
  rebuildVoiceCombo();

  // 第一次進這一頁才去問引擎與語音；之後只有換引擎與「重新偵測」會再問
  if (!requestedTtsInfo_) refreshTtsInfo();
}

void VoicePage::refreshIfActive() {
  if (!isVisible()) return;
  // 使用者正在拖滑桿時不要把值抽走
  if (ui_->rate->isSliderDown() || ui_->volume->isSliderDown()) return;
  // 自訂欄位與位址欄位不必再防：refresh() 已經不會回灌它們了
  refresh();
}

}  // namespace l2m
