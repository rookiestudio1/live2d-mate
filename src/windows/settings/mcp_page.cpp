#include "mcp_page.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFont>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "../../app/app_controller.h"
#include "../../mcp/mcp_http_server.h"
#include "core/config_patch.h"
#include "core/config_schema.h"
#include "core/i18n.h"
#include "core/mcp_host.h"
#include "core/mcp_snippets.h"
#include "core/settings_layout.h"
#include "ui_mcp_page.h"

namespace l2m {

namespace {

// 「已複製」提示顯示多久
constexpr int kCopiedHoldMs = 1500;

// 遞迴清空 layout。takeAt 拿到的若是 sub-layout（複製鈕那排 QHBoxLayout），
// delete 它不會帶走裡面的按鈕 —— 按鈕的 parent 是外層容器 widget，
// 會原地留在舊座標疊在重建後的新內容上，所以得先鑽進去把 widget 都刪掉。
void clearLayoutRecursive(QLayout* layout) {
  QLayoutItem* item = nullptr;
  while ((item = layout->takeAt(0)) != nullptr) {
    if (item->widget()) item->widget()->deleteLater();
    if (item->layout()) clearLayoutRecursive(item->layout());
    delete item;
  }
}

const char* groupKey(McpSnippetGroup group) {
  switch (group) {
    case McpSnippetGroup::Direct:
      return "mcp.group.direct";
    case McpSnippetGroup::Stdio:
      return "mcp.group.stdio";
    case McpSnippetGroup::Tunnel:
      return "mcp.group.tunnel";
    case McpSnippetGroup::Note:
    default:
      return "mcp.group.note";
  }
}

}  // namespace

McpPage::McpPage(const SettingsContext& context, McpHttpServer& server, QWidget* parent) : QWidget(parent), ctx_(context), server_(server), ui_(std::make_unique<Ui::McpPage>()) {
  ui_->setupUi(this);
  // 捲動區的底色跟著分頁走（settings_context.h 說明了為什麼是關 autoFillBackground）
  blendScrollAreaBackground(ui_->scrollArea);

  connect(ui_->showToken, &QCheckBox::toggled, this, [this](bool shown) { ui_->token->setEchoMode(shown ? QLineEdit::Normal : QLineEdit::Password); });
  connect(ui_->generate, &QPushButton::clicked, this, [this] {
    ui_->token->setText(QString::fromStdString(generateToken()));
    updateWarnings();
  });
  connect(ui_->token, &QLineEdit::textChanged, this, [this] { updateWarnings(); });
  connect(ui_->host, &QComboBox::currentTextChanged, this, [this] { updateWarnings(); });
  connect(ui_->apply, &QPushButton::clicked, this, &McpPage::apply);

  // 發話節奏是**即時套用**的，不進 apply()（理由見標頭）。
  // section/field 從 mcpToggles() 取，不在這裡寫死 —— 打錯的話只會靜默失效，
  // 而測試驗的是那張表。
  {
    const auto& specs = mcpToggles();
    const auto boxes = speechBoxes();
    for (size_t i = 0; i < boxes.size() && i < specs.size(); ++i) {
      const std::string section = specs[i].section;
      const std::string field = specs[i].field;
      connect(boxes[i], &QCheckBox::toggled, this, [this, section, field](bool value) {
        if (updating_) return;
        if (!ctx_.applyPatch(boolPatch(section, field, value))) {
          // 沒寫進去就把畫面拉回設定的現值，不能讓兩邊不一致
          reloadForm();
        }
      });
    }
  }

  // 伺服器狀態是 direct connection 發過來的，接收端會回頭呼叫 server_.status()。
  // 之所以不會重入 McpHttpServer 的非遞迴 mutex，是因為 setStatus() 刻意在
  // 解鎖之後才 emit（見 mcp/mcp_http_server.h）。新增任何 statusChanged 的
  // 接收端時都要重新確認這條還成立。
  connect(&server_, &McpHttpServer::statusChanged, this, &McpPage::refreshIfActive);
}

McpPage::~McpPage() = default;

QString McpPage::tr2(const char* key) const { return QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), key)); }

std::vector<QCheckBox*> McpPage::speechBoxes() const { return {ui_->talkative, ui_->notifyOnComplete, ui_->speakNoWait, ui_->announceSteps}; }

std::string McpPage::selectedHost() const {
  // 下拉的顯示文字帶著說明（"127.0.0.1 (this machine only)"），
  // 真正的值放在 userData 裡；使用者自己打字時就直接用文字。
  const int index = ui_->host->findText(ui_->host->currentText());
  if (index >= 0) {
    const QVariant data = ui_->host->itemData(index);
    if (data.isValid()) return data.toString().toStdString();
  }
  return ui_->host->currentText().trimmed().toStdString();
}

void McpPage::retranslate() {
  ui_->hint->setText(tr2("mcp.hint"));
  ui_->listenGroup->setTitle(tr2("mcp.section.listen"));
  ui_->speechGroup->setTitle(tr2("mcp.section.speech"));
  ui_->clientsGroup->setTitle(tr2("mcp.section.clients"));
  ui_->enabled->setText(tr2("mcp.field.enabled"));
  ui_->hostLabel->setText(tr2("mcp.field.host"));
  ui_->portLabel->setText(tr2("mcp.field.port"));
  ui_->tokenLabel->setText(tr2("mcp.field.token"));
  ui_->token->setPlaceholderText(tr2("mcp.field.tokenPlaceholder"));
  ui_->showToken->setText(tr2("mcp.token.show"));
  ui_->generate->setText(tr2("mcp.token.generate"));
  ui_->applyHint->setText(tr2("settings.mcp.applyHint"));
  ui_->apply->setText(tr2("mcp.apply"));

  {
    const auto& specs = mcpToggles();
    const auto boxes = speechBoxes();
    for (size_t i = 0; i < boxes.size() && i < specs.size(); ++i) {
      boxes[i]->setText(tr2(specs[i].labelKey));
    }
  }
  ui_->speechHint->setText(tr2("settings.mcp.speechHint"));

  reloadForm();
}

void McpPage::reloadForm() {
  // 表單只在「切到這一頁」與「套用之後」重讀 —— 不掛 ConfigStore::changed，
  // 否則使用者打到一半的 token 會被別處的設定變更沖掉。
  ScopedUpdate guard(updating_);
  const AppConfig& cfg = ctx_.controller->config().get();

  ui_->enabled->setChecked(cfg.mcp.enabled);
  ui_->port->setValue(cfg.mcp.port);

  {
    const auto& specs = mcpToggles();
    const auto boxes = speechBoxes();
    for (size_t i = 0; i < boxes.size() && i < specs.size(); ++i) {
      boxes[i]->setChecked(toggleValue(cfg, specs[i]));
    }
  }

  ui_->token->setText(cfg.mcp.token.has_value() ? QString::fromStdString(*cfg.mcp.token) : QString());

  clearComboItems(ui_->host);
  for (const auto& option : listHostOptions(systemNetworkAddresses())) {
    QString label;
    switch (option.kind) {
      case McpHostKind::Loopback:
        label = tr2("mcp.host.loopback");
        break;
      case McpHostKind::All:
        label = tr2("mcp.host.all");
        break;
      case McpHostKind::Interface:
      default:
        label = QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), "mcp.host.interface", i18n::TParams().arg("address", option.value).arg("name", option.label)));
        break;
    }
    ui_->host->addItem(label, QString::fromStdString(option.value));
  }
  // 設定裡的位址可能不在清單上（網卡拔掉了），那就自己補一個
  int index = ui_->host->findData(QString::fromStdString(cfg.mcp.host));
  if (index < 0) {
    ui_->host->addItem(QString::fromStdString(cfg.mcp.host), QString::fromStdString(cfg.mcp.host));
    index = ui_->host->count() - 1;
  }
  ui_->host->setCurrentIndex(index);
}

void McpPage::updateWarnings() {
  const std::string host = selectedHost();
  const QString token = ui_->token->text().trimmed();

  if (const auto refusal = validateMcpBinding(host, token.isEmpty() ? std::nullopt : std::optional<std::string>(token.toStdString()))) {
    ui_->warning->setText(tr2("mcp.warn.tokenRequired"));
    ui_->apply->setEnabled(false);
    return;
  }

  ui_->apply->setEnabled(true);
  if (requiresToken(host)) {
    ui_->warning->setText(tr2("mcp.warn.exposed") + QStringLiteral("\n") + tr2("mcp.warn.firewall"));
  } else {
    ui_->warning->clear();
  }
}

void McpPage::updateStatus() {
  const AppConfig& cfg = ctx_.controller->config().get();
  if (!cfg.mcp.enabled) {
    ui_->status->setText(tr2("mcp.status.disabled"));
    return;
  }

  const McpHttpServer::Status state = server_.status();
  if (state.running) {
    ui_->status->setText(QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), "mcp.status.running", i18n::TParams().arg("url", state.url))));
  } else if (!state.error.empty()) {
    ui_->status->setText(QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), "mcp.status.error", i18n::TParams().arg("error", state.error))));
  } else {
    ui_->status->setText(tr2("mcp.status.stopped"));
  }
}

void McpPage::rebuildSnippets() {
  // 整份重建：位址或 token 一變，每個片段的內容都要跟著換。
  // 片段直接掛在 clientsGroup 的 layout 上（那一區沒有自己的捲動區了，
  // 群組跟著內容長高，捲動交給整頁那一個）
  QVBoxLayout* snippetsLayout = ui_->clientsLayout;
  if (!snippetsLayout) return;
  clearLayoutRecursive(snippetsLayout);

  const AppConfig& cfg = ctx_.controller->config().get();
  SnippetInput input;
  // 連線指令不能寫 0.0.0.0 —— 那是「綁在哪」不是「連到哪」
  input.host = resolveAdvertisedHost(cfg.mcp.host, systemNetworkAddresses());
  input.port = cfg.mcp.port;
  input.token = cfg.mcp.token;
  input.exePath = QApplication::applicationFilePath().toStdString();

  QWidget* host = ui_->clientsGroup;
  McpSnippetGroup lastGroup = McpSnippetGroup::Note;
  bool first = true;
  for (const auto& snippet : buildMcpSnippets(input)) {
    if (first || snippet.group != lastGroup) {
      auto* heading = new QLabel(tr2(groupKey(snippet.group)), host);
      QFont font = heading->font();
      font.setBold(true);
      heading->setFont(font);
      snippetsLayout->addSpacing(first ? 0 : 10);
      snippetsLayout->addWidget(heading);
      lastGroup = snippet.group;
      first = false;
    }

    auto* label = new QLabel(tr2(snippet.labelKey.c_str()), host);
    label->setWordWrap(true);
    snippetsLayout->addWidget(label);

    if (!snippet.hintKey.empty()) {
      auto* hint = new QLabel(tr2(snippet.hintKey.c_str()), host);
      hint->setWordWrap(true);
      hint->setEnabled(false);
      snippetsLayout->addWidget(hint);
    }

    if (snippet.text.has_value()) {
      auto* box = new QPlainTextEdit(QString::fromStdString(*snippet.text), host);
      box->setReadOnly(true);
      // 內容通常 3~8 行，固定一個高度比讓它撐滿好讀
      box->setFixedHeight(snippet.text->find('\n') == std::string::npos ? 48 : 140);
      snippetsLayout->addWidget(box);

      auto* copy = new QPushButton(tr2("mcp.copy"), host);
      const QString content = QString::fromStdString(*snippet.text);
      connect(copy, &QPushButton::clicked, this, [this, copy, content] {
        QApplication::clipboard()->setText(content);
        copy->setText(tr2("mcp.copied"));
        QTimer::singleShot(kCopiedHoldMs, copy, [this, copy] { copy->setText(tr2("mcp.copy")); });
      });
      auto* row = new QHBoxLayout();
      row->addStretch(1);
      row->addWidget(copy);
      snippetsLayout->addLayout(row);
    }

    if (snippet.docUrl.has_value()) {
      auto* link = new QPushButton(tr2("mcp.docLink"), host);
      const QString url = QString::fromStdString(*snippet.docUrl);
      connect(link, &QPushButton::clicked, this, [url] {
        // 只放行 https，避免設定檔被改成 file:// 之類的東西
        if (url.startsWith(QStringLiteral("https://"))) QDesktopServices::openUrl(QUrl(url));
      });
      auto* row = new QHBoxLayout();
      row->addStretch(1);
      row->addWidget(link);
      snippetsLayout->addLayout(row);
    }
  }

  snippetsLayout->addStretch(1);
}

void McpPage::refresh() {
  reloadForm();
  updateWarnings();
  updateStatus();
  rebuildSnippets();
}

void McpPage::refreshIfActive() {
  if (!isVisible()) return;
  // 伺服器狀態變了只更新狀態列與片段區，不碰表單 ——
  // 使用者可能正在打 token
  updateStatus();
  rebuildSnippets();
}

void McpPage::apply() {
  const std::string host = selectedHost();
  const QString token = ui_->token->text().trimmed();
  const std::optional<std::string> tokenValue = token.isEmpty() ? std::nullopt : std::optional<std::string>(token.toStdString());

  // UI 已經擋過一次，這裡再確認一次；伺服器啟動時還會擋第三次
  if (const auto refusal = validateMcpBinding(host, tokenValue)) {
    ui_->warning->setText(tr2("mcp.warn.tokenRequired"));
    return;
  }

  ui_->status->setText(tr2("mcp.applying"));
  // 四個欄位一起送：分開送會讓伺服器以「新 port ＋ 舊 token」之類的
  // 半套設定先重啟一次
  if (!ctx_.applyPatch(mcpPatch(ui_->enabled->isChecked(), host, ui_->port->value(), tokenValue))) {
    reloadForm();
    updateStatus();
    return;
  }

  const AppConfig& cfg = ctx_.controller->config().get();
  if (cfg.mcp.enabled) {
    server_.start(cfg.mcp.host, cfg.mcp.port, cfg.mcp.token);
  } else {
    server_.stop();
  }
  updateStatus();
  rebuildSnippets();
}

}  // namespace l2m
