#include "viewer_window.h"

#include <QAction>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QGuiApplication>
#include <QIcon>
#include <QLabel>
#include <QListWidget>
#include <QLocalSocket>
#include <QLocale>
#include <QMimeData>
#include <QProcess>
#include <QRandomGenerator>
#include <QRect>
#include <QScreen>
#include <QShortcut>
#include <QStandardPaths>
#include <QStatusBar>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include "collapsible_splitter.h"
#include "core/builtin_actions.h"
#include "core/external_model.h"
#include "core/i18n.h"
#include "core/interaction_logic.h"
#include "core/json_doc.h"
#include "core/model_scanner.h"
#include "core/string_util.h"
#include "core/viewer_settings.h"
#include "core/tray_label.h"
#include "live2d/action_player.h"
#include "live2d/model_controller.h"
#include "tinted_icon.h"
#include "viewer_canvas.h"

namespace {

// 動作項目在 QTreeWidgetItem 上帶的兩份資料：群組名，以及群組內的索引
// （-1 ＝ 整個群組，播的時候隨機挑一段）。
constexpr int kRoleGroup = Qt::UserRole;
constexpr int kRoleIndex = Qt::UserRole + 1;

// 命名檔裡有寫意義就用意義當主標籤（原名退到括號裡），沒有就維持原名 ——
// 與系統匣、命名視窗共用 core/tray_label.h 的同一套規則。
// 內建項目在載入時就被 removeBuiltinActions() 拿掉了，所以這裡不必再標「內建」。
QString itemLabel(const std::string& locale, const std::map<std::string, std::string>& meanings, const std::string& key, const std::string& raw) {
  const auto it = meanings.find(key);
  const std::string meaning = it == meanings.end() ? std::string() : it->second;
  return QString::fromStdString(l2m::formatNamedLabel(locale, meaning, raw, std::string()));
}

// 工具列圖示。都在 src/viewer/icons.qrc 裡（Material Symbols 的 24px svg），
// 少了 Qt6::Svg 的話會安靜地變成空圖示，不會有任何錯誤訊息。
//
// **顏色不是 svg 裡寫的那個**：素材把 fill 寫死成近黑的 #1f1f1f，深色主題下
// 工具列底色也是深的，整排按鈕會看不見。所以一律過 tintedIcon() 依調色盤重塗，
// 理由與另外兩種做法為什麼不行寫在 tinted_icon.h。
QIcon viewerIcon(const char* name) { return tintedIcon(QStringLiteral(":/viewer/icons/%1.svg").arg(QLatin1String(name))); }

// 「設定為桌寵」連桌寵那條 QLocalSocket 的兩個上限。
// 回覆那一段給到 5 秒：桌寵那端要解析 model3.json 再重掃一次 models 目錄才回話，
// 真正耗時的 GL 載入排在淡出之後，不在這段等待裡面。
constexpr int kConnectTimeoutMs = 500;
constexpr int kReplyTimeoutMs = 5000;

// 桌寵沒在跑時要啟動的執行檔，就放在檢視器旁邊（安裝版與建置樹都是平放的）
#ifdef Q_OS_WIN
constexpr const char* kMateExecutable = "live2d_mate.exe";
#else
constexpr const char* kMateExecutable = "live2d_mate";
#endif

// 視窗幾何的設定檔：%APPDATA%/live2d_mate/viewer.json —— 跟桌寵**同一個資料夾**、
// 不同檔案（為什麼不寫進桌寵的 config.json，理由寫在 core/viewer_settings.h）。
//
// AppDataLocation 是 %APPDATA%/<applicationName>，而這支的 applicationName 是
// live2d_viewer（main.cpp 設的），所以把最後一層換成 live2d_mate 就是桌寵那個目錄。
// 三個平台的形狀一樣（Roaming\、~/.local/share/、~/Library/Application Support/），
// 換一層就對，不必自己拼 %APPDATA%。**這一段跟著 main.cpp 的 setApplicationName 走** ——
// 名字沒設的話 AppDataLocation 會少一層目錄，所以最後一層已經是 live2d_mate 時直接沿用。
std::filesystem::path viewerSettingsPath() {
  const std::filesystem::path appData = std::filesystem::u8path(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation).toStdString());
  if (appData.empty()) return {};  // 查不到就是不記，功能少一項總比寫到奇怪的地方好
  const std::filesystem::path dir = appData.filename() == "live2d_mate" ? appData : appData.parent_path() / "live2d_mate";
  return dir / "viewer.json";
}

// QScreen → 純資料的矩形。用 availableGeometry 而不是 geometry：
// 工作列蓋住的那一條不能算「看得見」，視窗擺在那裡等於抓不到標題列。
l2m::ScreenRect toScreenRect(const QScreen* screen) {
  const QRect rect = screen->availableGeometry();
  return {rect.x(), rect.y(), rect.width(), rect.height()};
}

}  // namespace

ViewerWindow::ViewerWindow() {
  locale_ = l2m::i18n::matchLocale(QLocale::system().name().toStdString());
  buildUi();
  retranslate();
  setAcceptDrops(true);
  // buildUi() 的 resize() 只是沒有設定檔時的預設值，這裡有記錄就蓋掉它。
  // 必須在 show() 之前 —— 先顯示再搬動的話畫面會閃一下。
  restoreWindowGeometry();
  statusBar()->showMessage(tr2("viewer.empty"));
}

// 視窗幾何的存與取。Qt/OS 的部分只有「讀檔、寫檔、問 QScreen」，
// 會錯的判斷（尺寸合不合理、螢幕沒了怎麼辦）全在 core/viewer_settings.h。
void ViewerWindow::restoreWindowGeometry() {
  const std::filesystem::path path = viewerSettingsPath();
  if (path.empty()) return;

  const std::optional<std::string> text = l2m::jsonu::readFileUtf8(path);
  if (!text) return;  // 第一次開，或這台機器從來沒跑過桌寵
  const std::optional<l2m::WindowGeometry> saved = l2m::parseViewerWindow(*text);
  if (!saved) return;  // 檔案壞掉就當作沒有，不吵使用者

  // 主螢幕排第一：fitToScreens 在「一面螢幕都碰不到」時退回的就是第一個。
  std::vector<l2m::ScreenRect> screens;
  const QScreen* primary = QGuiApplication::primaryScreen();
  if (primary) screens.push_back(toScreenRect(primary));
  for (const QScreen* screen : QGuiApplication::screens()) {
    if (screen != primary) screens.push_back(toScreenRect(screen));
  }

  const l2m::WindowGeometry fitted = l2m::fitToScreens(*saved, screens);
  setGeometry(fitted.x, fitted.y, fitted.width, fitted.height);
  // 最大化要在 show() 之前設狀態，而且要排在 setGeometry 之後：Qt 把當下的
  // 矩形記成「還原後的大小」，順序反過來的話取消最大化會跳回預設位置。
  if (fitted.maximized) setWindowState(windowState() | Qt::WindowMaximized);
}

void ViewerWindow::saveWindowGeometry() const {
  const std::filesystem::path path = viewerSettingsPath();
  if (path.empty()) return;

  // 最大化／全螢幕時 geometry() 是那個滿版矩形，記下去等於把「還原後的大小」
  // 弄丟；normalGeometry() 才是要的那一份（Qt 自己維護，算不出來才退回 geometry()）。
  // isMinimized() 也要算進來：Windows 的最小化視窗座標是 -32000，那個值通得過
  // kMaxWindowCoord 的檢查而被原樣寫進 viewer.json，下次開啟被 fitToScreens 判成
  // 「在畫面外」而擺回主螢幕正中央 —— 位置就這麼靜靜地掉了（尺寸還在，所以不容易
  // 發現）。從工作列右鍵關閉一個最小化中的視窗走的就是這條。
  const bool covered = isMaximized() || isFullScreen() || isMinimized();
  QRect rect = covered ? normalGeometry() : geometry();
  if (!rect.isValid()) rect = geometry();

  // 記的是 isMaximized() 而不是 covered：**全螢幕刻意不記** —— F11 是臨時狀態，
  // 下次一開就整片蓋住桌面會嚇到人（狀態列還收著，看起來像當掉了）。
  const l2m::WindowGeometry saved{rect.x(), rect.y(), rect.width(), rect.height(), isMaximized()};

  std::error_code ec;
  std::filesystem::create_directories(path.parent_path(), ec);  // 桌寵從來沒跑過時這個目錄還不存在
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    qWarning() << "[viewer] 視窗位置寫不進去" << QString::fromStdString(path.u8string());
    return;
  }
  file << l2m::serializeViewerWindow(saved);
}

void ViewerWindow::closeEvent(QCloseEvent* event) {
  // 同時開好幾個 Viewer 時**最後關掉的那個說了算**（沒有 single-instance，
  // 誰都可以寫）。要記的東西就這一份，先來後到不值得多一層鎖。
  saveWindowGeometry();
  QMainWindow::closeEvent(event);
}

QString ViewerWindow::tr2(const char* key) const { return QString::fromStdString(l2m::i18n::translate(locale_, key)); }

void ViewerWindow::buildUi() {
  canvas_ = new ViewerCanvas(this);
  connect(canvas_, &ViewerCanvas::modelTapped, this, &ViewerWindow::playTapMotion);
  connect(canvas_, &ViewerCanvas::modelLoaded, this, [this](bool ok) {
    if (ok) {
      refreshLists();
      return;
    }
    clearLists();
    // 有原因就講原因（「這個建置解不動 webp」之類）。reason 是英文，與
    // viewer.status.setAsPet.failed 的 {error} 同一個慣例：技術原因來自 core 的純函式，
    // 翻譯的是外框那句話 —— 把診斷訊息也做成五份翻譯只會讓它們慢慢對不上。
    const std::string reason = canvas_->lastLoadError();
    const char* key = reason.empty() ? "viewer.status.loadFailed" : "viewer.status.loadFailedReason";
    statusBar()->showMessage(QString::fromStdString(l2m::i18n::translate(locale_, key, l2m::i18n::TParams().arg("path", modelPath_.toStdString()).arg("reason", reason))));
  });

  auto* side = new QWidget(this);
  // 浮動模式時清單是蓋在模型上的，自己不畫底色的話會整片透出去而看不清楚。
  // 停靠模式下畫的是同一個 palette 的視窗底色，所以兩種模式看起來一樣。
  side->setAutoFillBackground(true);
  auto* sideLayout = new QVBoxLayout(side);

  buildToolBar(side);
  sideLayout->addWidget(toolBar_);

  modelLabel_ = new QLabel(side);
  modelLabel_->setWordWrap(true);
  modelLabel_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  sideLayout->addWidget(modelLabel_);

  motionBox_ = new QGroupBox(side);
  auto* motionLayout = new QVBoxLayout(motionBox_);
  motionTree_ = new QTreeWidget(motionBox_);
  motionTree_->setHeaderHidden(true);
  // 單擊就播：這是預覽工具，多一次雙擊只是多一次等待。
  // itemActivated 另外接是為了鍵盤（Enter）也能播。
  connect(motionTree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) { playMotionItem(item); });
  connect(motionTree_, &QTreeWidget::itemActivated, this, [this](QTreeWidgetItem* item, int) { playMotionItem(item); });
  motionLayout->addWidget(motionTree_);
  sideLayout->addWidget(motionBox_, 1);

  expressionBox_ = new QGroupBox(side);
  auto* expressionLayout = new QVBoxLayout(expressionBox_);
  expressionList_ = new QListWidget(expressionBox_);
  connect(expressionList_, &QListWidget::itemClicked, this, &ViewerWindow::applyExpressionItem);
  connect(expressionList_, &QListWidget::itemActivated, this, &ViewerWindow::applyExpressionItem);
  expressionLayout->addWidget(expressionList_);
  sideLayout->addWidget(expressionBox_, 1);

  splitter_ = new CollapsibleSplitter(Qt::Horizontal, this);
  splitter_->addWidget(canvas_);
  splitter_->addWidget(side);
  // 拉大視窗時把空間全給畫布 —— 清單維持原寬，模型跟著變大
  splitter_->setStretchFactor(0, 1);
  splitter_->setStretchFactor(1, 0);
  splitter_->setSizes({700, 300});
  setCentralWidget(splitter_);

  // 浮動模式靠這兩個掛鉤把畫布撐回整條寬度（見 eventFilter 的註解）
  canvas_->installEventFilter(this);
  splitter_->installEventFilter(this);

  // F11 全螢幕。用 QShortcut 而不是 keyPressEvent：焦點多半在清單或畫布那些
  // 子 widget 上，主視窗的 keyPressEvent 根本收不到；QShortcut 預設的
  // WindowShortcut 情境則是整個視窗有效。
  auto* fullScreen = new QShortcut(QKeySequence(Qt::Key_F11), this);
  connect(fullScreen, &QShortcut::activated, this, &ViewerWindow::toggleFullScreen);

  // 沒有設定檔（第一次開）時的預設大小；有記錄的話建構子的
  // restoreWindowGeometry() 會蓋掉它。
  resize(1000, 680);
}

void ViewerWindow::buildToolBar(QWidget* parent) {
  // **不是** QMainWindow::addToolBar() 加的視窗級工具列，而是塞進清單那一欄的
  // 版面裡當一般 widget。這樣它跟著清單走：清單浮起來它一起浮、清單收合它一起
  // 消失，全螢幕也跟狀態列一起收掉（見 toggleFullScreen）。
  // QToolBar 本來就是 QWidget，放進 QVBoxLayout 沒有任何特別待遇；也因此不必
  // 設 objectName（那是 QMainWindow::saveState 才要的）。
  toolBar_ = new QToolBar(parent);
  toolBar_->setMovable(false);
  toolBar_->setFloatable(false);
  // 純圖示，說明字走 tooltip —— 清單那一欄只有 300 px 上下，五顆按鈕帶文字的話
  // 一定有人被收進右邊的「»」延伸選單。
  toolBar_->setToolButtonStyle(Qt::ToolButtonIconOnly);

  // 圖示一律等 updateToolBarState() 指派 —— 這三顆雖然不隨狀態變，卻會隨調色盤變
  // （深色主題要換成亮色，見 tinted_icon.h），而重畫的入口只有那一支。
  openAction_ = toolBar_->addAction(QIcon(), QString());
  connect(openAction_, &QAction::triggered, this, &ViewerWindow::chooseModel);

  // 跟「開啟模型…」同一組（都在講「拿哪一隻模型」），不是後面那組播放控制。
  // 還沒載入任何模型時是灰的 —— 按下去只會得到一句「沒有模型」。
  setAsPetAction_ = toolBar_->addAction(QIcon(), QString());
  setAsPetAction_->setEnabled(false);
  connect(setAsPetAction_, &QAction::triggered, this, &ViewerWindow::setAsPet);

  toolBar_->addSeparator();

  resetAction_ = toolBar_->addAction(QIcon(), QString());
  connect(resetAction_, &QAction::triggered, this, &ViewerWindow::resetPlayback);

  // 三顆狀態鈕都是 checkable（凹著＝那個模式正開著），圖示則畫**按下去會發生
  // 什麼**（同「全螢幕 ↔ 離開全螢幕」那組箭頭的通用慣例），tooltip 再寫一次。
  // 這三件事都由 updateToolBarState() 一起重寫，狀態才不會各走各的。
  //
  // **一律接 triggered 而不是 toggled**：後者連 setChecked() 也會發，
  // updateToolBarState() 一寫就繞回這裡的 slot。
  loopAction_ = toolBar_->addAction(QIcon(), QString());
  loopAction_->setCheckable(true);
  connect(loopAction_, &QAction::triggered, this, [this](bool on) { setAutoReplay(on); });

  toolBar_->addSeparator();

  // 會撐開的空白，把後面兩顆推到最右邊 —— QToolBar 沒有「靠右對齊」這種選項，
  // 塞一個 Expanding 的空 widget 是 Qt 的標準做法。它的 minimumWidth 是 0，
  // 所以欄位變窄時先被壓扁的是這塊空白，按鈕不會提早被收進「»」延伸選單。
  auto* spacer = new QWidget(toolBar_);
  spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
  toolBar_->addWidget(spacer);

  dockAction_ = toolBar_->addAction(QIcon(), QString());
  dockAction_->setCheckable(true);
  connect(dockAction_, &QAction::triggered, this, [this](bool floating) { setDocked(!floating); });

  fullScreenAction_ = toolBar_->addAction(QIcon(), QString());
  fullScreenAction_->setCheckable(true);
  connect(fullScreenAction_, &QAction::triggered, this, [this] { toggleFullScreen(); });
}

void ViewerWindow::retranslate() {
  setWindowTitle(tr2("viewer.windowTitle"));
  // 純圖示按鈕的說明字就是 tooltip；text 仍然要設，那是無障礙介面讀到的名字。
  // 三顆狀態鈕的文字會跟著狀態換，所以只在 updateToolBarState() 裡寫。
  openAction_->setText(tr2("viewer.open"));
  openAction_->setToolTip(tr2("viewer.open"));
  setAsPetAction_->setText(tr2("viewer.setAsPet"));
  setAsPetAction_->setToolTip(tr2("viewer.setAsPet"));
  resetAction_->setText(tr2("viewer.reset"));
  resetAction_->setToolTip(tr2("viewer.reset"));
  motionBox_->setTitle(tr2("viewer.section.motions"));
  expressionBox_->setTitle(tr2("viewer.section.expressions"));
  splitter_->setToggleTips(tr2("viewer.splitter.collapse"), tr2("viewer.splitter.expand"));
  updateToolBarState();
}

// 系統在淺／深色之間切換時，Qt 會換掉整個應用程式的調色盤並送這幾個事件過來。
// 工具列圖示是照調色盤染的（tinted_icon.h），主題一變就整組重新指派一次。
// ThemeChange 也要接：使用者若曾經以 QApplication::setPalette() 明示指定過調色盤，
// Qt 就不再覆寫它、也不發 ApplicationPaletteChange，只剩這一則。
void ViewerWindow::changeEvent(QEvent* event) {
  QMainWindow::changeEvent(event);
  const QEvent::Type type = event->type();
  if (type == QEvent::ApplicationPaletteChange || type == QEvent::PaletteChange || type == QEvent::StyleChange || type == QEvent::ThemeChange) updateToolBarState();
}

void ViewerWindow::updateToolBarState() {
  // 工具列還沒建好就收到調色盤事件（changeEvent 那條路）—— 什麼都不必做，
  // 建完之後建構子的 retranslate() 會呼叫這裡。
  if (!fullScreenAction_) return;

  // 前三顆不隨狀態變，但**每次都重新指派一份新的 QIcon**：換掉 QIcon::cacheKey()，
  // 誰都不會拿到 QPixmapCache 裡那張舊顏色的圖（成本與取捨寫在 tinted_icon.h 最後一段）。
  // 文字與 tooltip 是固定的，在 retranslate()。
  openAction_->setIcon(viewerIcon("open_file"));
  setAsPetAction_->setIcon(viewerIcon("set_as_pet"));
  resetAction_->setIcon(viewerIcon("stop"));

  // 後三顆都是「凹著＝這個模式正開著」，圖示畫的則是**按下去會發生什麼**
  // —— 全螢幕那組向內／向外的箭頭本來就是這個慣例，另外兩組跟著走才一致。
  const auto apply = [](QAction* action, bool on, const QString& icon, const QString& tip) {
    action->setChecked(on);
    action->setIcon(viewerIcon(icon.toLatin1().constData()));
    action->setText(tip);
    action->setToolTip(tip);
  };
  apply(dockAction_, !docked_, docked_ ? QStringLiteral("undock") : QStringLiteral("dock"), tr2(docked_ ? "viewer.undock" : "viewer.dock"));
  const bool full = isFullScreen();
  apply(fullScreenAction_, full, full ? QStringLiteral("exit_fullscreen") : QStringLiteral("fullscreen"), tr2(full ? "viewer.exitFullScreen" : "viewer.fullScreen"));
  apply(loopAction_, autoReplay_, autoReplay_ ? QStringLiteral("no_loop") : QStringLiteral("loop"), tr2(autoReplay_ ? "viewer.noLoop" : "viewer.loop"));
}

void ViewerWindow::setAutoReplay(bool on) {
  if (autoReplay_ == on) return;
  autoReplay_ = on;
  updateToolBarState();
  // 立刻套到剛才那一段上，不然按了要等下一次點清單才看得出差別。
  // 沒有「剛才那一段」（還沒預覽過、或按過停止動作）就只是換個設定。
  if (!lastMotionGroup_.empty()) startPreviewMotion(lastMotionGroup_, lastMotionIndex_);
}

void ViewerWindow::setDocked(bool docked) {
  if (docked_ == docked) return;
  docked_ = docked;
  // 浮動時畫布要在兄弟裡的最底層，清單與手把才蓋得住它。
  // 停靠時壓在最底層也沒差 —— 那時候三者根本不重疊。
  canvas_->lower();
  // 兩個方向不對稱，而且**不能都交給 refresh()**：splitter 內部的尺寸從頭到尾
  // 沒變過（浮動只改畫布的 geometry，它並不知道），所以切成浮動時 refresh()
  // 設回去的是同一個矩形 —— QWidget::setGeometry 值沒變就不發 Resize，
  // eventFilter 也就永遠不會動，畫面上看起來像按鈕沒反應。
  if (docked_) {
    // 回停靠：畫布現在是整條寬度，refresh() 會把它縮回第一格（值真的有變）
    splitter_->refresh();
  } else {
    // 切浮動：自己撐開一次，之後每次拖手把都由 eventFilter 接手
    canvas_->setGeometry(splitter_->contentsRect());
  }
  updateToolBarState();
}

bool ViewerWindow::eventFilter(QObject* watched, QEvent* event) {
  // 浮動模式：畫布仍然是 splitter 的第一格（手把要靠它才拖得動清單），
  // 只是每次 splitter 幫它排完版就立刻改回整條寬度。於是拖手把只改到清單的
  // 寬度，畫布一動也不動 —— 清單是後加入的兄弟，本來就疊在畫布上面。
  //
  // setGeometry 會再發一次 Resize/Move，但第二次的值一樣就不會再發，
  // 所以最多多繞一圈，不會無限遞迴。
  if (!docked_ && (watched == canvas_ || watched == splitter_)) {
    const QEvent::Type type = event->type();
    if (type == QEvent::Resize || type == QEvent::Move) canvas_->setGeometry(splitter_->contentsRect());
  }
  return QMainWindow::eventFilter(watched, event);
}

bool ViewerWindow::looksLikeModelPath(const QString& path) {
  const QString name = QFileInfo(path).fileName();
  if (name.endsWith(QStringLiteral(".model3.json"), Qt::CaseInsensitive)) return true;
  if (name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive)) return true;
  // 資料夾：**第一層真的有入口檔才收**（loadModel 會用同一支換成那個檔案）。
  // 這一步會碰磁碟，但呼叫點只有 dragEnterEvent、dropEvent 與命令列 —— 拖曳期間
  // 一直來的是 dragMoveEvent，沒有重寫就沿用 enter 的答案，不會每格重掃一次。
  //
  // 先檢查再接受，而不是一律接受、載入失敗才報錯：找不到模型的資料夾應該讓
  // 游標當場顯示「不能放」，那比放開之後才在狀態列出現一行字有用得多。
  if (l2m::findDirectoryEntry(std::filesystem::u8path(path.toStdString()))) return true;
  // 裸命名入口（model.json / index.json）也收：能不能載入交給 describeModel 判斷
  return l2m::isGenericEntryName(name.toStdString());
}

QString ViewerWindow::firstModelPath(const QMimeData* mime) {
  if (!mime || !mime->hasUrls()) return {};
  for (const QUrl& url : mime->urls()) {
    const QString local = url.toLocalFile();
    if (!local.isEmpty() && looksLikeModelPath(local)) return local;
  }
  return {};
}

void ViewerWindow::dragEnterEvent(QDragEnterEvent* event) {
  if (!firstModelPath(event->mimeData()).isEmpty()) event->acceptProposedAction();
}

void ViewerWindow::dropEvent(QDropEvent* event) {
  const QString path = firstModelPath(event->mimeData());
  if (path.isEmpty()) return;
  event->acceptProposedAction();
  loadModel(path);
}

void ViewerWindow::chooseModel() {
  const QString filter = tr2("viewer.dialog.filter") + QStringLiteral(";;") + tr2("viewer.dialog.filterAll");
  const QString path = QFileDialog::getOpenFileName(this, tr2("viewer.dialog.title"), modelPath_, filter);
  if (!path.isEmpty()) loadModel(path);
}

void ViewerWindow::loadModel(const QString& entryPath) {
  // 拖進來（或命令列給）的是資料夾時，先換成它第一層的入口檔。判定與
  // looksLikeModelPath 走的是同一支，所以「拖得進來」與「載得起來」不會分岔。
  // 下面一路用的都是換過的 path：modelPath_ 要是真正的入口檔，否則「設定為桌寵」
  // 會把一個資料夾路徑送給桌寵，那端 describeModel 一定回不了模型。
  QString path = entryPath;
  if (const std::optional<std::filesystem::path> dirEntry = l2m::findDirectoryEntry(std::filesystem::u8path(entryPath.toStdString()))) {
    path = QString::fromStdString(dirEntry->u8string());
  }

  // 先解析內容（不碰 GL）。桌寵掃描 models 目錄時跑的是同一支，
  // 所以動作／表情清單兩邊一字不差。
  std::optional<l2m::ModelInfo> described = l2m::describeModel(std::filesystem::u8path(path.toStdString()));
  if (!described) {
    statusBar()->showMessage(QString::fromStdString(l2m::i18n::translate(locale_, "viewer.status.parseFailed", l2m::i18n::TParams().arg("path", path.toStdString()))));
    return;
  }

  // 合成的內建動作／表情只屬於桌寵（那是餵給 AI 的介面），檢視器只顯示模型
  // 真正做了什麼。連 model_ 都不留：留著的話狀態列的計數與 playMotionByName
  // 的解析都還看得到它們，清單卻沒有，等於自己製造兩份不一致的真相。
  l2m::removeBuiltinActions(*described);

  model_ = std::move(described);
  modelPath_ = path;
  // 清單等 modelLoaded 才填 —— GL 那邊載入失敗時不該留著一份點了沒反應的清單
  canvas_->requestModelLoad(path);
}

void ViewerWindow::setAsPet() {
  if (modelPath_.isEmpty() || !model_) return;

  // 一律送絕對路徑：桌寵是另一個行程，工作目錄跟這裡沒有關係。
  // 正規化（POSIX 斜線、收掉 "." 與 ".."）由桌寵那端做，
  // 它存進 config 的形式必須跟自己算出來的一致。
  const QString absolute = QFileInfo(modelPath_).absoluteFilePath();
  const std::string name = model_->name;

  const auto report = [this, &name](const char* key) { statusBar()->showMessage(QString::fromStdString(l2m::i18n::translate(locale_, key, l2m::i18n::TParams().arg("name", name)))); };
  const auto reportError = [this](const std::string& error) {
    statusBar()->showMessage(QString::fromStdString(l2m::i18n::translate(locale_, "viewer.status.setAsPet.failed", l2m::i18n::TParams().arg("error", error))));
  };

  // ── 桌寵已經在跑：直接送給它 ──
  // 連得上就一定要讀回那一行 —— 「送出去了」與「桌寵真的換好了」是兩件事，
  // 路徑可能根本不是 Cubism 4 模型，那句錯誤只有桌寵那端知道。
  QLocalSocket socket;
  socket.connectToServer(QString::fromLatin1(l2m::kInstanceServerName));
  if (socket.waitForConnected(kConnectTimeoutMs)) {
    const std::string request = l2m::encodeSetModelRequest(absolute.toStdString());
    socket.write(QByteArray::fromStdString(request));
    socket.waitForBytesWritten(kConnectTimeoutMs);

    // 等回覆是同步的，最壞會卡滿 kReplyTimeoutMs（舊版桌寵永遠不回話）。
    // 沒有沙漏游標的話那幾秒看起來就是整個視窗當掉了；狀態列也先寫一句再
    // repaint()，因為事件迴圈馬上就被下面的 wait 佔住，不強制重畫看不到。
    statusBar()->showMessage(tr2("viewer.status.setAsPet.sending"));
    statusBar()->repaint();
    QGuiApplication::setOverrideCursor(Qt::BusyCursor);
    QByteArray answer;
    while (!answer.contains('\n') && socket.waitForReadyRead(kReplyTimeoutMs)) answer.append(socket.readAll());
    QGuiApplication::restoreOverrideCursor();
    socket.disconnectFromServer();

    // 一個字都沒回**不能**當成成功：對面若是不認得 setmodel 的舊版桌寵，
    // 它只會把角色叫出來然後沉默。這裡報成功的話，使用者看到「已設定為桌寵」
    // 而畫面上什麼都沒變 —— 靜默的假成功比一句錯誤糟得多。
    //
    // 但訊息刻意**不斷言原因**：新版桌寵也可能只是慢（它還沒載過任何模型時
    // AppController::fadeOutThen 走同步捷徑，回覆要等整個 GL 載入跑完才寫得出來），
    // 而那條路其實是成功的。講死「版本太舊」會在那種情況下說謊。
    if (answer.isEmpty()) {
      statusBar()->showMessage(tr2("viewer.status.setAsPet.noAnswer"));
      return;
    }

    const l2m::InstanceReply reply = l2m::parseInstanceReply(answer.toStdString());
    if (reply.ok) {
      report("viewer.status.setAsPet.ok");
    } else {
      reportError(reply.message);
    }
    return;
  }

  // ── 桌寵沒在跑：啟動一個，讓它自己帶著模型開起來 ──
  // 這條只知道「行程生出來了」，換模型的成敗要等它自己跑完，所以訊息也只講到那裡。
  const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath(QString::fromLatin1(kMateExecutable));
  if (!QFileInfo::exists(exe)) {
    statusBar()->showMessage(tr2("viewer.status.setAsPet.noMate"));
    return;
  }
  if (!QProcess::startDetached(exe, {QString::fromLatin1(l2m::kSetModelFlag), absolute})) {
    statusBar()->showMessage(tr2("viewer.status.setAsPet.noMate"));
    return;
  }
  report("viewer.status.setAsPet.launched");
}

void ViewerWindow::clearLists() {
  motionTree_->clear();
  expressionList_->clear();
  modelLabel_->clear();
  // GL 載入失敗時 model_ 仍然留著上一份（見 loadModel），但畫面上什麼都沒有 ——
  // 這種模型交給桌寵，桌寵那端的 describeModel 一樣會過、一樣回 ok，
  // 於是兩邊都是空白畫面卻報「已設定為桌寵」。所以按鈕跟著清單一起熄掉。
  setAsPetAction_->setEnabled(false);
  setWindowTitle(tr2("viewer.windowTitle"));
}

void ViewerWindow::refreshLists() {
  motionTree_->clear();
  expressionList_->clear();
  setAsPetAction_->setEnabled(model_.has_value());
  if (!model_) return;
  const l2m::ModelInfo& model = *model_;

  for (const auto& group : model.motions) {
    auto* item = new QTreeWidgetItem(motionTree_);
    item->setText(0, itemLabel(locale_, model.annotations.motions, l2m::motionKey(group.name), group.name));
    item->setData(0, kRoleGroup, QString::fromStdString(group.name));
    item->setData(0, kRoleIndex, -1);

    // 只有一段的群組不展開子項目：點群組本身就是點那一段
    if (group.count <= 1) continue;
    for (int i = 0; i < group.count; ++i) {
      const std::string file = static_cast<size_t>(i) < group.files.size() ? group.files[static_cast<size_t>(i)] : std::string();
      const std::string raw = file.empty() ? "#" + std::to_string(i) : file;
      auto* child = new QTreeWidgetItem(item);
      child->setText(0, itemLabel(locale_, model.annotations.motions, l2m::motionKey(group.name, i), raw));
      child->setData(0, kRoleGroup, QString::fromStdString(group.name));
      child->setData(0, kRoleIndex, i);
    }
  }
  motionTree_->expandAll();

  // 第一列固定是「不套用表情」，才有辦法把套上去的表情收回來
  auto* none = new QListWidgetItem(tr2("viewer.expression.none"), expressionList_);
  none->setData(Qt::UserRole, QString());
  for (const auto& name : model.expressions) {
    auto* item = new QListWidgetItem(itemLabel(locale_, model.annotations.expressions, name, name), expressionList_);
    item->setData(Qt::UserRole, QString::fromStdString(name));
  }

  modelLabel_->setText(modelPath_);
  setWindowTitle(QString::fromStdString(l2m::i18n::translate(locale_, "viewer.windowTitle.withModel", l2m::i18n::TParams().arg("name", model.name))));
  showModelStatus();
}

void ViewerWindow::showModelStatus() {
  if (!model_) return;
  const l2m::ModelInfo& model = *model_;
  statusBar()->showMessage(QString::fromStdString(l2m::i18n::translate(
    locale_, "viewer.status.loaded", l2m::i18n::TParams().arg("name", model.name).arg("motions", std::to_string(model.motions.size())).arg("expressions", std::to_string(model.expressions.size())))));
}

void ViewerWindow::playMotionItem(QTreeWidgetItem* item) {
  if (!item) return;
  startPreviewMotion(item->data(0, kRoleGroup).toString().toStdString(), item->data(0, kRoleIndex).toInt());
}

bool ViewerWindow::startPreviewMotion(const std::string& group, int index) {
  if (!model_) return false;
  auto* controller = canvas_->modelController();
  if (!controller) return false;

  // 預覽一律用 PriorityForce：使用者按了就要看到，不該被待機動作擋下來
  if (l2m::playMotionByName(*controller, *model_, group, index, l2m::ModelController::PriorityForce, autoReplay_)) {
    // 記住這一段，切換「自動重播」時才能當場重播一次讓人看到差別。
    // 只在真的播起來時記 —— 被擋下來的那一段不算「剛才在播的」。
    lastMotionGroup_ = group;
    lastMotionIndex_ = index;
    return true;
  }
  statusBar()->showMessage(QString::fromStdString(l2m::i18n::translate(locale_, "viewer.status.motionRejected", l2m::i18n::TParams().arg("name", group))));
  return false;
}

void ViewerWindow::playTapMotion(const QPointF& localPos) {
  if (!model_) return;
  auto* controller = canvas_->modelController();
  if (!controller) return;

  // 命中測試與部位推算跟桌寵同一支（live2d/model_controller.h 的 tapBodyPart）：
  // 99% 的模型沒有填 HitAreas，那時靠的是模型外接框的幾何。
  float viewX = 0;
  float viewY = 0;
  controller->screenToView(localPos.x(), localPos.y(), canvas_->width(), canvas_->height(), &viewX, &viewY);

  // **點在角色身上才算數。** 桌寵靠 alpha 形狀視窗把剪影外的點擊整個穿透掉，
  // 這裡沒有那層 —— 不擋的話點畫布右上角的深灰空白也會播摸頭（regionAt 會把
  // 框外的點夾回框內）。用外接框而不是逐像素剪影：Viewer 的畫布底色不透明，
  // 讀不到 alpha 剪影，而外接框已經足夠把「明顯的空白處」擋在外面。
  const auto box = controller->visibleBoundsView();
  if (box && !(viewX >= box->left && viewX <= box->right && viewY >= box->bottom && viewY <= box->top)) return;

  const std::vector<std::string> areas = controller->hitTest(viewX, viewY);
  const l2m::BodyPart part = controller->tapBodyPart(areas, localPos.x(), localPos.y(), canvas_->width(), canvas_->height());

  if (const auto pick = l2m::pickTapMotion(areas, part, model_->motions)) {
    if (startPreviewMotion(pick->group, pick->index)) selectMotionItem(pick->group, pick->index);
    return;
  }

  // 模型沒做觸摸動作：退回隨機一段（同桌寵 —— 點了完全沒反應比播錯一段還糟）。
  // 待機群組先排掉，全被排掉才連它一起挑：很多模型把所有動作都歸在 Idle 底下，
  // 那種模型排乾淨之後一個都不剩。
  std::vector<const l2m::MotionGroupInfo*> groups;
  for (const auto& group : model_->motions) {
    if (group.count > 0 && !l2m::strutil::equalsInsensitive(group.name, "Idle")) groups.push_back(&group);
  }
  if (groups.empty()) {
    for (const auto& group : model_->motions) {
      if (group.count > 0) groups.push_back(&group);
    }
  }
  if (groups.empty()) return;
  const int pick = QRandomGenerator::global()->bounded(static_cast<int>(groups.size()));
  if (startPreviewMotion(groups[static_cast<size_t>(pick)]->name, -1)) selectMotionItem(groups[static_cast<size_t>(pick)]->name, -1);
}

void ViewerWindow::selectMotionItem(const std::string& group, int index) {
  const QString wanted = QString::fromStdString(group);
  for (int i = 0; i < motionTree_->topLevelItemCount(); ++i) {
    QTreeWidgetItem* top = motionTree_->topLevelItem(i);
    if (top->data(0, kRoleGroup).toString() != wanted) continue;
    // 只有一段的群組沒有子項目，index 落在群組本身
    QTreeWidgetItem* target = top;
    for (int child = 0; child < top->childCount(); ++child) {
      if (top->child(child)->data(0, kRoleIndex).toInt() == index) {
        target = top->child(child);
        break;
      }
    }
    motionTree_->setCurrentItem(target);
    motionTree_->scrollToItem(target);
    return;
  }
}

void ViewerWindow::applyExpressionItem(QListWidgetItem* item) {
  if (!item || !model_) return;
  auto* controller = canvas_->modelController();
  if (!controller) return;

  const QString name = item->data(Qt::UserRole).toString();
  std::optional<std::string> resolved;
  if (!name.isEmpty()) resolved = name.toStdString();
  if (l2m::applyExpressionByName(*controller, canvas_->overlay(), *model_, resolved)) return;
  statusBar()->showMessage(QString::fromStdString(l2m::i18n::translate(locale_, "viewer.status.expressionFailed", l2m::i18n::TParams().arg("name", name.toStdString()))));
}

void ViewerWindow::resetPlayback() {
  auto* controller = canvas_->modelController();
  if (!controller) return;

  // ── 動作 ────────────────────────────────────────────────
  // 這裡不必呼叫 stopLoopingAiMotion()：唯一會一直循環的是內建的 doze，
  // 而內建項目在 loadModel() 就整組拿掉了，檢視器播得到的動作都會自己播完。
  //
  // 走 startIdleMotion 而不是寫死 startMotion("Idle", …)：待機群組名不是規格，
  // 碧藍航線那批把待機動畫塞在空字串群組裡（只有檔名叫 idle），寫死的話
  // startMotion 在 GetMotionCount 就回 false，**現在播的那一段還在播** ——
  // 症狀是這顆按鈕在那些模型上按了完全沒反應（見 core/idle_motion_pick.h）。
  controller->startIdleMotion(l2m::ModelController::PriorityForce, /*restoreFirst=*/true);
  // 已經停了就不再是「剛才在播的那一段」，否則切一下自動重播它會自己活過來
  lastMotionGroup_.clear();
  lastMotionIndex_ = -1;

  // ── 表情 ────────────────────────────────────────────────
  // 表情跟動作是兩層（.exp3.json 走 Cubism 的表情管理、虛擬表情走參數覆寫層），
  // 動作停下來不會把它們帶走 —— 少了這一段，按過重置之後那張笑臉還掛在臉上，
  // 而且要手動去清單上點「（不套用表情）」才收得回來。
  // nullopt ＝ 清除，兩層由 applyExpressionByName 一起收乾淨（見 action_player.h）。
  if (model_) l2m::applyExpressionByName(*controller, canvas_->overlay(), *model_, std::nullopt);

  // 清單的游標也要跟著回到原點，否則畫面上是素臉、清單卻還反白著剛才那個表情，
  // 等於自己製造兩份不一致的真相。表情清單第一列固定是「（不套用表情）」
  // （見 refreshLists()），所以選 0 就是「現在什麼都沒套」。
  // setCurrentRow／setCurrentItem 不會發 itemClicked／itemActivated，
  // 接在那兩個訊號上的套用不會被繞回來。
  if (expressionList_->count() > 0) expressionList_->setCurrentRow(0);
  motionTree_->setCurrentItem(nullptr);
  // 狀態列同理：上一則「動作『X』沒有播放」沒有時限，不收掉的話重置完還掛在那裡
  showModelStatus();
}

void ViewerWindow::toggleFullScreen() {
  if (isFullScreen()) {
    statusBar()->show();
    // showNormal() 一律回到「還原」大小，本來最大化的視窗會突然縮掉
    if (wasMaximized_) {
      showMaximized();
    } else {
      showNormal();
    }
    updateToolBarState();
    return;
  }
  wasMaximized_ = isMaximized();
  // 底下那一條狀態列在全螢幕只會礙眼，收起來；離開時再放回來。
  // 訊息本身沒有丟掉，showMessage 期間收起來、回來還是同一句。
  //
  // **工具列刻意留著**：全螢幕本身就是工具列上的一顆 toggle，收掉的話它按下去
  // 就再也按不回來（只剩 F11），那顆的 exit_fullscreen 圖示也永遠見不到人。
  // 真的想要整片乾淨的話，手把上那顆收合鈕會把整欄清單一起收掉。
  statusBar()->hide();
  showFullScreen();
  updateToolBarState();
}
