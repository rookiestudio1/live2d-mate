#pragma once

// 系統匣選單。
//
// 大部分的設定已經搬到 windows/settings/（QTabWidget 五分頁），這裡只留
// **高頻的一次性操作**：大小、透明度、位置，加上「設定…」與「離開」。
// 動作與表情選單一併移除 —— 它們靠 stateChanged 重建，而閒置隨機動作每次
// 觸發都會發 stateChanged，等於讓整個選單為了兩個子選單一直重建。
//
// 選單仍然是每次狀態變更整份重建，不去局部更新 QAction：免得選單勾選狀態
// 和實際設定慢慢對不上；語系切換也是靠這個整份重建生效。
//
// MCP 的狀態改放在圖示的 tooltip（見 updateToolTip）：它沒有適合的選單位置了，
// 但「伺服器在跑嗎」是 AI 連不上時第一個要確認的事，不能只在設定視窗裡看得到。

#include <QMenu>
#include <QObject>
#include <QSystemTrayIcon>
#include <QTimer>

#include <functional>
#include <memory>
#include <string>

#include "core/command_result.h"
#include "core/settings_layout.h"

namespace l2m {

class AppController;

class Tray : public QObject {
  Q_OBJECT

public:
  // 系統匣要用到、但不屬於 AppController 的能力，由 main 注入。
  // 這樣 AppController 就不必反過來認識設定視窗與 MCP 伺服器。
  struct Deps {
    // 開啟設定視窗並切到指定分頁
    std::function<void(SettingsTab)> openSettings;
    // MCP 狀態：做成圖示的 tooltip，不佔選單列
    std::function<bool()> mcpEnabled;
    std::function<bool()> mcpRunning;
    std::function<std::string()> mcpUrl;
    std::function<void()> quit;
  };

  Tray(AppController& controller, Deps deps, QObject* parent = nullptr);

  void rebuild();

  // 系統匣氣球通知。**刻意不是 modal 對話框**：兩個呼叫端（「上次異常結束」的提示、
  // 模型載入失敗）都一定發生在剛開機的時候 —— 桌寵才剛出現就跳一個要按確定的視窗
  // 太兇。點通知會叫 onClicked（Windows 上使用者忽略通知是常態，所以那個 callback
  // 不保證會被呼叫）
  // onClicked 可以不給（點了不做事）。std::function 的預設引數不受「巢狀型別 NSDMI
  // 不可當預設引數」那條 clang 限制影響，見 CLAUDE.md。
  void notify(const QString& title, const QString& body, std::function<void()> onClicked = {});

public slots:
  // MCP 狀態顯示在圖示的 tooltip 而不是選單列。
  //
  // 順帶的好處：McpHttpServer::statusChanged 是 direct connection，接到這裡
  // 就不必為了一行狀態整份重建選單，重入視窗縮到最小。
  //
  // 注意：這個 slot 會回頭呼叫 deps_.mcpRunning() / mcpUrl()，也就是
  // McpHttpServer::status()，而它會拿 statusMutex_。之所以不會重入，是因為
  // setStatus() 刻意在解鎖之後才 emit（見 mcp/mcp_http_server.h）。
  // 新增任何 statusChanged 的接收端時都要重新確認這條還成立。
  void updateToolTip();

private:
  // 位置變更等事件會頻繁觸發，聚合一下再重建
  void scheduleRebuild();

  void buildScaleMenu(QMenu* menu);
  void buildOpacityMenu(QMenu* menu);
  void buildPositionMenu(QMenu* menu);

  QString tr2(const char* key) const;

  // 執行一個命令；失敗時把 error 與 hint 一起顯示出來。
  // 系統匣與 MCP 走同一條路徑，這裡就是「人類版」的錯誤呈現。
  void run(const CommandResult& result);

  AppController& controller_;
  Deps deps_;
  QSystemTrayIcon icon_;
  std::unique_ptr<QMenu> menu_;
  QTimer rebuildTimer_;
  // 選單開著時來的重建請求先記著，收起來（aboutToHide）後再補做
  bool rebuildPending_ = false;
};

}  // namespace l2m
