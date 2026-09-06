#include "settings_window.h"

#include <QLabel>
#include <QMessageBox>
#include <QTabWidget>
#include <QTimer>

#include "../../app/app_controller.h"
#include "about_page.h"
#include "core/i18n.h"
#include "general_page.h"
#include "llm_page.h"
#include "mcp_page.h"
#include "model_page.h"
#include "persona_page.h"
#include "ui_settings_window.h"
#include "voice_page.h"

namespace l2m {

namespace {

// 狀態訊息顯示多久（沿用 NamingWindow 的節奏）
constexpr int kStatusHoldMs = 1500;

}  // namespace

SettingsWindow::SettingsWindow(AppController& controller, McpHttpServer& server, VoiceDeps voiceDeps, BubbleDeps bubbleDeps, LlmDeps llmDeps, WeatherDeps weatherDeps, QWidget* parent)
  : QWidget(parent, Qt::Window), controller_(controller), ui_(std::make_unique<Ui::SettingsWindow>()) {
  ui_->setupUi(this);

  SettingsContext ctx;
  ctx.controller = &controller_;
  ctx.applyPatch = [this](const std::string& json) { return applyPatch(json); };
  ctx.setStatus = [this](const QString& text) { setStatus(text); };
  ctx.run = [this](const CommandResult& result) {
    if (result.ok) return;
    // 系統匣與 MCP 走同一組字串，這是「人類版」的錯誤呈現
    QMessageBox box(QMessageBox::Warning, tr2("dialog.actionFailed.title"), QString::fromStdString(result.error), QMessageBox::Ok, this);
    if (!result.hint.empty()) box.setInformativeText(QString::fromStdString(result.hint));
    box.exec();
  };
  ctx.retranslateAll = [this] { retranslate(); };

  general_ = new GeneralPage(ctx, std::move(bubbleDeps), std::move(weatherDeps), this);
  model_ = new ModelPage(ctx, this);
  // 角色分頁拿的是 llmDeps 的複本（AI 擴寫用 chatBuffered），LLM 分頁拿本尊
  persona_ = new PersonaPage(ctx, llmDeps, this);
  voice_ = new VoicePage(ctx, std::move(voiceDeps), this);
  mcp_ = new McpPage(ctx, server, this);
  llm_ = new LlmPage(ctx, std::move(llmDeps), this);
  about_ = new AboutPage(ctx, this);

  // 加入順序必須與 settingsTabs() 一致 —— settingsTabIndex() 是照那個順序算的
  for (const auto tab : settingsTabs()) {
    ui_->tabs->addTab(pageFor(tab), QString());
  }
  pageLocales_.assign(settingsTabs().size(), std::string());

  connect(ui_->tabs, &QTabWidget::currentChanged, this, [this](int) { refreshCurrentPage(); });
}

SettingsWindow::~SettingsWindow() = default;

QString SettingsWindow::tr2(const char* key) const { return QString::fromStdString(i18n::translate(controller_.uiLocale(), key)); }

QWidget* SettingsWindow::pageFor(SettingsTab tab) const {
  switch (tab) {
    case SettingsTab::General:
      return general_;
    case SettingsTab::Model:
      return model_;
    case SettingsTab::Persona:
      return persona_;
    case SettingsTab::Voice:
      return voice_;
    case SettingsTab::Mcp:
      return mcp_;
    case SettingsTab::Llm:
      return llm_;
    case SettingsTab::About:
    default:
      return about_;
  }
}

void SettingsWindow::open(SettingsTab tab) {
  setWindowTitle(tr2("settings.windowTitle"));
  retranslateTabBar();

  const int index = settingsTabIndex(tab);
  const bool sameTab = ui_->tabs->currentIndex() == index;
  ui_->tabs->setCurrentIndex(index);

  // 分頁內容有 isVisible() 護欄（設定變動很頻繁，藏著的分頁不用跟著重建），
  // 所以一定要先 show 再 refresh —— 反過來的話第一次開窗內容是空的。
  // 這條與舊的 McpWindow::open() 一樣，理由也一樣。
  show();
  // 切到別的分頁時 currentChanged 已經做過了，同一頁才要自己補
  if (sameTab) refreshCurrentPage();
  raise();
  activateWindow();
}

void SettingsWindow::retranslateTabBar() {
  const auto& tabs = settingsTabs();
  for (int i = 0; i < static_cast<int>(tabs.size()); ++i) {
    ui_->tabs->setTabText(i, tr2(settingsTabKey(tabs[i])));
  }
}

void SettingsWindow::refreshVisible() {
  if (!isVisible()) return;
  refreshCurrentPage();
}

void SettingsWindow::retranslate() {
  setWindowTitle(tr2("settings.windowTitle"));
  retranslateTabBar();
  // 分頁的文字等它被切到時再重設（見 refreshCurrentPage）；
  // 這裡只把「已經翻成哪個語系」的記錄清掉。
  pageLocales_.assign(settingsTabs().size(), std::string());
  refreshCurrentPage();
  if (trayRetranslate) trayRetranslate();
}

void SettingsWindow::refreshCurrentPage() {
  const int index = ui_->tabs->currentIndex();
  const auto tab = settingsTabAt(index);
  if (!tab) return;

  // 分頁內容是延遲建立的：MCP 的連線片段與命名區都是上百個 widget，
  // 開窗時五頁全部先建好會明顯變慢。第一次被切到（或語系換掉）才建。
  const std::string locale = controller_.uiLocale();
  const bool needsRetranslate = pageLocales_[static_cast<size_t>(index)] != locale;
  if (needsRetranslate) pageLocales_[static_cast<size_t>(index)] = locale;

  switch (*tab) {
    case SettingsTab::General:
      if (needsRetranslate) general_->retranslate();
      general_->refresh();
      break;
    case SettingsTab::Model:
      if (needsRetranslate) model_->retranslate();
      model_->refresh();
      break;
    case SettingsTab::Persona:
      if (needsRetranslate) persona_->retranslate();
      persona_->refresh();
      break;
    case SettingsTab::Voice:
      if (needsRetranslate) voice_->retranslate();
      voice_->refresh();
      break;
    case SettingsTab::Mcp:
      if (needsRetranslate) mcp_->retranslate();
      mcp_->refresh();
      break;
    case SettingsTab::Llm:
      if (needsRetranslate) llm_->retranslate();
      llm_->refresh();
      break;
    case SettingsTab::About:
      if (needsRetranslate) about_->retranslate();
      about_->refresh();
      break;
  }
}

bool SettingsWindow::applyPatch(const std::string& patchJson) {
  try {
    controller_.config().patch(patchJson);
  } catch (const std::exception& err) {
    // UI 已經用 schema 的範圍限制過控制項，走到這裡代表 config.json 被手改壞了、
    // 或我們的 patch 拼錯了。呼叫端拿到 false 要把控制項復原成設定的現值 ——
    // 畫面和實際設定不一致比顯示錯誤訊息更糟。
    setStatus(QString::fromStdString(i18n::translate(controller_.uiLocale(), "settings.applyFailed", i18n::TParams().arg("error", err.what()))));
    return false;
  }
  setStatus(tr2("settings.saved"));
  return true;
}

void SettingsWindow::setStatus(const QString& text) {
  ui_->status->setText(text);
  QTimer::singleShot(kStatusHoldMs, this, [this, text] {
    if (ui_->status->text() == text) ui_->status->clear();
  });
}

}  // namespace l2m
