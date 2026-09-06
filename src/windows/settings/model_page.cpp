#include "model_page.h"

#include <QApplication>
#include <QDesktopServices>
#include <QFont>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <map>
#include <vector>

#include "../../app/app_controller.h"
#include "core/annotations.h"
#include "core/builtin_actions.h"
#include "core/i18n.h"
#include "core/model_types.h"
#include "ui_model_page.h"

namespace l2m {

namespace {

// 每打一個字就寫一次磁碟太吵，聚合一下
constexpr int kSaveDebounceMs = 400;
// 命名列左欄的固定寬度，讓輸入框對齊
constexpr int kLabelColumnWidth = 220;

std::string meaningOf(const std::map<std::string, std::string>& table, const std::string& key) {
  const auto it = table.find(key);
  return it != table.end() ? it->second : std::string();
}

}  // namespace

ModelPage::ModelPage(const SettingsContext& context, QWidget* parent) : QWidget(parent), ctx_(context), ui_(std::make_unique<Ui::ModelPage>()) {
  ui_->setupUi(this);
  // 捲動區的底色跟著分頁走（settings_context.h 說明了為什麼是關 autoFillBackground）
  blendScrollAreaBackground(ui_->namingScroll);
  // 兩欄比例 1:2。setSizes 不能省 —— 只有 setStretchFactor 的話實際比例會被
  // 右半邊的內容寬度拉走，模型頁與角色頁的清單就對不齊。
  // 種子值與踩過的坑見 settings_context.h 的 kListPaneWidth。
  ui_->splitter->setStretchFactor(0, 1);
  ui_->splitter->setStretchFactor(1, 2);
  ui_->splitter->setSizes({kListPaneWidth, kDetailPaneWidth});

  connect(ui_->modelList, &QListWidget::currentRowChanged, this, [this](int) {
    if (updating_) return;
    // 選取只影響按鈕；真正的切換要按按鈕（會凍住 GUI 執行緒，見標頭註解）
    const std::string selected = selectedModelId();
    const ModelInfo* current = ctx_.controller->currentModel();
    ui_->loadButton->setEnabled(!busy_ && !selected.empty() && (!current || current->id != selected));
  });
  connect(ui_->modelList, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem*) { loadSelected(); });
  connect(ui_->loadButton, &QPushButton::clicked, this, &ModelPage::loadSelected);
  connect(ui_->rescanButton, &QPushButton::clicked, this, [this] {
    ctx_.controller->rescan();
    rebuildModelList();
  });
  connect(ui_->openFolderButton, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(QString::fromStdString(ctx_.controller->modelsDir().u8string()))); });
  connect(ui_->defaultNamesButton, &QPushButton::clicked, this, [this] {
    const CommandResult result = ctx_.controller->applyDefaultNames();
    if (result.ok) {
      // applyDefaultNames 發的 modelsChanged 只會走 refresh()，而 refresh()
      // 在模型沒換時刻意不重建命名區（保護打字中的輸入框）。這裡是按鈕觸發、
      // 沒有輸入焦點要保，強制重建一次，剛填進去的預設名稱才會出現在輸入框裡
      shownModelId_.clear();
      rebuildNaming();
    }
    applySave(result);
  });
  // 「重設」收的是閒置復原那一整包，不只是表情 —— 這裡刻意跟 resetToIdle()
  // 共用，因為「試看」放得出來的東西不是只有表情：打瞌睡（doze）是唯一會循環的
  // 內建動作，清表情碰不到動作管線，停不掉它。而按「試看」的 playMotion() 會先
  // touchIdle() 把兩分鐘的閒置倒數重新給滿，設定視窗開著又沒有 MCP 指令（5 秒的
  // McpQuietReset 沒被武裝），不共用的話 UI 上就完全沒有辦法把它收掉。
  // 補一記 touchIdle()：使用者剛剛真的互動過，別讓閒置表演立刻接上來。
  connect(ui_->resetLookButton, &QPushButton::clicked, this, [this] {
    ctx_.controller->resetToIdle();
    ctx_.controller->touchIdle();
  });

  connect(ctx_.controller, &AppController::modelsChanged, this, &ModelPage::refreshIfActive);
  // 切換是非同步的（先淡出再載入），收尾只能等這個訊號
  // 失敗時把原因彈出來 —— 走的是 ctx_.run 那條「人類版」的錯誤呈現，跟系統匣與
  // MCP 同一組字串。只認這一頁發起的那一次：modelSwitchFinished 每次載入都會來
  //（MCP、系統匣、啟動時的第一次），對著使用者沒按過的載入彈對話框只是騷擾。
  // busy_ 就是「這次是我發起的」，而 finishLoading() 會把它清掉，所以先抄下來。
  connect(ctx_.controller, &AppController::modelSwitchFinished, this, [this](bool ok) {
    const bool mine = busy_;
    finishLoading();
    // 對話框不能在這裡就地開。這條訊號鏈整條都是 direct connection，發源地是
    // CharacterWindow::requestModelLoad() 的 makeCurrent() 與 doneCurrent() **之間**；
    // QMessageBox::exec() 會在那中間開一個巢狀事件迴圈，於是 GL context 一直 current
    // 不放，而且排隊中的 MCP switch_model（QueuedConnection，模態擋得住使用者輸入、
    // 擋不住佇列訊號）會趁機重入還沒返回的 loadPendingModel() —— 正是
    // model_controller.cpp 那段「刻意用 msleep 而不是 processEvents」要避開的東西。
    // 認領要**同步**做（現在就還在 emit 裡），對話框才延後 —— 晚一步的話
    // AppController 會以為沒人管而讓系統匣再通知一次，使用者收到兩份。
    if (!ok && mine) {
      ctx_.controller->markModelLoadFailureReported();
      QTimer::singleShot(0, this, [this] { ctx_.run(ctx_.controller->lastModelLoadResult()); });
    }
  });
}

ModelPage::~ModelPage() = default;

QString ModelPage::tr2(const char* key) const { return QString::fromStdString(i18n::translate(ctx_.controller->uiLocale(), key)); }

std::string ModelPage::selectedModelId() const {
  const QListWidgetItem* item = ui_->modelList->currentItem();
  if (!item) return {};
  return item->data(Qt::UserRole).toString().toStdString();
}

void ModelPage::setBusy(bool busy) {
  busy_ = busy;
  ui_->loadButton->setEnabled(!busy);
  ui_->rescanButton->setEnabled(!busy);
  ui_->modelList->setEnabled(!busy);
}

void ModelPage::loadSelected() {
  if (busy_) return;
  const std::string id = selectedModelId();
  if (id.empty()) return;
  const ModelInfo* current = ctx_.controller->currentModel();
  if (current && current->id == id) return;

  // 切換會先淡出 400 ms 才載入，而載入本身仍然同步把 GUI 執行緒卡住數秒。
  // 直接呼叫的話下面這幾行的畫面更新根本來不及畫出來，使用者看到的是一個
  // 沒有反應的白窗（Windows 會把它畫成灰化的「沒有回應」）—— 沒有舊模型可淡時
  // fadeOutThen 是就地執行的，這一點照舊成立。先讓事件迴圈跑完這一輪把
  //「載入中」畫上去，下一輪才真的切換。
  setBusy(true);
  ctx_.setStatus(tr2("settings.model.loading"));
  QApplication::setOverrideCursor(Qt::WaitCursor);

  QTimer::singleShot(0, this, [this, id] {
    const CommandResult result = ctx_.controller->switchModel(id);
    // 失敗代表載入根本沒開始（找不到模型），不會有 modelSwitchFinished 來收尾
    if (!result.ok) {
      finishLoading();
      ctx_.run(result);
      return;
    }
    // 成功時收尾等 modelSwitchFinished；清單與命名區由 modelsChanged 重建
  });
}

void ModelPage::finishLoading() {
  // setOverrideCursor 與 restoreOverrideCursor 必須成對；busy_ 就是那個配對旗標
  //（modelSwitchFinished 每次載入都會來，不是只有這一頁按下去的那次）
  if (!busy_) return;
  QApplication::restoreOverrideCursor();
  setBusy(false);
}

void ModelPage::retranslate() {
  ui_->loadButton->setText(tr2("settings.model.load"));
  ui_->rescanButton->setText(tr2("settings.model.rescan"));
  ui_->openFolderButton->setText(tr2("settings.model.openFolder"));
  ui_->listHeading->setText(tr2("settings.model.listHeading"));
  ui_->namingHeading->setText(tr2("settings.model.namingHeading"));
  ui_->namingHint->setText(tr2("naming.hint"));
  ui_->defaultNamesButton->setText(tr2("settings.model.useDefaultNames"));
  ui_->resetLookButton->setText(tr2("settings.model.resetLook"));

  QFont headingFont = ui_->listHeading->font();
  headingFont.setBold(true);
  ui_->listHeading->setFont(headingFont);
  ui_->namingHeading->setFont(headingFont);

  // 模型名稱比「動作與表情命名」大一級：它是這一欄的主詞，命名只是動作
  QFont modelFont = headingFont;
  modelFont.setPointSize(modelFont.pointSize() + 4);
  ui_->currentModelName->setFont(modelFont);
  updateCurrentModelName();

  rebuildModelList();
  // 語系換了，命名區的標題與提示文字也得重來
  shownModelId_.clear();
  rebuildNaming();
}

void ModelPage::refresh() {
  rebuildModelList();
  updateCurrentModelName();
  // 存檔本身也會發 modelsChanged。無條件重建會把使用者正在打字的輸入框
  // 連同焦點一起砍掉，所以只在模型真的換了才重建。
  const ModelInfo* model = ctx_.controller->currentModel();
  const std::string id = model ? model->id : std::string();
  if (id == shownModelId_) return;
  rebuildNaming();
}

void ModelPage::refreshIfActive() {
  if (!isVisible()) return;
  refresh();
}

void ModelPage::rebuildModelList() {
  ScopedUpdate guard(updating_);
  const std::string previous = selectedModelId();

  ui_->modelList->clear();
  const ModelInfo* current = ctx_.controller->currentModel();
  for (const auto& model : ctx_.controller->models()) {
    const bool isCurrent = current && current->id == model.id;
    const QString label = QString::fromStdString(model.name) + (isCurrent ? tr2("settings.model.currentSuffix") : QString());
    auto* item = new QListWidgetItem(label, ui_->modelList);
    item->setData(Qt::UserRole, QString::fromStdString(model.id));
    if (isCurrent) {
      QFont font = item->font();
      font.setBold(true);
      item->setFont(font);
    }
  }

  if (ui_->modelList->count() == 0) {
    auto* empty = new QListWidgetItem(tr2("settings.model.empty"), ui_->modelList);
    empty->setFlags(Qt::NoItemFlags);
  }

  // 盡量把選取停在原來那一個（重新掃描之後清單順序可能變）
  const std::string wanted = previous.empty() ? (current ? current->id : std::string()) : previous;
  for (int row = 0; row < ui_->modelList->count(); ++row) {
    if (ui_->modelList->item(row)->data(Qt::UserRole).toString().toStdString() == wanted) {
      ui_->modelList->setCurrentRow(row);
      break;
    }
  }

  const std::string selected = selectedModelId();
  ui_->loadButton->setEnabled(!busy_ && !selected.empty() && (!current || current->id != selected));
  ui_->defaultNamesButton->setEnabled(current != nullptr);
  ui_->resetLookButton->setEnabled(current != nullptr);
}

void ModelPage::updateCurrentModelName() {
  const ModelInfo* model = ctx_.controller->currentModel();
  // 沒有模型時沿用命名區的空狀態字串，兩處講的是同一件事
  ui_->currentModelName->setText(model ? QString::fromStdString(model->name) : tr2("naming.noModel"));
}

QWidget* ModelPage::buildRow(QWidget* parent, const QString& title, const QString& subtitle, const QString& placeholder, const std::string& meaning, std::function<void(const std::string&)> onSave,
                             std::function<void()> onPreview) {
  auto* row = new QWidget(parent);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 4, 0, 4);
  layout->setSpacing(8);

  auto* labels = new QVBoxLayout();
  labels->setSpacing(0);
  auto* titleLabel = new QLabel(title, row);
  QFont titleFont = titleLabel->font();
  titleFont.setBold(true);
  titleLabel->setFont(titleFont);
  labels->addWidget(titleLabel);
  if (!subtitle.isEmpty()) {
    auto* subtitleLabel = new QLabel(subtitle, row);
    subtitleLabel->setEnabled(false);
    labels->addWidget(subtitleLabel);
  }
  auto* labelBox = new QWidget(row);
  labelBox->setLayout(labels);
  labelBox->setMinimumWidth(kLabelColumnWidth);
  layout->addWidget(labelBox);

  auto* edit = new QLineEdit(QString::fromStdString(meaning), row);
  edit->setPlaceholderText(placeholder);
  layout->addWidget(edit, 1);

  // 防抖：計時器掛在輸入框底下，命名區重建時會跟著消失
  auto* debounce = new QTimer(edit);
  debounce->setSingleShot(true);
  debounce->setInterval(kSaveDebounceMs);
  connect(debounce, &QTimer::timeout, this, [edit, onSave] { onSave(edit->text().toStdString()); });
  connect(edit, &QLineEdit::textEdited, debounce, qOverload<>(&QTimer::start));

  auto* preview = new QPushButton(tr2("naming.tryIt"), row);
  connect(preview, &QPushButton::clicked, this, [this, onPreview] {
    ctx_.setStatus(tr2("naming.previewing"));
    onPreview();
  });
  layout->addWidget(preview);

  return row;
}

QWidget* ModelPage::buildPresetRow(QWidget* parent, const QString& title, std::function<void()> onPreview) {
  auto* row = new QWidget(parent);
  auto* layout = new QHBoxLayout(row);
  layout->setContentsMargins(0, 4, 0, 4);
  layout->setSpacing(8);

  // 標籤欄照抄 buildRow 的結構（QWidget + 沒有動過 margin 的 QVBoxLayout）而不是
  // 自己填一個縮排數字：那層 layout 帶著 Qt 依 style 給的預設邊距，直接把 QLabel
  // 放進 QHBoxLayout 會少掉它、名稱就比上面那兩區靠左。同結構才會精確對齊，
  // 換 style 或 DPI 也不會走位。
  auto* labels = new QVBoxLayout();
  labels->setSpacing(0);
  auto* titleLabel = new QLabel(title, row);
  QFont titleFont = titleLabel->font();
  titleFont.setBold(true);
  titleLabel->setFont(titleFont);
  labels->addWidget(titleLabel);
  auto* labelBox = new QWidget(row);
  labelBox->setLayout(labels);
  labelBox->setMinimumWidth(kLabelColumnWidth);
  layout->addWidget(labelBox);
  layout->addStretch(1);

  auto* preview = new QPushButton(tr2("naming.tryIt"), row);
  connect(preview, &QPushButton::clicked, this, [this, onPreview] {
    ctx_.setStatus(tr2("naming.previewing"));
    onPreview();
  });
  layout->addWidget(preview);

  return row;
}

void ModelPage::applySave(const CommandResult& result) {
  if (result.ok) {
    ctx_.setStatus(tr2("naming.saved"));
    return;
  }
  QString message = QString::fromStdString(result.error);
  if (!result.hint.empty()) message += QStringLiteral("　") + QString::fromStdString(result.hint);
  ctx_.setStatus(message);
}

void ModelPage::rebuildNaming() {
  // 整份重建：內容跟著模型走，局部更新只會讓「哪一列對應哪個 key」慢慢對不上
  auto* content = new QWidget();
  namingLayout_ = new QVBoxLayout(content);
  namingLayout_->setContentsMargins(0, 0, 8, 0);
  namingLayout_->setSpacing(2);

  const ModelInfo* model = ctx_.controller->currentModel();
  if (!model) {
    auto* empty = new QLabel(tr2("naming.noModel"), content);
    QFont font = empty->font();
    font.setBold(true);
    empty->setFont(font);
    namingLayout_->addWidget(empty);
    auto* hint = new QLabel(tr2("naming.noModelHint"), content);
    hint->setWordWrap(true);
    namingLayout_->addWidget(hint);
    namingLayout_->addStretch(1);
    ui_->namingScroll->setWidget(content);
    // setWidget 會把內容設回 autoFillBackground(true)，這裡收回來
    blendScrollAreaBackground(ui_->namingScroll);
    shownModelId_.clear();
    return;
  }

  shownModelId_ = model->id;
  const std::string locale = ctx_.controller->uiLocale();
  const ModelAnnotations named = ctx_.controller->currentAnnotations();

  // 內建動作／表情也在 model->motions / model->expressions 裡（掃描時併進去的），
  // 但畫面上要獨立成最下面那一區，所以這兩區的計數與迴圈都要把它們扣掉 ——
  // 不扣的話同一個 wave 會在上下各出現一次。
  const auto isBuiltin = [](const std::vector<std::string>& list, const std::string& name) { return std::find(list.begin(), list.end(), name) != list.end(); };
  const int ownMotionCount = static_cast<int>(model->motions.size()) - static_cast<int>(model->builtinMotions.size());
  const int ownExpressionCount = static_cast<int>(model->expressions.size()) - static_cast<int>(model->builtinExpressions.size());

  // ── 動作 ──
  auto* motionsHeading = new QLabel(QString::fromStdString(i18n::translate(locale, "naming.motionsHeading", i18n::TParams().count(ownMotionCount))), content);
  QFont headingFont = motionsHeading->font();
  headingFont.setPointSize(headingFont.pointSize() + 2);
  headingFont.setBold(true);
  motionsHeading->setFont(headingFont);
  namingLayout_->addWidget(motionsHeading);

  if (ownMotionCount == 0) {
    namingLayout_->addWidget(new QLabel(tr2("naming.noMotions"), content));
  }

  for (const auto& group : model->motions) {
    if (isBuiltin(model->builtinMotions, group.name)) continue;
    const std::string groupKey = motionKey(group.name);
    const QString groupTitle = QString::fromStdString(group.name.empty() ? i18n::translate(locale, "naming.unnamedGroup") : group.name);
    const QString subtitle = QString::fromStdString(i18n::translate(locale, "naming.groupSubtitle", i18n::TParams().count(group.count)));

    namingLayout_->addWidget(buildRow(
      content, groupTitle, subtitle, tr2("naming.motionPlaceholder"), meaningOf(named.motions, groupKey),
      [this, groupKey](const std::string& meaning) { applySave(ctx_.controller->setMeaning(AnnotationKind::Motions, groupKey, meaning)); },
      [this, groupKey] { ctx_.controller->previewMotionKey(groupKey); }));

    // 同一個群組常常塞了好幾段完全不同的動作，逐一列出才有辦法各自命名
    if (group.count <= 1) continue;
    for (int index = 0; index < group.count; ++index) {
      const std::string key = motionKey(group.name, index);
      const QString title = QStringLiteral("　#%1").arg(index);
      const QString file = index < static_cast<int>(group.files.size()) && !group.files[index].empty() ? QString::fromStdString(group.files[index]) : tr2("naming.noFileInfo");

      namingLayout_->addWidget(buildRow(
        content, title, file, tr2("naming.motionPlaceholder"), meaningOf(named.motions, key),
        [this, key](const std::string& meaning) { applySave(ctx_.controller->setMeaning(AnnotationKind::Motions, key, meaning)); }, [this, key] { ctx_.controller->previewMotionKey(key); }));
    }
  }

  // ── 表情 ──
  auto* expressionsHeading = new QLabel(QString::fromStdString(i18n::translate(locale, "naming.expressionsHeading", i18n::TParams().count(ownExpressionCount))), content);
  expressionsHeading->setFont(headingFont);
  namingLayout_->addSpacing(12);
  namingLayout_->addWidget(expressionsHeading);

  if (ownExpressionCount == 0) {
    namingLayout_->addWidget(new QLabel(tr2("naming.noExpressions"), content));
  }

  for (const auto& name : model->expressions) {
    if (isBuiltin(model->builtinExpressions, name)) continue;
    namingLayout_->addWidget(buildRow(
      content, QString::fromStdString(name), QString(), tr2("naming.expressionPlaceholder"), meaningOf(named.expressions, name),
      [this, name](const std::string& meaning) { applySave(ctx_.controller->setMeaning(AnnotationKind::Expressions, name, meaning)); }, [this, name] { ctx_.controller->setExpression(name); }));
  }

  // ── 自定義動作與表情 ──
  buildBuiltinSection(content, *model);

  namingLayout_->addStretch(1);
  ui_->namingScroll->setWidget(content);
  // 同上：每次換內容都要再關一次
  blendScrollAreaBackground(ui_->namingScroll);
}

void ModelPage::buildBuiltinSection(QWidget* content, const ModelInfo& model) {
  // 以 builtinMotions／builtinExpressions 為準，不是直接列 availableBuiltinActions()：
  // 模型自己已經有同名項目時掃描器不會登記，那一個內建就叫不動，列出來只會誤導
  std::vector<BuiltinActionInfo> listed;
  for (const auto& action : availableBuiltinActions(model.parameters)) {
    const auto& registry = action.motion ? model.builtinMotions : model.builtinExpressions;
    if (std::find(registry.begin(), registry.end(), action.name) == registry.end()) continue;
    listed.push_back(action);
  }
  if (listed.empty()) return;

  const std::string locale = ctx_.controller->uiLocale();
  auto* heading = new QLabel(QString::fromStdString(i18n::translate(locale, "naming.builtinHeading", i18n::TParams().count(static_cast<int>(listed.size())))), content);
  QFont headingFont = heading->font();
  headingFont.setPointSize(headingFont.pointSize() + 2);
  headingFont.setBold(true);
  heading->setFont(headingFont);
  namingLayout_->addSpacing(12);
  namingLayout_->addWidget(heading);

  auto* hint = new QLabel(tr2("naming.builtinHint"), content);
  hint->setWordWrap(true);
  hint->setEnabled(false);
  namingLayout_->addWidget(hint);

  for (const auto& action : listed) {
    // 標題用本地化名稱（揮手／點頭…），不是 wave／nod —— 這一區的英文 id 是內部
    // 介面，畫面上要讀得懂。實際對應到的參數移到 tooltip：槽位解析有靠 cdi3 名稱
    // 猜的成分，猜錯時仍然要查得到是哪幾個參數在動，但攤在副標上一列太長。
    const QString title = QString::fromStdString(i18n::translate(locale, "builtin." + action.name));
    QStringList ids;
    for (const auto& id : action.params) ids << QString::fromStdString(id);
    const QString tip = QString::fromStdString(action.name) + (ids.isEmpty() ? QString() : QStringLiteral(" · ") + ids.join(QStringLiteral(", ")));

    QWidget* row = nullptr;
    if (action.motion) {
      const std::string key = motionKey(action.name);
      row = buildPresetRow(content, title, [this, key] { ctx_.controller->previewMotionKey(key); });
    } else {
      const std::string name = action.name;
      row = buildPresetRow(content, title, [this, name] { ctx_.controller->setExpression(name); });
    }
    row->setToolTip(tip);
    namingLayout_->addWidget(row);
  }
}

}  // namespace l2m
