#pragma once

// 設定視窗 —— 這個 app 大部分設定的 UI。
//
// 由原本的 windows/mcp_window.*（MCP 分頁）與 windows/naming_window.*
//（模型分頁右半）整併而來，再加上原本散在系統匣「一般」「模型」「語音」
// 子選單裡的項目。系統匣只留高頻的一次性操作（大小／透明度／位置）。
//
// 骨架走 Qt Designer（settings_window.ui ＋ 五個 *_page.ui），動態內容
//（模型清單、引擎／語音下拉、命名列、連線片段、關於欄位）由程式碼填。
//
// 分頁刻意各自是一個 QWidget 子類，不是塞在同一個類別裡的五組 Ui 物件：
// 四個分頁掛的訊號完全不同（模型頁掛 modelsChanged、MCP 頁掛 statusChanged、
// 語音頁掛非同步 TTS 回呼），塞在一起的話「哪個訊號該重建哪一塊」很快就
// 對不上；而且相依面也不同，只有 MCP 分頁需要 McpHttpServer&。
//
// 分頁內容是**延遲建立**的：open() 只 refresh 目前那一頁，其餘等被切到時
// 才由 currentChanged 補做。MCP 的連線片段與命名區都是上百個 widget，
// 全部先建好會讓開窗明顯變慢。
//
// 沒有 retranslateUi：本專案的 i18n 是執行期 JSON 而不是 Qt Linguist，
// 語系切換靠 retranslate() 整份重設文字（與舊的兩個視窗「open() 時整份
// 重設」同一條規則，只是多了「在一般分頁換語言就地生效」）。

#include <QWidget>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/settings_layout.h"
#include "settings_context.h"

namespace Ui {
class SettingsWindow;
}

namespace l2m {

class AboutPage;
class AppController;
class GeneralPage;
class LlmPage;
class McpHttpServer;
class McpPage;
class ModelPage;
class PersonaPage;
class VoicePage;

class SettingsWindow : public QWidget {
  Q_OBJECT

public:
  SettingsWindow(AppController& controller, McpHttpServer& server, VoiceDeps voiceDeps, BubbleDeps bubbleDeps, LlmDeps llmDeps, WeatherDeps weatherDeps, QWidget* parent = nullptr);
  // ui_ 持有的是不完整型別，解構子必須離線定義
  ~SettingsWindow() override;

  // 開啟（或帶到最前面）並切到指定分頁。
  // 系統匣的「設定…」走 General，MCP 的 open_naming_editor 走 Model。
  void open(SettingsTab tab = SettingsTab::General);

  // 外部狀態變了（目前只有天氣服務）：視窗開著才重讀目前這一頁。
  // 與各分頁自己的 refreshIfActive() 同一條規則 —— 沒在看的東西不必重畫。
  void refreshVisible();

  // main 指派：在一般分頁換語言之後，系統匣的文字也要跟著換。
  // Tray 比 SettingsWindow 晚建，所以用事後指派而不是建構子參數。
  std::function<void()> trayRetranslate;

private:
  // 語系換掉：視窗標題與分頁標題就地重設，分頁內容等被切到時才重建
  void retranslate();
  void retranslateTabBar();
  // 只重讀目前這一頁；其餘等被切到時才做
  void refreshCurrentPage();
  QWidget* pageFor(SettingsTab tab) const;

  // 套用一段兩層 patch JSON；失敗時把訊息顯示在狀態列並回 false
  bool applyPatch(const std::string& patchJson);
  void setStatus(const QString& text);

  QString tr2(const char* key) const;

  AppController& controller_;
  std::unique_ptr<Ui::SettingsWindow> ui_;

  GeneralPage* general_ = nullptr;
  ModelPage* model_ = nullptr;
  PersonaPage* persona_ = nullptr;
  VoicePage* voice_ = nullptr;
  McpPage* mcp_ = nullptr;
  LlmPage* llm_ = nullptr;
  AboutPage* about_ = nullptr;

  // 每個分頁「已經翻成哪個語系」；空字串代表內容還沒建過。
  // 與 settingsTabs() 同索引。
  std::vector<std::string> pageLocales_;
};

}  // namespace l2m
