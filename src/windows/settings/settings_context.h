#pragma once

// 設定分頁共用的環境。
//
// 設定視窗把「怎麼套用、怎麼回報、語系換了要通知誰」注入給每個分頁，
// 分頁就不必反過來認識 SettingsWindow —— 與 Tray::Deps、AppController 的
// public std::function 成員是同一套「注入能力、保持相依無環」的慣例。

#include <QAbstractItemModel>
#include <QComboBox>
#include <QScrollArea>
#include <QString>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "core/command_result.h"
#include "core/config_schema.h"
#include "core/llm_types.h"
#include "core/tts_types.h"

namespace l2m {

class AppController;

struct SettingsContext {
  AppController* controller = nullptr;
  // 套用一段兩層 patch JSON。失敗時錯誤訊息已經顯示在狀態列了，回 false，
  // 呼叫端負責把控制項復原成設定的現值 —— 畫面和實際設定不一致比顯示錯誤更糟。
  std::function<bool(const std::string&)> applyPatch;
  // 底部狀態列（1.5 秒後自動清掉）
  std::function<void(const QString&)> setStatus;
  // 執行一個命令；失敗時把 error 與 hint 一起彈出來。
  // 系統匣與 MCP 走同一條路徑，這是「人類版」的錯誤呈現。
  std::function<void(const CommandResult&)> run;
  // 語言換掉之後，整個設定視窗與系統匣的文字都要重設
  std::function<void()> retranslateAll;
};

// 語音清單是非同步查的，而且不屬於 AppController（是 SpeechController 與
// TtsManager 的能力），比照 Tray::Deps 由 main 注入。
struct VoiceDeps {
  std::function<void(std::function<void(std::vector<TtsEngineInfo>)>)> listEngines;
  std::function<void(std::function<void(std::vector<VoiceInfo>)>)> listVoices;
  // 「重新偵測語音」：清掉可用性快取
  std::function<void()> resetVoiceCache;
};

// LLM 相關的注入能力。與 VoiceDeps 同理由由 main 注入：
// HttpJson 與 LlmManager 都是 main() 的 stack local。
struct LlmDeps {
  // LLM 分頁的「測試連線」。吃的是**表單上的設定**而不是存檔的 config ——
  // 使用者要測的是自己剛打的 URL 與金鑰，還沒按套用。
  std::function<void(const LlmConfig&, std::function<void(std::vector<std::string> models, std::string error)>)> listModels;
  // 角色分頁的「AI 擴寫」。走 LlmManager 的存檔設定（擴寫是「用你設好的 LLM
  // 幫忙寫」，不是連線測試）；未啟用時 done 直接收到錯誤。回傳 handle 可取消。
  std::function<std::unique_ptr<LlmRequestHandle>(std::vector<LlmMessage> messages, LlmChatOptions options, std::function<void(std::string text, std::string error)> done)> chatBuffered;
};

// 氣泡預覽。氣泡平常只在說話時出現，所以「與角色的間距」這個設定在調整當下
// 畫面上是空的，等於盲調 —— 一般分頁改動時借氣泡視窗顯示一句預覽，放手後收掉。
// 與 VoiceDeps 同理由 main 注入而不是掛在 AppController 上：BubbleWindow 是
// main() 的 stack local，AppController 不認識它。
struct BubbleDeps {
  std::function<void(const QString& text)> showPreview;
  std::function<void()> hidePreview;
  // 正在說台詞時就不要搶氣泡了 —— 真的台詞本來就看得到效果
  std::function<bool()> speaking;
};

// 天氣分區的注入能力。與 VoiceDeps / BubbleDeps 同理由 main 注入：
// WeatherService 是 main() 的 stack local，AppController 不認識它。
struct WeatherDeps {
  // 城市名 → 座標（地點欄位的「套用」按鈕）。查詢要打網路，所以是非同步的，
  // 也因此這個欄位**刻意不即時套用** —— 每個鍵擊查一次 geocoding 是災難
  //（與 GPT-SoVITS 的伺服器位址同一條理由）。
  // done(ok, latitude, longitude, resolvedName, error)
  std::function<void(const std::string& name, std::function<void(bool, double, double, std::string, std::string)> done)> resolveCity;
  // 現在的天氣，一行英文（空＝還沒有資料）
  std::function<std::string()> summary;
  // 最近一次失敗的原因（空＝沒有錯誤）
  std::function<std::string()> lastError;
};

// 模型頁與角色頁左右兩欄的預設寬度種子（QSplitter::setSizes 的參數）。
//
// 兩頁的左半邊結構一模一樣（標題 + QListWidget，minimumSize 都是 180），
// 右半邊卻不同（命名捲動區 vs 角色編輯器）。而 QSplitter 的初始分配是
// 「先給兩邊各自的 sizeHint，剩下的才照 stretch 分」，所以只寫
// setStretchFactor(0, 1)/(1, 2) 的話宣告的 1:2 根本不會成立，
// 兩頁的清單也就對不齊（視窗 920 px 寬時實測：模型頁 293、角色頁 281，
// 而且差多少還跟語系有關 —— 右半邊的提示字換一種語言長度就變了）。
//
// 兩頁餵同一組種子把 sizer 釘住，比例才真的成立。值必須明顯大於兩欄的
// minimumSizeHint：試過 setSizes({1, 2}) 想「只表達比例」，結果兩邊都被夾到
// 各自的最小寬度，剩餘空間的分法改由「兩邊最小寬度的差」主導，反而從差 12 px
// 變成差 98 px（實測 612 vs 514）。
constexpr int kListPaneWidth = 280;
constexpr int kDetailPaneWidth = kListPaneWidth * 2;

// 程式化更新控制項期間，擋掉自己發出的訊號。
//
// 沒有這個的話：refresh() 的 setChecked() 會發 toggled → 處理器去 patch →
// ConfigStore 發 changed → refresh() → setChecked() … 繞不完。
// ConfigStore「序列化結果真的有變才 emit」只擋得住整數與布林；語速／音量是
// 滑桿值除以 100，浮點往返的誤差會讓它每一圈都「真的有變」。
class ScopedUpdate {
public:
  explicit ScopedUpdate(bool& flag) : flag_(flag) { flag_ = true; }
  ~ScopedUpdate() { flag_ = false; }
  ScopedUpdate(const ScopedUpdate&) = delete;
  ScopedUpdate& operator=(const ScopedUpdate&) = delete;

private:
  bool& flag_;
};

// 讓 QScrollArea 的底色跟著它所在的那一頁，而不是自成一塊灰色面板。
//
// QScrollArea 的 viewport 是 autoFillBackground(true) ＋ backgroundRole 為
// QPalette::Window，而 QScrollArea::setWidget() 會回頭把交給它的內容 widget 也
// 設成 autoFillBackground(true)（同樣是 Window 色）—— 兩層都在平塗 Window。
// 問題是分頁面板不是 Window 色：Qt 6.11.2 ＋ windows11 樣式實測，分頁底是
// #ffffff 而 Window 是 #f3f3f3，於是四個捲動區（一般／模型／MCP／角色）在白色
// 的分頁上各印出一塊灰底方框。（Qt 6.8.3 上兩者都是 #fbfbfb，看不出來，所以這
// 是換到 6.11 才浮現的。）
//
// 改法是把兩層的 autoFillBackground 都關掉，讓它們完全不畫背景、露出分頁已經
// 畫好的像素 —— 實測改完捲動區量到 #ffffff，與分頁一致。**不改成指定顏色**：
// 分頁面板是樣式用 PE_FrameTabWidget 畫的，那塊底色不保證等於調色盤裡的任何
// 一個角色（這次就正好不等於 Window），寫死顏色只是換一種對不上的方式。
//
// **viewport 與內容 widget 兩個都要關**：只關 viewport 的話，蓋在上面那層仍然
// 平塗 Window，畫面一點也不會變。也因為是 setWidget() 動的手，模型頁那種
// **執行期換內容**的（model_page.cpp 每次 refresh 都 setWidget）必須在每一次
// setWidget 之後再呼叫一次，只在建構子呼叫會被下一次換內容覆蓋掉。
inline void blendScrollAreaBackground(QScrollArea* scroll) {
  if (!scroll) return;
  if (QWidget* viewport = scroll->viewport()) viewport->setAutoFillBackground(false);
  if (QWidget* content = scroll->widget()) content->setAutoFillBackground(false);
}

// 清空下拉的項目 —— 刻意不用 QComboBox::clear()。
//
// Qt 6.8.3 的 clear() 是「先把 row 全部移掉，再補一則 accessibility 的
// value-change 事件」：
//
//   void QComboBox::clear() {
//     d->model->removeRows(0, d->model->rowCount(d->root), d->root);
//     QAccessibleValueChangeEvent event(this, QString());
//     QAccessible::updateAccessibility(&event);   // ← 問題在這一則
//   }
//
// 事件送出的當下項目已經不在了。只要系統上有 UI Automation 客戶端連著
// （朗讀程式、語音存取、螢幕內容擷取那一類），qwindows 的 UIA 橋接就會同步
// 把這則事件送進 UiaRaiseAutomationPropertyChangedEvent，UIA 立刻反過來呼叫
// UiaNodeFromProvider 去問那些剛被刪掉的項目 —— provider 在呼叫還在飛的時候
// 被解構，於是純虛擬函式呼叫 → abort()，行程以 0xC0000409 死掉。
// 實際踩到的堆疊（位址是從 Release 版反組譯逐格對回來的）：
//
//   SettingsWindow::refreshCurrentPage → VoicePage::refresh
//     → VoicePage::rebuildVoiceCombo → QComboBox::clear
//     → QAccessible::updateAccessibility → UiaRaiseAutomationPropertyChangedEvent
//     → UiaNodeFromProvider → …… → QObject::~QObject → purecall → abort
//
// 第一次切到語音分頁時 refreshCurrentPage() 會一前一後跑 retranslate() 與
// refresh()，兩邊都重建一次下拉，那顆「偵測中…」項目的生與滅只隔幾微秒，
// 剛好落在 UIA 還握著它的窗口裡 —— 所以症狀是偶發，而且只在有無障礙客戶端
// 的機器上出現。
//
// 這裡直接走 model：removeRows 那一段與 clear() 一模一樣（我們沒有用
// setRootModelIndex，root 就是預設的無效 index），少的只有那則事後事件。
// 清空之後一定會重新填項目並 setCurrentIndex()，那一步自己會發 value-change，
// 朗讀程式讀到的最終狀態不變。
inline void clearComboItems(QComboBox* combo) {
  if (!combo) return;
  QAbstractItemModel* model = combo->model();
  if (!model) return;
  model->removeRows(0, model->rowCount());
}

}  // namespace l2m
