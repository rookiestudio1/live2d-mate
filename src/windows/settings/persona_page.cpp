#include "persona_page.h"

#include <QApplication>
#include <QColor>
#include <QDesktopServices>
#include <QFont>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPalette>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QUrl>

#include <QDialog>
#include <QDialogButtonBox>
#include <QPointer>
#include <QProgressDialog>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <filesystem>

#include "../../app/app_controller.h"
#include "core/i18n.h"
#include "core/persona.h"
#include "core/persona_generate.h"
#include "core/string_util.h"
#include "ui_persona_page.h"

namespace l2m {

namespace {

// 一行一句 → 多行文字（loadIntoEditor 與 applyGeneratedDoc 共用）
QString joinLines(const std::vector<std::string>& lines) {
  QString out;
  for (const auto& line : lines) {
    if (!out.isEmpty()) out += QLatin1Char('\n');
    out += QString::fromStdString(line);
  }
  return out;
}

}  // namespace

PersonaPage::PersonaPage(const SettingsContext& context, LlmDeps llmDeps, QWidget* parent) : QWidget(parent), ctx_(context), llmDeps_(std::move(llmDeps)), ui_(std::make_unique<Ui::PersonaPage>()) {
  ui_->setupUi(this);
  // 捲動區的底色跟著分頁走（settings_context.h 說明了為什麼是關 autoFillBackground）
  blendScrollAreaBackground(ui_->editorScroll);
  // 兩欄比例 1:2。setSizes 不能省 —— 只有 setStretchFactor 的話實際比例會被
  // 右半邊的內容寬度拉走，模型頁與角色頁的清單就對不齊。
  // 種子值與踩過的坑見 settings_context.h 的 kListPaneWidth。
  ui_->splitter->setStretchFactor(0, 1);
  ui_->splitter->setStretchFactor(1, 2);
  ui_->splitter->setSizes({kListPaneWidth, kDetailPaneWidth});

  connect(ui_->personaList, &QListWidget::currentRowChanged, this, [this](int row) {
    if (updating_) return;
    // 有未存的變更就先問；使用者按取消的話把選取跳回去
    if (!confirmDiscardChanges()) {
      ScopedUpdate guard(updating_);
      ui_->personaList->setCurrentRow(previousRow_);
      return;
    }
    previousRow_ = row;
    // 選取只載入，不套用 —— 套用是「使用角色」按鈕
    loadIntoEditor(selectedPersonaName());
    updateButtons();
  });

  // 六個輸入框共用同一條 dirty 邏輯：任一與載入時不同就算改過
  const auto onEdited = [this] {
    if (updating_) return;
    dirty_ = ui_->descriptionEdit->toPlainText() != loadedDescription_ || ui_->linesEdit->toPlainText() != loadedLines_ || ui_->welcomeEdit->toPlainText() != loadedWelcome_ ||
             ui_->greetingsEdit->toPlainText() != loadedGreetings_ || ui_->breaksEdit->toPlainText() != loadedBreaks_ || ui_->pettedEdit->toPlainText() != loadedPetted_ ||
             ui_->weatherAlertsEdit->toPlainText() != loadedWeatherAlerts_;
    updateEditorState();
    updateButtons();
  };
  connect(ui_->descriptionEdit, &QPlainTextEdit::textChanged, this, onEdited);
  connect(ui_->linesEdit, &QPlainTextEdit::textChanged, this, onEdited);
  connect(ui_->welcomeEdit, &QPlainTextEdit::textChanged, this, onEdited);
  connect(ui_->greetingsEdit, &QPlainTextEdit::textChanged, this, onEdited);
  connect(ui_->breaksEdit, &QPlainTextEdit::textChanged, this, onEdited);
  connect(ui_->pettedEdit, &QPlainTextEdit::textChanged, this, onEdited);
  connect(ui_->weatherAlertsEdit, &QPlainTextEdit::textChanged, this, onEdited);

  connect(ui_->useButton, &QPushButton::clicked, this, &PersonaPage::useSelected);
  connect(ui_->clearButton, &QPushButton::clicked, this, &PersonaPage::clearActive);
  connect(ui_->newButton, &QPushButton::clicked, this, &PersonaPage::createNew);
  connect(ui_->saveButton, &QPushButton::clicked, this, &PersonaPage::saveCurrent);
  connect(ui_->deleteButton, &QPushButton::clicked, this, &PersonaPage::deleteSelected);
  connect(ui_->rescanButton, &QPushButton::clicked, this, [this] {
    ctx_.controller->rescanPersonas();
    refresh();
  });
  connect(ui_->openFolderButton, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(ctx_.controller->personasDir().u8string()))); });
  connect(ui_->generateButton, &QPushButton::clicked, this, &PersonaPage::generateWithAi);
  connect(ui_->memoryButton, &QPushButton::clicked, this, &PersonaPage::openMemoryFolder);

  connect(ctx_.controller, &AppController::personaChanged, this, &PersonaPage::refreshIfActive);
}

PersonaPage::~PersonaPage() {
  // 進行中的擴寫要取消：回呼有 QPointer 護衛，但取消掉乾淨得多
  if (generateHandle_) generateHandle_->cancel();
}

QString PersonaPage::tr2(const char* key) const { return QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), key)); }

// ── 清單 ──

std::string PersonaPage::selectedPersonaName() const {
  const QListWidgetItem* item = ui_->personaList->currentItem();
  if (!item) return {};
  return item->data(Qt::UserRole).toString().toStdString();
}

void PersonaPage::selectRowFor(const std::string& name) {
  for (int row = 0; row < ui_->personaList->count(); ++row) {
    if (ui_->personaList->item(row)->data(Qt::UserRole).toString().toStdString() == name) {
      ui_->personaList->setCurrentRow(row);
      return;
    }
  }
}

void PersonaPage::rebuildPersonaList() {
  ScopedUpdate guard(updating_);
  const std::string previous = selectedPersonaName();

  ui_->personaList->clear();
  const std::string active = ctx_.controller->activePersonaName();
  for (const auto& persona : ctx_.controller->personas()) {
    const bool isActive = persona.name == active;
    const QString label = QString::fromStdString(persona.name) + (isActive ? tr2("settings.persona.activeSuffix") : QString());
    auto* item = new QListWidgetItem(label, ui_->personaList);
    item->setData(Qt::UserRole, QString::fromStdString(persona.name));
    if (isActive) {
      QFont font = item->font();
      font.setBold(true);
      item->setFont(font);
    }
  }

  if (ui_->personaList->count() == 0) {
    auto* empty = new QListWidgetItem(tr2("settings.persona.empty"), ui_->personaList);
    empty->setFlags(Qt::NoItemFlags);
  }

  // 盡量把選取停在原來那一份；沒有的話停在套用中的那一份
  selectRowFor(previous.empty() ? active : previous);
  previousRow_ = ui_->personaList->currentRow();
}

void PersonaPage::updateActiveName() {
  const std::string active = ctx_.controller->activePersonaName();
  ui_->activeName->setText(active.empty() ? tr2("settings.persona.noneActive")
                                          : QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), "settings.persona.inUse", i18n::TParams().arg("name", active))));
}

// ── 輸入框 ──

void PersonaPage::loadIntoEditor(const std::string& name) {
  ScopedUpdate guard(updating_);
  const auto text = name.empty() ? std::nullopt : readPersona(ctx_.controller->personasDir(), name);
  // 六個 # 區塊拆進六個框；英文區塊標題只活在檔案裡，畫面上看不到。
  // 載入的內容是正規化後的（台詞區去空行、trim），dirty 比的是同一份基準，
  // 所以剛載入永遠是「沒有變更」。
  const PersonaDoc doc = parsePersonaDoc(text.value_or(std::string()));
  loadedDescription_ = QString::fromStdString(doc.description);
  loadedLines_ = joinLines(doc.lines);
  loadedWelcome_ = joinLines(doc.welcome);
  loadedGreetings_ = joinLines(doc.greetings);
  loadedBreaks_ = joinLines(doc.breaks);
  loadedPetted_ = joinLines(doc.petted);
  loadedWeatherAlerts_ = joinLines(doc.weatherAlerts);
  loadedReserved_ = doc.reserved;
  ui_->descriptionEdit->setPlainText(loadedDescription_);
  ui_->linesEdit->setPlainText(loadedLines_);
  ui_->welcomeEdit->setPlainText(loadedWelcome_);
  ui_->greetingsEdit->setPlainText(loadedGreetings_);
  ui_->breaksEdit->setPlainText(loadedBreaks_);
  ui_->pettedEdit->setPlainText(loadedPetted_);
  ui_->weatherAlertsEdit->setPlainText(loadedWeatherAlerts_);
  for (QPlainTextEdit* edit : {ui_->descriptionEdit, ui_->linesEdit, ui_->welcomeEdit, ui_->greetingsEdit, ui_->breaksEdit, ui_->pettedEdit, ui_->weatherAlertsEdit}) {
    edit->setEnabled(!name.empty());
  }
  shownName_ = name;
  dirty_ = false;
  updateEditorState();
}

PersonaDoc PersonaPage::docFromEditors() const {
  PersonaDoc doc;
  doc.description = ui_->descriptionEdit->toPlainText().toStdString();
  const auto splitToLines = [](const QString& text, std::vector<std::string>& out) {
    for (const QString& line : text.split(QLatin1Char('\n'))) {
      const std::string trimmed = strutil::trim(line.toStdString());
      if (!trimmed.empty()) out.push_back(trimmed);
    }
  };
  splitToLines(ui_->linesEdit->toPlainText(), doc.lines);
  splitToLines(ui_->welcomeEdit->toPlainText(), doc.welcome);
  splitToLines(ui_->greetingsEdit->toPlainText(), doc.greetings);
  splitToLines(ui_->breaksEdit->toPlainText(), doc.breaks);
  splitToLines(ui_->pettedEdit->toPlainText(), doc.petted);
  splitToLines(ui_->weatherAlertsEdit->toPlainText(), doc.weatherAlerts);
  // 唯一沒有輸入框的一份：保留字清單裡還沒有對應欄位的區塊，原樣寫回
  // 才不會被存檔吃掉（六個具名區塊都已經有框，各自從框裡組回來）
  doc.reserved = loadedReserved_;
  return doc;
}

bool PersonaPage::updateCounter(QLabel* counter, QPlainTextEdit* edit, int maxChars) {
  const size_t length = strutil::utf8Length(edit->toPlainText().toStdString());
  counter->setText(
    QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), "settings.persona.charCount", i18n::TParams().arg("count", std::to_string(length)).arg("max", std::to_string(maxChars)))));

  // 超過上限時只是把儲存鈕變灰並把計數器轉紅，**不截斷** ——
  // 打到一半被吃掉字並讓游標亂跳，比讓使用者自己刪還糟。
  // 台詞框另外把「單行超過 kPersonaLineMaxChars」也算超限（氣泡撐不下）。
  bool tooLong = length > static_cast<size_t>(maxChars);
  if (!tooLong && edit != ui_->descriptionEdit) {
    for (const QString& line : edit->toPlainText().split(QLatin1Char('\n'))) {
      if (strutil::utf8Length(strutil::trim(line.toStdString())) > static_cast<size_t>(kPersonaLineMaxChars)) {
        tooLong = true;
        break;
      }
    }
  }
  QPalette palette = counter->palette();
  palette.setColor(QPalette::WindowText, tooLong ? QColor(Qt::red) : QApplication::palette().color(QPalette::WindowText));
  counter->setPalette(palette);
  return tooLong;
}

void PersonaPage::updateEditorState() {
  // 回傳值這裡用不到：六個計數器各自著色，超限與否由 anyTooLong() 現算
  updateCounter(ui_->descriptionCount, ui_->descriptionEdit, kPersonaMaxChars);
  updateCounter(ui_->linesCount, ui_->linesEdit, kPersonaLinesMaxChars);
  updateCounter(ui_->welcomeCount, ui_->welcomeEdit, kPersonaLinesMaxChars);
  updateCounter(ui_->greetingsCount, ui_->greetingsEdit, kPersonaLinesMaxChars);
  updateCounter(ui_->breaksCount, ui_->breaksEdit, kPersonaLinesMaxChars);
  updateCounter(ui_->pettedCount, ui_->pettedEdit, kPersonaLinesMaxChars);
  updateCounter(ui_->weatherAlertsCount, ui_->weatherAlertsEdit, kPersonaLinesMaxChars);
}

bool PersonaPage::anyTooLong() const {
  // 與 writePersona 的 personaDocIssue 同一套規則 —— UI 擋住的正是磁碟層會拒絕的
  return personaDocIssue(docFromEditors()).has_value();
}

void PersonaPage::updateButtons() {
  const std::string selected = selectedPersonaName();
  const std::string active = ctx_.controller->activePersonaName();

  ui_->useButton->setEnabled(!selected.empty() && selected != active);
  // 停用看的是「有沒有套用中的角色」，跟清單上選了誰無關 ——
  // 使用者常常一邊看著別份的內容，一邊想把目前這份關掉
  ui_->clearButton->setEnabled(!active.empty());
  ui_->saveButton->setEnabled(!selected.empty() && dirty_ && !anyTooLong());
  ui_->deleteButton->setEnabled(!selected.empty());
  // 擴寫需要有目標（產出要填回編輯器，存檔時得知道存哪一份）；
  // 生成中維持 disable（回呼回來才恢復）
  ui_->generateButton->setEnabled(!selected.empty() && static_cast<bool>(llmDeps_.chatBuffered) && !generateHandle_);
}

// ── 按鈕 ──

void PersonaPage::saveCurrent() {
  const std::string name = shownName_;
  if (name.empty()) return;

  const CommandResult result = writePersona(ctx_.controller->personasDir(), name, serializePersonaDoc(docFromEditors()));
  if (!result.ok) {
    ctx_.run(result);
    return;
  }

  loadedDescription_ = ui_->descriptionEdit->toPlainText();
  loadedLines_ = ui_->linesEdit->toPlainText();
  loadedWelcome_ = ui_->welcomeEdit->toPlainText();
  loadedGreetings_ = ui_->greetingsEdit->toPlainText();
  loadedBreaks_ = ui_->breaksEdit->toPlainText();
  loadedPetted_ = ui_->pettedEdit->toPlainText();
  loadedWeatherAlerts_ = ui_->weatherAlertsEdit->toPlainText();
  dirty_ = false;
  updateButtons();
  ctx_.setStatus(tr2("settings.saved"));
  // 存的若是套用中的那一份，MCP 的快照要跟著換 —— 不通知的話 AI 還在讀舊的
  ctx_.controller->notifyPersonaEdited();
}

void PersonaPage::useSelected() {
  const std::string name = selectedPersonaName();
  if (name.empty()) return;

  const CommandResult result = ctx_.controller->usePersona(name);
  if (!result.ok) {
    ctx_.run(result);
    return;
  }
  // 已經連線的 client 不會知道 —— MCP 的傳輸推不了 notifications/*
  //（見 persona_page.h 與 core/mcp_resources.h）
  ctx_.setStatus(QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), "settings.persona.applied", i18n::TParams().arg("name", name))));
}

// 停用套用中的角色。usePersona("") 會把 config 的 persona.current 清成 null 並發
// personaChanged，main.cpp 接在上面的 pushPersona 就會推一份空的 PersonaSnapshot 給
// McpHttpServer —— initialize 的 instructions 與 speak／perform 的說明都退回沒有角色的版本。
//
// 刻意不跳確認框，也刻意不動輸入框與選取：.md 檔一個字都沒碰，反悔就再按一次
// 「使用此角色」。理由見 persona_page.h 第 5 點。
void PersonaPage::clearActive() {
  if (ctx_.controller->activePersonaName().empty()) return;

  const CommandResult result = ctx_.controller->usePersona(std::string());
  if (!result.ok) {
    ctx_.run(result);
    return;
  }
  // 跟「使用此角色」同一個提醒：MCP 推不了 notifications/*，已連線的 client
  //（見 persona_page.h 與 core/mcp_resources.h）
  ctx_.setStatus(tr2("settings.persona.cleared"));
}

void PersonaPage::createNew() {
  if (!confirmDiscardChanges()) return;

  bool accepted = false;
  const QString entered = QInputDialog::getText(this, tr2("settings.persona.newTitle"), tr2("settings.persona.newLabel"), QLineEdit::Normal, QString(), &accepted);
  if (!accepted) return;

  const std::string name = entered.toStdString();
  if (const auto issue = personaNameIssue(name)) {
    ctx_.run(CommandResult::failure(tr2("settings.persona.nameInvalid").toStdString(), *issue));
    return;
  }

  const bool exists = std::any_of(ctx_.controller->personas().begin(), ctx_.controller->personas().end(), [&name](const PersonaInfo& p) { return p.name == name; });
  if (exists) {
    ctx_.run(CommandResult::failure(i18n::translate(ctx_.controller->uiLocale(), "settings.persona.nameTaken", i18n::TParams().arg("name", name))));
    return;
  }

  const CommandResult result = writePersona(ctx_.controller->personasDir(), name, std::string());
  if (!result.ok) {
    ctx_.run(result);
    return;
  }

  // rescanPersonas 會發 personaChanged → refresh()，清單重建之後才選得到它
  ctx_.controller->rescanPersonas();
  {
    ScopedUpdate guard(updating_);
    selectRowFor(name);
    previousRow_ = ui_->personaList->currentRow();
  }
  loadIntoEditor(name);
  updateButtons();
  ui_->descriptionEdit->setFocus();
}

void PersonaPage::deleteSelected() {
  const std::string name = selectedPersonaName();
  if (name.empty()) return;

  const QString question = QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), "settings.persona.deleteConfirm", i18n::TParams().arg("name", name)));
  if (QMessageBox::question(this, tr2("settings.persona.delete"), question) != QMessageBox::Yes) {
    return;
  }

  const CommandResult result = deletePersona(ctx_.controller->personasDir(), name);
  if (!result.ok) {
    ctx_.run(result);
    return;
  }

  // 刪掉的若是套用中的那一份，設定裡的值要一起清掉，
  // 不然 activePersonaName() 每次都得靠「檔案還在不在」擋一次
  if (name == ctx_.controller->activePersonaName()) ctx_.controller->usePersona(std::string());

  dirty_ = false;
  shownName_.clear();
  ctx_.controller->rescanPersonas();
}

// ── AI 擴寫與記憶 ──

QString PersonaPage::stageLabel(PersonaGenStage stage) const {
  switch (stage) {
    case PersonaGenStage::Description:
      return tr2("settings.persona.description");
    case PersonaGenStage::Lines:
      return tr2("settings.persona.lines");
    case PersonaGenStage::Welcome:
      return tr2("settings.persona.welcome");
    case PersonaGenStage::Greetings:
      return tr2("settings.persona.greetings");
    case PersonaGenStage::Breaks:
      return tr2("settings.persona.breaks");
    case PersonaGenStage::Petted:
      return tr2("settings.persona.petted");
    case PersonaGenStage::Weather:
      return tr2("settings.persona.weatherAlerts");
  }
  return {};
}

void PersonaPage::generateWithAi() {
  if (!llmDeps_.chatBuffered || shownName_.empty()) return;

  bool accepted = false;
  const QString brief = QInputDialog::getMultiLineText(this, tr2("settings.persona.generateBriefTitle"), tr2("settings.persona.generateBriefLabel"), QString(), &accepted);
  if (!accepted) return;

  PersonaGenerateInput input;
  // 以編輯器現況為底（含還沒存檔的修改）—— 使用者常常先打幾句再請 AI 接手
  input.current = docFromEditors();
  input.brief = brief.toStdString();

  if (generateHandle_) generateHandle_->cancel();
  generateSession_ = std::make_shared<PersonaGenerateSession>(std::move(input));
  ui_->generateButton->setEnabled(false);
  ui_->generateButton->setText(tr2("settings.persona.generating"));

  // 確定進度（第幾階段/共幾階段）：本機推論一跑幾十秒，沒有回饋看起來像當掉。
  // 取消鈕真的會中止請求（cancel 之後保證不再回呼），整條階段鏈跟著停。
  if (generateProgress_) generateProgress_->deleteLater();
  auto* progress = new QProgressDialog(QString(), tr2("settings.persona.generateCancelButton"), 0, generateSession_->stageCount(), this);
  progress->setWindowTitle(tr2("settings.persona.generateBriefTitle"));
  progress->setWindowModality(Qt::WindowModal);
  progress->setMinimumDuration(0);
  progress->setMinimumWidth(360);
  generateProgress_ = progress;
  connect(progress, &QProgressDialog::canceled, this, [this] {
    if (generateHandle_) {
      generateHandle_->cancel();
      generateHandle_.reset();
    }
    generateSession_.reset();
    ui_->generateButton->setText(tr2("settings.persona.generate"));
    updateButtons();
  });

  runGenerateStage();
}

void PersonaPage::finishGenerate() {
  generateHandle_.reset();
  generateSession_.reset();
  if (generateProgress_) {
    // close() 不會發 canceled，取消那條路不會被誤觸
    generateProgress_->close();
    generateProgress_->deleteLater();
  }
  ui_->generateButton->setText(tr2("settings.persona.generate"));
  updateButtons();
}

// LLM 回呼裡想開 modal（QMessageBox / QDialog::exec）一律先繞這裡排到下一輪
// 事件迴圈 —— **直接開會當掉，而且是延後到使用者按下按鈕的那一刻才炸**。
//
// 這個回呼是從 QNetworkReply::readyRead 的訊號槽一路呼上來的
//（postStream 的 readyRead lambda → onChunk → ChatSession::dispatch →
// sink.onDone → 這裡）。modal 的巢狀事件迴圈就開在那條堆疊上，於是：
//  1. 同一個 reply 的 finished 緊接著在巢狀迴圈裡送達，
//     http_json.cpp 的 finished 處理器對它 deleteLater()；
//  2. 那個 deleteLater 是**從巢狀迴圈內部**發出的，QDeferredDeleteEvent 記下的
//     loopLevel+scopeLevel 比迴圈自己高一級 —— 依 Qt 的規則它會**就地被這個
//     巢狀迴圈執行掉**，QNetworkReply 在自己的訊號還在堆疊上時就被釋放；
//  3. 使用者按下「套用到編輯器」、巢狀迴圈退掉、堆疊捲回 Qt6Network 的
//     replyDownloadData()，它下一句 `emit q->downloadProgress(...)` 打的已經是
//     釋放掉的物件 → 0xC0000005。堆疊長相就是
//     qt_static_metacall ← QMetaObject::activate ← QNetworkReply::downloadProgress
//     ← 網路內部 ← QMetaCallEvent::placeMetaCall。
//
// 排到下一輪之後，訊號會先完整退棧、reply 正常被回收，對話框才開起來。
// 中間四個階段沒踩到是因為它們只呼叫 runGenerateStage()，不開巢狀迴圈。
void PersonaPage::modalLater(std::function<void()> fn) { QTimer::singleShot(0, this, std::move(fn)); }

void PersonaPage::runGenerateStage() {
  if (!generateSession_) return;

  if (generateProgress_) {
    generateProgress_->setValue(generateSession_->stageIndex());
    generateProgress_->setLabelText(QStringLiteral("%1 / %2  %3").arg(generateSession_->stageIndex() + 1).arg(generateSession_->stageCount()).arg(stageLabel(generateSession_->currentStage())));
  }

  LlmChatOptions options;
  // 描述階段最長（~1800 字），蓋過 config 預設的 512；其餘階段用不完無妨
  options.maxTokens = 2048;

  QPointer<PersonaPage> self(this);
  std::shared_ptr<PersonaGenerateSession> session = generateSession_;
  std::unique_ptr<LlmRequestHandle> handle = llmDeps_.chatBuffered(session->nextMessages(), options, [self, session](std::string text, std::string error) {
    // 取消或已換一輪：session 對不上就是過期回呼，直接丟
    if (!self || self->generateSession_ != session) return;
    if (!error.empty()) {
      self->finishGenerate();
      self->modalLater([self, error] { self->ctx_.run(CommandResult::failure(error)); });
      return;
    }
    if (const std::string issue = session->accept(text); !issue.empty()) {
      self->finishGenerate();
      self->modalLater([self, issue] { self->ctx_.run(CommandResult::failure("The AI draft could not be used: " + issue, "Try again, or give a shorter direction.")); });
      return;
    }
    if (!session->done()) {
      self->runGenerateStage();  // 同一場對話的下一階段
      return;
    }
    const PersonaDoc doc = session->result();
    self->finishGenerate();
    self->modalLater([self, doc] { self->showGeneratePreview(doc); });
  });
  // **回呼可能已經跑完了**：LLM 沒開（main.cpp 的 chatBuffered 包裝）與設定不齊
  //（HttpLlmEngine::chat 的 request.error）都是**同步**把錯誤送出來的 —— 那是
  // 刻意的（「同步失敗也要回 no-op handle，呼叫端才不必判空」，比照 tts_types.h），
  // 但代價是 handle 回到手上時這一輪其實早就結束、finishGenerate() 也已經把
  // generateHandle_ 清乾淨了。這時候若照收，就留下一個永遠不會再被 reset 的
  // handle，updateButtons() 的 `!generateHandle_` 從此為假 ——
  // 症狀是「LLM 沒開時按一次『AI 擴寫』，看到警告之後按鈕就再也按不下去，
  // 後來把 LLM 打開也沒用，非得重開程式」。
  // 所以只在「這一輪還活著」時才收下 handle：同步失敗那條路上 finishGenerate()
  // 已經把 generateSession_ 清掉，對不上就代表這個 handle 沒有人要等了。
  if (generateSession_ == session) generateHandle_ = std::move(handle);
  updateButtons();
}

void PersonaPage::showGeneratePreview(const PersonaDoc& doc) {
  // 六個區塊的全文先給使用者過目，按「套用到編輯器」才動表單；
  // 存檔仍然是右上那顆「儲存」
  QDialog dialog(this);
  dialog.setWindowTitle(tr2("settings.persona.generatePreviewTitle"));
  auto* layout = new QVBoxLayout(&dialog);
  auto* view = new QPlainTextEdit(QString::fromStdString(serializePersonaDoc(doc)), &dialog);
  view->setReadOnly(true);
  view->setMinimumSize(520, 420);
  layout->addWidget(view);
  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(tr2("settings.persona.generateApply"));
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
  layout->addWidget(buttons);

  if (dialog.exec() == QDialog::Accepted) applyGeneratedDoc(doc);
}

void PersonaPage::applyGeneratedDoc(const PersonaDoc& doc) {
  // 走一般的 setPlainText 路徑（不掛 updating_）：textChanged → dirty →
  // 儲存鈕亮起，跟手打的修改一視同仁
  ui_->descriptionEdit->setPlainText(QString::fromStdString(doc.description));
  ui_->linesEdit->setPlainText(joinLines(doc.lines));
  ui_->welcomeEdit->setPlainText(joinLines(doc.welcome));
  ui_->greetingsEdit->setPlainText(joinLines(doc.greetings));
  ui_->breaksEdit->setPlainText(joinLines(doc.breaks));
  ui_->pettedEdit->setPlainText(joinLines(doc.petted));
  ui_->weatherAlertsEdit->setPlainText(joinLines(doc.weatherAlerts));
  // 還沒有輸入框的保留區塊直接進暫存，存檔時一起寫回
  loadedReserved_ = doc.reserved;
  updateButtons();
  ctx_.setStatus(tr2("settings.persona.generated"));
}

void PersonaPage::openMemoryFolder() {
  // 目錄平常由 main 啟動時建好；這裡再保一次險（使用者可能手動刪過）
  std::error_code ec;
  std::filesystem::create_directories(ctx_.controller->memoryDir(), ec);
  QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(ctx_.controller->memoryDir().u8string())));
}

bool PersonaPage::confirmDiscardChanges() {
  if (!dirty_ || shownName_.empty()) return true;

  const QString question = QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), "settings.persona.unsavedBody", i18n::TParams().arg("name", shownName_)));
  const auto answer = QMessageBox::question(this, tr2("settings.persona.unsavedTitle"), question, QMessageBox::Save | QMessageBox::Discard | QMessageBox::Cancel);

  if (answer == QMessageBox::Cancel) return false;
  if (answer == QMessageBox::Save) {
    const CommandResult result = writePersona(ctx_.controller->personasDir(), shownName_, serializePersonaDoc(docFromEditors()));
    if (!result.ok) {
      // 存不進去（太長、唯讀位置…）就別讓選取跑掉，不然使用者兩邊都沒了
      ctx_.run(result);
      return false;
    }
    ctx_.controller->notifyPersonaEdited();
  }
  dirty_ = false;
  return true;
}

// ── 分頁介面 ──

void PersonaPage::retranslate() {
  ui_->useButton->setText(tr2("settings.persona.use"));
  ui_->clearButton->setText(tr2("settings.persona.clear"));
  ui_->newButton->setText(tr2("settings.persona.new"));
  ui_->saveButton->setText(tr2("settings.persona.save"));
  ui_->deleteButton->setText(tr2("settings.persona.delete"));
  ui_->rescanButton->setText(tr2("settings.persona.rescan"));
  ui_->openFolderButton->setText(tr2("settings.persona.openFolder"));
  ui_->generateButton->setText(tr2("settings.persona.generate"));
  ui_->memoryButton->setText(tr2("settings.persona.openMemory"));
  ui_->listHeading->setText(tr2("settings.persona.listHeading"));
  ui_->editorHint->setText(tr2("settings.persona.hint"));
  ui_->descriptionLabel->setText(tr2("settings.persona.description"));
  ui_->linesLabel->setText(tr2("settings.persona.lines"));
  ui_->welcomeLabel->setText(tr2("settings.persona.welcome"));
  ui_->greetingsLabel->setText(tr2("settings.persona.greetings"));
  ui_->breaksLabel->setText(tr2("settings.persona.breaks"));
  ui_->pettedLabel->setText(tr2("settings.persona.petted"));
  ui_->weatherAlertsLabel->setText(tr2("settings.persona.weatherAlerts"));

  QFont headingFont = ui_->listHeading->font();
  headingFont.setBold(true);
  ui_->listHeading->setFont(headingFont);

  // 「使用中」比提示文字大一級：清單上選取的那一份不一定就是套用中的那一份，
  // 使用者要一眼看出 AI 現在到底在演誰
  QFont activeFont = headingFont;
  activeFont.setPointSize(activeFont.pointSize() + 4);
  ui_->activeName->setFont(activeFont);

  refresh();
}

void PersonaPage::refresh() {
  // 使用者可以把別處抓來的角色卡直接丟進 personas/ —— 一開設定就要看得到。
  // 只有清單真的變了才會回頭發 personaChanged，所以不會繞不完。
  ctx_.controller->rescanPersonas();

  rebuildPersonaList();
  updateActiveName();

  // 存檔本身也會發 personaChanged。無條件重載會把使用者正在打字的內容
  // 連同游標位置一起砍掉，所以只在顯示的那一份真的換了才重載。
  const std::string selected = selectedPersonaName();
  if (selected != shownName_) loadIntoEditor(selected);
  updateEditorState();
  updateButtons();
}

void PersonaPage::refreshIfActive() {
  if (!isVisible()) return;
  refresh();
}

}  // namespace l2m
