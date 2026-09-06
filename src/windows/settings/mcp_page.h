#pragma once

// 設定視窗的「MCP」分頁。
// 整份移植自原本的 windows/mcp_window.*。
//
// 三個區塊：
//  1. 監聽設定（開關、位址、埠、token）——「綁到 127.0.0.1 以外一定要 token」
//     這條規則在這裡先擋一次，伺服器啟動時再擋一次（config.json 可以手改）。
//  2. 發話節奏（多話／完成時通知）—— 見下。
//  3. 連線片段 —— 每家 AI 應用的設定長得不一樣，集中在這裡產生並提供複製。
//
// **這一頁刻意保留「套用並重新啟動」按鈕，不跟其他分頁一樣即時套用。**
// 不是風格問題：host 與 token 有跨欄位驗證（非 loopback 一定要 token），
// 即時套用等於使用者把 host 改成 0.0.0.0、token 還沒開始打的那一瞬間就
// 把角色的控制權開到區網上；而且 token 是逐字打的，每個鍵擊都重啟一次
// httplib 伺服器（stop() 會 join 執行緒並放生所有未決呼叫）。
// 分頁上有一行 settings.mcp.applyHint 把這件事講出來。
//
// **例外中的例外：發話節奏那四個核取方塊是即時套用的**，不跟同頁其他欄位一起
// 等「套用」。理由與上面那段剛好相反：它們沒有跨欄位驗證，而且完全不影響伺服器
// 怎麼跑 —— 只改變 initialize 送出的 instructions 文字。跟著 apply 走的話，
// 使用者勾一個「多話」就會白白重啟一次 httplib 伺服器（stop() 會 join 執行緒
// 並放生所有未決呼叫），代價與收益完全不成比例。
// 代價是它們**不是立刻見效**：instructions 只在 initialize 送出，已連線的 AI
// 要重新連線才讀得到，所以區塊底下有一行 settings.mcp.speechHint 講明這件事
// （跟角色分頁「使用此角色」之後那句是同一個限制、同一種講法）。
//
// 對應的表單是 mcp_page.ui；連線片段區由程式碼建，而且是**第一次切到這一頁
// 才建**（十幾個 QPlainTextEdit，先建好會讓開窗變慢）。
// 發話節奏那四個核取方塊則是 Designer 靜態的，只有文字與勾選狀態由程式碼填。
//
// **捲動區在最外層**（scrollArea 包住整頁，比照 general_page.ui）：三個區塊
// 加起來早就超過視窗高度，捲軸只包住片段區的話，使用者要看第三家的設定得先
// 在一個被壓成一小條的框裡捲 —— 而上面兩個群組明明是填一次就不會再動的。
// 連帶 clientsGroup 底下不再有自己的捲動區，**群組高度跟著片段內容長**，
// 想比對兩家的片段直接往下捲整頁就好。內容比視窗短的時候，尾端那個 tailSpacer
// 負責把剩下的空間吃掉 —— 沒有它，三個群組會被 QVBoxLayout 平均拉長。

#include <QWidget>

#include <memory>
#include <string>
#include <vector>

#include "settings_context.h"

class QCheckBox;
class QLayout;

namespace Ui {
class McpPage;
}

namespace l2m {

class McpHttpServer;

class McpPage : public QWidget {
  Q_OBJECT

public:
  McpPage(const SettingsContext& context, McpHttpServer& server, QWidget* parent = nullptr);
  ~McpPage() override;

  void retranslate();
  void refresh();
  // 伺服器狀態變動很頻繁；沒被切到這一頁就不必跟著重建片段區
  void refreshIfActive();

private:
  QString tr2(const char* key) const;

  void reloadForm();
  void rebuildSnippets();
  void updateStatus();
  void updateWarnings();
  void apply();
  // 目前表單上選的位址（可編輯下拉，使用者能自己打）
  std::string selectedHost() const;
  // 發話節奏的核取方塊，順序與 core/settings_layout.h 的 mcpToggles() 對齊。
  // 那張表才是「哪個核取方塊寫哪個 config 欄位」的真相來源（測試驗的是它），
  // 這裡只負責把 Designer 建好的 widget 對到表上的位置。
  std::vector<QCheckBox*> speechBoxes() const;

  SettingsContext ctx_;
  McpHttpServer& server_;
  std::unique_ptr<Ui::McpPage> ui_;
  bool updating_ = false;
};

}  // namespace l2m
