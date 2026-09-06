#pragma once

// 設定視窗的「角色」分頁。
//
// 左半是角色描述的清單，右半是**上下七個多行輸入框**（角色描述／隨機台詞／
// 歡迎詞／時段問候／久坐提醒／摸摸反應，各配標籤與字數計數器），最上面一條
// 按鈕列（使用角色／不使用角色／新增／儲存／刪除）。一份角色仍然是
// personas/<名稱>.md 一個純文字檔 —— 載入時由 parsePersonaDoc 把七個 # 區塊
// 拆進七個框，存檔時 serializePersonaDoc 吐回去；**英文區塊標題完全不出現在
// 畫面上**。保留字清單裡還沒有對應欄位的區塊（doc.reserved，目前是空集合）
// 在載入時原樣暫存，存檔時跟著寫回，不會被吃掉。
// 分區規則與長度上限在 core/persona_doc.h。
//
// **七個框包在一個 QScrollArea 裡**（editorScroll，做法比照 general_page.ui）：
// 「使用中：X」與提示文字釘在頂端不捲 —— 捲到最下面時還是要看得出現在編的
// 是哪一份角色。有兩件事不能省，缺一個捲軸就永遠不會出現：
//  * 七個 QPlainTextEdit 各給明確的 minimumSize.height（描述 140、隨機台詞
//    100、其餘四個 72）。widgetResizable 的 QScrollArea 把內容 widget 撐成
//    max(viewport, 佈局最小高)，而 QPlainTextEdit 的 minimumSizeHint 只有
//    兩三行 —— 不給下限的話七個框會一起被壓扁成一條縫，內容永遠塞得下。
//  * editorLayout 的 stretch 維持 3:2:1:1:1:1 —— 視窗夠高時多出來的空間仍然
//    優先給描述與隨機台詞，跟包捲軸之前的手感一樣。
//
// 為什麼放在「模型」右邊：模型決定這隻桌寵長什麼樣，角色決定它講話是什麼樣，
// 兩個都是「這隻桌寵是誰」的設定。
//
// 五個必須保留的行為（前兩個是模型分頁已經踩過的同一個坑），七個輸入框
// 一體適用：
//
//  1. **選取清單只載入，不套用**。套用是「使用角色」按鈕。選取即套用的話，
//     使用者只是想看看別份寫了什麼，AI 的個性就被換掉了。
//
//  2. **refresh() 只在選取真的換了才重載輸入框**（一次載入七個框）。存檔本身
//     也會發 personaChanged → refreshIfActive()，無條件重載會把使用者正在打的
//     字連同游標位置一起洗掉。清單與上方的「使用中」標籤則每次都更新。
//
//  3. **長度上限用「擋住儲存」實作，不是截斷**。QPlainTextEdit 沒有
//     maxLength，打字打到一半被截斷會吃掉輸入並讓游標彈到別的地方。
//     七個框各自算字數，**任一超限就把儲存鈕變灰**，超限那一個的計數器
//     轉警示色 —— 使用者要看得出來是哪一區超了。描述的上限是 2000
//     （進 AI 的 prompt），台詞區寬鬆得多（見 persona_doc.h）。
//
//  4. **有未儲存的變更就先問**。dirty ＝ 七個框任一與載入時不同。
//
//  5. **「不使用角色」不跳確認框，但「刪除」要**。兩者的代價差一個數量級：
//     停用只是把 config 的 persona.current 清成 null，.md 檔一個字都不會動，
//     想反悔就再按一次「使用此角色」；刪除是真的從磁碟上移除檔案。
//     它與「使用此角色」是同一組動作的正反面，所以行為也要對稱 ——
//     那一顆也是按下去就生效，不問。
//     停用走的是 usePersona("")，跟 deleteSelected() 刪掉套用中那一份時
//     清設定的路徑是同一條，不另外開一個 API。
//     只在**真的有套用中的角色**時才 enable：沒有角色可停用的時候，
//     一顆按得下去卻什麼都不會發生的按鈕只會讓人以為壞掉了。
//
// 「使用角色」與「不使用角色」之後都要提醒使用者：已經連線的 AI 要重新連線才會讀到。
// 這不是偷懶 —— MCP 的傳輸是 POST-only 的 httplib（沒有 SSE），伺服器端
// 推不了 notifications/*，instructions 與工具說明只在連線交握時送一次。
// 理由與三條通道的分工寫在 core/mcp_resources.h。
//
// 對應的表單是 persona_page.ui，全部控制項都是 Designer 靜態的
//（清單項目由程式碼填，但沒有動態產生的 widget）。

#include <QPointer>
#include <QString>
#include <QWidget>

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "core/persona_doc.h"
#include "core/persona_generate.h"
#include "settings_context.h"

class QLabel;
class QPlainTextEdit;
class QProgressDialog;

namespace Ui {
class PersonaPage;
}

namespace l2m {

class PersonaPage : public QWidget {
  Q_OBJECT

public:
  PersonaPage(const SettingsContext& context, LlmDeps llmDeps, QWidget* parent = nullptr);
  ~PersonaPage() override;

  void retranslate();
  void refresh();
  void refreshIfActive();

private:
  QString tr2(const char* key) const;

  void rebuildPersonaList();
  // 上方那行「使用中：X」。單純 setText，不會動到輸入框，
  // 所以 refresh() 的「沒換就不重載」提前返回之前就能先更新它
  void updateActiveName();
  // 把某一份的內容載進輸入框，並把 dirty 狀態歸零
  void loadIntoEditor(const std::string& name);
  // 字數計數器與儲存鈕的啟用狀態（兩者由同一條長度規則決定）
  void updateEditorState();
  void updateButtons();

  // 七個輸入框的目前內容組回 PersonaDoc（reserved 用載入時暫存的那一份）
  PersonaDoc docFromEditors() const;
  // 單一輸入框的計數器：現長/上限，超限轉警示色。回傳是否超限。
  bool updateCounter(QLabel* counter, QPlainTextEdit* edit, int maxChars);
  // 七個框任一超限（儲存鈕以此變灰）
  bool anyTooLong() const;

  void saveCurrent();
  void useSelected();
  // 停用套用中的角色：設定清成 null，MCP 的 instructions／工具說明退回沒有角色的版本
  void clearActive();
  void createNew();
  void deleteSelected();
  // AI 擴寫（LLM 第二期）：同一場對話分六階段各生成一區
  //（core/persona_generate.h 的狀態機），全部完成後預覽、確認才**填進編輯器**
  // —— 不直接寫檔，儲存仍然是使用者按的那一下
  void generateWithAi();
  // 發出目前階段的請求；回覆合格就前進並遞迴到下一階段，全部完成開預覽
  void runGenerateStage();
  // 生成收尾（成功、失敗、取消共用）：關進度框、恢復按鈕
  void finishGenerate();
  QString stageLabel(PersonaGenStage stage) const;
  // LLM 回呼裡要開 modal 一律走這裡（排到下一輪事件迴圈）。
  // **直接開會讓 QNetworkReply 死在自己的訊號裡** —— 理由與堆疊寫在 .cpp。
  void modalLater(std::function<void()> fn);
  void showGeneratePreview(const PersonaDoc& doc);
  void applyGeneratedDoc(const PersonaDoc& doc);
  // 開啟長期記憶資料夾（memory/<角色名>.md；core/persona_memory.h）
  void openMemoryFolder();

  // 有未儲存的變更時先問。要繼續切換回 true，使用者按取消回 false。
  bool confirmDiscardChanges();

  std::string selectedPersonaName() const;
  void selectRowFor(const std::string& name);

  SettingsContext ctx_;
  LlmDeps llmDeps_;
  std::unique_ptr<Ui::PersonaPage> ui_;
  // 進行中的 AI 擴寫（一次一個；再按一次會取消上一個）。
  // session 是階段狀態機；回呼用「還是同一個 session」判定自己過不過期
  std::shared_ptr<PersonaGenerateSession> generateSession_;
  std::unique_ptr<LlmRequestHandle> generateHandle_;
  // 生成中的進度對話框（確定進度：第幾階段/共幾階段；取消鈕會中止請求）。
  // QPointer：使用者按取消時它自己關掉，回呼那側不能對著懸空指標 close
  QPointer<QProgressDialog> generateProgress_;
  // 輸入框目前顯示的是哪一份；只有它變了才重載
  std::string shownName_;
  // 載入時七個框的內容，用來判斷有沒有被改過（dirty ＝ 任一不同）
  QString loadedDescription_;
  QString loadedLines_;
  QString loadedWelcome_;
  QString loadedGreetings_;
  QString loadedBreaks_;
  QString loadedPetted_;
  QString loadedWeatherAlerts_;
  // 載入時解析出的未來保留區塊（doc.reserved），存檔時原樣寫回 ——
  // 七個具名區塊都有輸入框之後，這是唯一還需要暫存的一份
  std::vector<std::pair<std::string, std::string>> loadedReserved_;
  // 取消切換時要跳回去的那一列
  int previousRow_ = -1;
  bool dirty_ = false;
  bool updating_ = false;
};

}  // namespace l2m
