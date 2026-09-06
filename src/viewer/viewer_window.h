#pragma once

// Live2D Viewer 的唯一一個視窗。
//
// 版面：左邊是 ViewerCanvas（模型），右邊那一欄由上而下是工具列（六顆純圖示，
// 左邊是開啟模型／設定為桌寵／重置動作與表情／自動重播，右邊靠齊的是浮動清單／全螢幕，
// 圖示是 icons.qrc 裡的 svg，見 buildToolBar()）、模型路徑、動作清單、表情清單。
// 圖示的**顏色跟著系統主題走**（素材本身寫死近黑，深色主題下會整排看不見）：
// 一律過 tinted_icon.h 的 tintedIcon() 依調色盤重塗，切換主題時由 changeEvent 重畫。
//
// ── 設定為桌寵（setAsPet）────────────────────────────────
// 把現在看的這一隻交給桌寵當桌寵。刻意跟「開啟模型…」排在同一組（兩顆都在講
// 「拿哪一隻模型」），不是後面那組播放控制；沒載入模型時是灰的。
// 送出的協定與正規化規則在 core/external_model.h（那邊有測試釘住）：
// 桌寵在跑就直接連它那條 QLocalSocket 並**讀回一行**（「送出去了」與
// 「桌寵真的換好了」是兩件事 —— 路徑可能根本不是 Cubism 4 模型，
// 那句錯誤只有桌寵那端知道）；沒在跑就啟動一個帶 --set-model 的新實例，
// 那條只知道行程生出來了，所以狀態列的訊息也只講到那裡。
// 中間的 CollapsibleSplitter 可以拖，手把上還有一顆按鈕能一鍵把清單收起來／
// 放回原寬（見 collapsible_splitter.h）。模型會跟著畫布的大小縮放
//（投影每幀重算，見 viewer_canvas.h）。
//
// 工具列刻意**不是** QMainWindow::addToolBar() 的視窗級工具列，而是清單那一欄
// 版面裡的一般 widget：這樣它跟著清單走 —— 清單浮起來它一起浮、收合一起消失。
// F11 切換全螢幕時只收狀態列，**工具列刻意留著** —— 全螢幕本身就是工具列上的
// 一顆 toggle，收掉的話按下去就再也按不回來（只剩 F11）。要整片乾淨的話，
// 手把上那顆收合鈕會把整欄清單一起收掉。
//
// ── 停靠／浮動（docked_）─────────────────────────────────
// 清單有兩種佔位方式，切換鈕在工具列右半邊（checkable，凹著＝正浮著）：
//   停靠（預設）＝ 清單佔走視窗的一塊，畫布縮小，也就是本來的樣子。
//   浮動        ＝ 畫布吃滿整個視窗，清單蓋在它上面；拖手把只改浮層寬度，
//                  畫布一動也不動。想「看大一點又不想關掉清單」時用。
//
// 做法刻意只有一句話：**畫布永遠是 splitter 的第一格**（手把要靠它才拖得動），
// 浮動時只是在 splitter 每次排完版之後，把畫布的 geometry 改回整條寬度
//（見 eventFilter）。清單是後加入的兄弟，本來就疊在畫布上面，於是不必動
// parent、不必另外做一層透明的佔位 widget，滑鼠命中判定也維持原生行為 ——
// 清單與手把不透明、照樣先吃到事件，其餘落到底下的畫布（視線跟隨要用）。
//
// 其他做法都試不得：另外放一個透明佔位 widget 蓋在畫布上，
// WA_TransparentForMouseEvents 會讓 QWidget::childAt 連整棵子樹一起跳過，
// 手把就再也點不到；把畫布 setParent 出去則是拿 QOpenGLWidget 的 GL context
// 去賭「同一個 top-level 不會重建」。
//
// 模型的來源有三條，全部落到同一支 loadModel()：
//   1.「開啟模型…」的檔案對話框
//   2. 把 *.model3.json、*.zip 或**整個模型資料夾**拖進視窗
//      （資料夾走 core/model_scanner.h 的 findDirectoryEntry() 換成第一層的入口檔）
//   3. 命令列參數（main.cpp 直接呼叫）
//
// 清單的內容不是自己解析出來的，而是 core/model_scanner.h 的 describeModel()
// —— 桌寵掃描 models 目錄時跑的是同一段程式碼，所以這裡看到的動作／表情
// （含執行期補全找回來的、含使用者寫在 *.annotations.json 裡的意義）
// 跟桌寵看到的一字不差。
//
// **唯一刻意的差別**：core/builtin_actions.h 合成的內建動作／表情在載入之後就用
// removeBuiltinActions() 整組拿掉。那些項目是桌寵餵給 AI 的介面（讓只綁一個 Idle
// 的模型也叫得動「揮手」），檢視器的用途卻是看這隻模型到底做了什麼，混進十來個
// 合成項目只會讓人分不清哪些是作者做的。掃描端因此完全不必動。
//
// 播放同樣不自己來：live2d/action_player.h 的兩支函式決定「這個名稱該走
// Cubism 的動作管理、還是合成動作、還是參數覆寫層」，桌寵走的也是那兩支。
//
// 介面全部手刻，沒有 .ui —— Qt Designer 在本專案只用在 src/windows/settings/。

#include <QMainWindow>
#include <QPointF>
#include <QString>

#include <optional>
#include <string>

#include "core/model_types.h"

class QAction;
class QGroupBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QCloseEvent;
class QMimeData;
class QToolBar;
class QTreeWidget;
class QTreeWidgetItem;
class CollapsibleSplitter;
class ViewerCanvas;

class ViewerWindow : public QMainWindow {
  Q_OBJECT

public:
  ViewerWindow();

  // 開啟一個 *.model3.json 或 *.zip。失敗只會在狀態列報告，不會關掉現有模型。
  void loadModel(const QString& entryPath);

  // 這個路徑看起來像不像模型入口（副檔名判斷，給拖放與命令列用）。
  // 真正能不能載入要等 describeModel()。
  static bool looksLikeModelPath(const QString& path);

protected:
  void dragEnterEvent(QDragEnterEvent* event) override;
  void dropEvent(QDropEvent* event) override;
  // 關窗時記下視窗位置與大小（%APPDATA%/live2d_mate/viewer.json）
  void closeEvent(QCloseEvent* event) override;
  bool eventFilter(QObject* watched, QEvent* event) override;
  // 系統切換淺／深色主題時把工具列圖示整組重畫（顏色是照調色盤染的，見 tinted_icon.h）
  void changeEvent(QEvent* event) override;

private:
  QString tr2(const char* key) const;

  // 視窗幾何的存與取。判斷邏輯（尺寸合不合理、螢幕沒了怎麼辦）在
  // core/viewer_settings.h，這兩支只負責讀檔／寫檔與問 QScreen。
  void restoreWindowGeometry();
  void saveWindowGeometry() const;

  void buildUi();
  void buildToolBar(QWidget* parent);
  void retranslate();
  void chooseModel();
  // 把現在這隻模型交給桌寵當桌寵。桌寵在跑就直接送給它並讀回成敗，
  // 沒在跑就啟動一個帶著 --set-model 的新實例（見 core/external_model.h）。
  void setAsPet();
  void refreshLists();
  void clearLists();
  // 狀態列的常駐那一句（模型名稱 ＋ 動作／表情數量）。載入完與按下重置之後都要
  // 回到這一句 —— showMessage() 沒有時限，上一則錯誤會一直掛在那裡，
  // 按了重置卻還寫著「動作『X』沒有播放」，就是這支函式要擋掉的不一致。
  void showModelStatus();
  void playMotionItem(QTreeWidgetItem* item);
  // 在模型上點一下的反應（ViewerCanvas::modelTapped）。與桌寵**共用同一套規則**
  // （core/interaction_logic.h 的 pickTapMotion ＋ ModelController::tapBodyPart），
  // 所以檢視器裡點到的那一段，就是「設定為桌寵」之後點下去會播的那一段。
  // 挑不出來時同樣退回隨機一段，點了不能沒反應。
  void playTapMotion(const QPointF& localPos);
  // 把清單的游標移到剛播的那一段 —— 檢視器的用途就是「這是哪一個動作？」，
  // 點了角色卻沒人告訴你答案等於白點
  void selectMotionItem(const std::string& group, int index);
  // 預覽播放的唯一入口：點清單與切換「自動重播」都走這裡
  // 回 false ＝ 沒播起來（優先權被擋、動作檔載入失敗）；狀態列已經報過錯了
  bool startPreviewMotion(const std::string& group, int index);
  void applyExpressionItem(QListWidgetItem* item);
  // 工具列那顆「重置動作與表情」。動作接回待機、表情兩層一起清掉，清單的游標
  // 也跟著回原點 —— 表情不是動作的一部分（一層在 Cubism 的表情管理、一層在
  // 參數覆寫層），只停動作的話那張笑臉會留在臉上，要自己去點「（不套用表情）」。
  void resetPlayback();
  void toggleFullScreen();
  void setDocked(bool docked);
  void setAutoReplay(bool on);
  // 工具列**六顆**按鈕的圖示一起重寫，狀態鈕那三顆連 checked 與 tooltip 也一起。
  // 分開寫的話總有一顆會忘了跟上（例如 F11 走鍵盤那條）。前三顆雖然不隨狀態變，
  // 卻要隨調色盤變（深色主題），而且必須是**新的 QIcon 物件**才繞得過 QPixmapCache
  // —— 理由寫在 tinted_icon.h。
  void updateToolBarState();

  // 拖放事件裡取第一個像模型的檔案；沒有就回空字串
  static QString firstModelPath(const QMimeData* mime);

  std::string locale_;
  ViewerCanvas* canvas_ = nullptr;
  CollapsibleSplitter* splitter_ = nullptr;
  QLabel* modelLabel_ = nullptr;
  QTreeWidget* motionTree_ = nullptr;
  QListWidget* expressionList_ = nullptr;
  QToolBar* toolBar_ = nullptr;
  QAction* openAction_ = nullptr;
  QAction* setAsPetAction_ = nullptr;
  QAction* resetAction_ = nullptr;
  QAction* dockAction_ = nullptr;
  QAction* fullScreenAction_ = nullptr;
  QAction* loopAction_ = nullptr;
  QGroupBox* motionBox_ = nullptr;
  QGroupBox* expressionBox_ = nullptr;

  // 清單現在是佔位（true，預設）還是浮在畫布上（false）
  bool docked_ = true;

  // 預覽動作要不要一直重播（預設關）。這個值在起播時才會用到 ——
  // 一路傳到 ACubismMotion::SetLoop，見 live2d/model_controller.h 的 startMotion。
  bool autoReplay_ = false;
  // 剛才預覽的那一段（空字串＝沒有）。切換自動重播時拿它當場重播一次，
  // 不然按了要等下一次點清單才看得出差別。
  std::string lastMotionGroup_;
  int lastMotionIndex_ = -1;

  // 目前這隻模型的描述（動作、表情、參數、命名）。載入失敗時保持上一份。
  std::optional<l2m::ModelInfo> model_;
  QString modelPath_;

  // 進全螢幕之前是不是最大化的 —— showNormal() 一律回到「還原」大小，
  // 沒記這一格的話本來最大化的視窗離開全螢幕會突然縮成小視窗。
  bool wasMaximized_ = false;
};
