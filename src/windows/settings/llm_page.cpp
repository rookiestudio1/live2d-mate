#include "llm_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QPointer>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>

#include <cmath>

#include "../../app/app_controller.h"
#include "core/config_patch.h"
#include "core/i18n.h"
#include "core/llm_http.h"
#include "ui_llm_page.h"

namespace l2m {

LlmPage::LlmPage(const SettingsContext& context, LlmDeps deps, QWidget* parent) : QWidget(parent), ctx_(context), deps_(std::move(deps)), ui_(std::make_unique<Ui::LlmPage>()) {
  ui_->setupUi(this);

  // provider 的項目由程式碼填：顯示文字走 i18n，真正的值放 userData
  ui_->provider->addItem(QString(), QStringLiteral("openai"));
  ui_->provider->addItem(QString(), QStringLiteral("anthropic"));

  connect(ui_->provider, &QComboBox::currentIndexChanged, this, [this](int index) {
    if (updating_) return;
    // 先把畫面上的值收進舊 provider 的暫存，再載入新 provider 的
    commitFormToCache();
    formProvider_ = ui_->provider->itemData(index).toString().toStdString();
    {
      ScopedUpdate guard(updating_);
      const bool anthropic = formProvider_ == "anthropic";
      ui_->apiKey->setText(QString::fromStdString(anthropic ? anthropicKey_ : openaiKey_));
      ui_->model->setEditText(QString::fromStdString(anthropic ? anthropicModel_ : openaiModel_));
    }
    applyProviderVisibility();
    updateWarnings();
  });
  connect(ui_->showKey, &QCheckBox::toggled, this, [this](bool shown) { ui_->apiKey->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password); });
  connect(ui_->baseUrl, &QLineEdit::textChanged, this, [this] {
    if (!updating_) updateWarnings();
  });
  connect(ui_->apiKey, &QLineEdit::textChanged, this, [this] {
    if (!updating_) updateWarnings();
  });
  connect(ui_->model, &QComboBox::editTextChanged, this, [this] {
    if (!updating_) updateWarnings();
  });
  connect(ui_->test, &QPushButton::clicked, this, &LlmPage::testConnection);
  connect(ui_->apply, &QPushButton::clicked, this, &LlmPage::apply);

  // 行為區：布林與數值有可靠的提交點，即時套用
  connect(ui_->enabled, &QCheckBox::toggled, this, [this](bool on) {
    if (updating_) return;
    ctx_.applyPatch(boolPatch("llm", "enabled", on));
  });
  connect(ui_->driveIdle, &QCheckBox::toggled, this, [this](bool on) {
    if (updating_) return;
    ctx_.applyPatch(boolPatch("llm", "driveIdle", on));
  });
  connect(ui_->temperature, &QSlider::valueChanged, this, [this](int value) {
    ui_->temperatureValue->setText(QString::number(value / 100.0, 'f', 2));
    if (updating_) return;
    // 滑桿值除以 100 —— 浮點往返誤差是 updating_ 護欄存在的理由（settings_context.h）
    ctx_.applyPatch(numberPatch("llm", "temperature", value / 100.0));
  });
  connect(ui_->cooldown, &QSpinBox::valueChanged, this, [this](int seconds) {
    if (updating_) return;
    ctx_.applyPatch(numberPatch("llm", "idleLlmCooldownMs", seconds * 1000.0));
  });
}

LlmPage::~LlmPage() = default;

QString LlmPage::tr2(const char* key) const { return QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), key)); }

void LlmPage::retranslate() {
  ui_->hint->setText(tr2("llm.hint"));
  ui_->connectionGroup->setTitle(tr2("llm.section.connection"));
  ui_->behaviorGroup->setTitle(tr2("llm.section.behavior"));
  ui_->providerLabel->setText(tr2("llm.field.provider"));
  ui_->provider->setItemText(0, tr2("llm.provider.openai"));
  ui_->provider->setItemText(1, tr2("llm.provider.anthropic"));
  ui_->baseUrlLabel->setText(tr2("llm.field.baseUrl"));
  ui_->baseUrl->setPlaceholderText(QString::fromLatin1(kDefaultLlmBaseUrl));
  ui_->apiKeyLabel->setText(tr2("llm.field.apiKey"));
  ui_->showKey->setText(tr2("llm.key.show"));
  ui_->modelLabel->setText(tr2("llm.field.model"));
  ui_->test->setText(tr2("llm.test"));
  ui_->keyNote->setText(tr2("llm.field.apiKeyNote"));
  ui_->apply->setText(tr2("llm.apply"));
  ui_->enabled->setText(tr2("llm.field.enabled"));
  ui_->driveIdle->setText(tr2("llm.field.driveIdle"));
  ui_->temperatureLabel->setText(tr2("llm.field.temperature"));
  ui_->cooldownLabel->setText(tr2("llm.field.cooldown"));
  ui_->cooldown->setSuffix(tr2("llm.cooldown.suffix"));
  ui_->behaviorHint->setText(tr2("llm.behavior.hint"));
  reloadForm();
}

void LlmPage::reloadForm() {
  ScopedUpdate guard(updating_);
  const LlmConfig& llm = ctx_.controller->config().get().llm;

  openaiKey_ = llm.apiKey;
  openaiModel_ = llm.model;
  anthropicKey_ = llm.anthropicApiKey;
  anthropicModel_ = llm.anthropicModel;
  formProvider_ = llm.provider;

  const int index = ui_->provider->findData(QString::fromStdString(llm.provider));
  ui_->provider->setCurrentIndex(index >= 0 ? index : 0);
  ui_->baseUrl->setText(QString::fromStdString(llm.baseUrl));
  const bool anthropic = formProvider_ == "anthropic";
  ui_->apiKey->setText(QString::fromStdString(anthropic ? anthropicKey_ : openaiKey_));
  ui_->model->setEditText(QString::fromStdString(anthropic ? anthropicModel_ : openaiModel_));

  ui_->enabled->setChecked(llm.enabled);
  ui_->driveIdle->setChecked(llm.driveIdle);
  const int sliderValue = static_cast<int>(std::lround(llm.temperature * 100));
  ui_->temperature->setValue(sliderValue);
  ui_->temperatureValue->setText(QString::number(sliderValue / 100.0, 'f', 2));
  ui_->cooldown->setValue(llm.idleLlmCooldownMs / 1000);

  applyProviderVisibility();
}

void LlmPage::commitFormToCache() {
  const std::string key = ui_->apiKey->text().trimmed().toStdString();
  const std::string model = ui_->model->currentText().trimmed().toStdString();
  if (formProvider_ == "anthropic") {
    anthropicKey_ = key;
    anthropicModel_ = model;
  } else {
    openaiKey_ = key;
    openaiModel_ = model;
  }
}

LlmConfig LlmPage::formConfig() const {
  LlmConfig out = ctx_.controller->config().get().llm;
  out.provider = formProvider_;
  out.baseUrl = ui_->baseUrl->text().trimmed().toStdString();
  out.apiKey = openaiKey_;
  out.model = openaiModel_;
  out.anthropicApiKey = anthropicKey_;
  out.anthropicModel = anthropicModel_;
  return out;
}

void LlmPage::updateWarnings() {
  commitFormToCache();
  const LlmConfig form = formConfig();
  // 紅字與呼叫失敗共用同一句（llmConfigIssue）；設定不完整照樣可以套用，
  // 使用者可能就是想先存一半（tts.custom 的同一條產品規則）
  ui_->warning->setText(QString::fromStdString(llmConfigIssue(form)));
  ui_->apply->setEnabled(!llmFieldsEqual(form, ctx_.controller->config().get().llm));
}

void LlmPage::applyProviderVisibility() {
  // baseUrl 只對 openai 相容協定有意義；Anthropic 的端點是固定的
  const bool anthropic = formProvider_ == "anthropic";
  ui_->baseUrlLabel->setVisible(!anthropic);
  ui_->baseUrl->setVisible(!anthropic);
}

void LlmPage::apply() {
  commitFormToCache();
  // 整包送（llmPatch）：兩層 patch 的內層是整個覆蓋，少送的欄位會被打回預設值
  if (!ctx_.applyPatch(llmPatch(formConfig()))) {
    reloadForm();
  }
  updateWarnings();
}

void LlmPage::testConnection() {
  if (!deps_.listModels) return;
  commitFormToCache();
  const int generation = ++testGeneration_;
  ui_->status->setText(tr2("llm.testing"));

  QPointer<LlmPage> self(this);
  deps_.listModels(formConfig(), [self, generation](std::vector<std::string> models, std::string error) {
    if (!self || generation != self->testGeneration_) return;  // 過期的回覆直接丟
    if (!error.empty()) {
      self->ui_->status->setText(QString::fromStdString(error));
      return;
    }
    {
      ScopedUpdate guard(self->updating_);
      const QString current = self->ui_->model->currentText();
      clearComboItems(self->ui_->model);
      for (const auto& model : models) {
        self->ui_->model->addItem(QString::fromStdString(model));
      }
      // 使用者已經打了名字就保留；空著才自動選第一個
      if (!current.trimmed().isEmpty()) {
        self->ui_->model->setEditText(current);
      } else if (self->ui_->model->count() > 0) {
        self->ui_->model->setCurrentIndex(0);
      }
    }
    self->ui_->status->setText(QString::fromStdString(i18n::translate(self->ctx_.controller->uiLocale(), "llm.test.ok", i18n::TParams().arg("count", std::to_string(models.size())))));
    self->updateWarnings();
  });
}

void LlmPage::refresh() {
  reloadForm();
  updateWarnings();
}

void LlmPage::refreshIfActive() {
  if (!isVisible()) return;
  refresh();
}

}  // namespace l2m
