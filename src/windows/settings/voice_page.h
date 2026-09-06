#pragma once

// 設定視窗的「語音」分頁。
// 內容來自原本系統匣的「語音」子選單（windows/tray.cpp buildVoiceMenu）。
//
// 與系統匣版的兩個差異：
//  1. 語速與音量改成滑桿的連續值。選單裝不下，所以以前只給 4 檔；
//     schema 允許 tts.rate 0.5~2、tts.volume 0~2。音量滑桿的 0~100% 映射到
//     增益 0~2 —— 50% 就是原始音量（增益 1，預設值），拉超過 50% 是軟體放大，
//     給音源偏小聲的引擎用（見 media/audio_player.cpp 的 set_master_volume 註解）。
//  2. 引擎與語音是**就地更新的下拉**，不是每次整份重建的選單。因此多了一個
//     系統匣沒有的問題：使用者快速換兩次引擎時，前一個引擎的語音清單可能
//     晚一步回來把新的蓋掉。用 voiceRequestEngine_ 比對後丟棄過期的回覆。
//
//  3. 「測試語音」的內容是**可編輯的輸入框**，預設填 settings.voice.sampleText。
//     試聽的重點常常是某個特定句子唸得對不對（多音字、外來語、數字），
//     寫死一句範例只驗得到「有沒有聲音」。這段文字**不寫進 config.json** ——
//     它是當下的試聽素材，不是設定。
//     旁邊的「測試思考」共用同一段文字，只是把 SpeakRequest::thinking 打開，
//     走 MCP think 工具的同一條路：思考泡泡＋帶殘響的聲音＋嘴巴不動。
//     那三件事湊起來對不對沒辦法用看的，只能真的播一次。
//
//  4. 「伺服器位址」與「自定義語音設定」兩組 GroupBox **常駐顯示**，改動累積在
//     表單裡，按右下角的「套用」才寫進 config。這一頁只有這兩塊不即時套用。
//
//     原本的設計是「只在引擎選到 custom 時顯示 ＋ 400 ms 防抖即時寫入」，
//     症狀是**改了 URL／參數不會生效，要把引擎切走再切回來**。三個縫：
//      - customParams／customHeaders 是 QPlainTextEdit，沒有 editingFinished
//        這種可靠的提交點，只能靠防抖。防抖沒到期就按「測試語音」，送出的是舊值。
//      - hideEvent() 的補寫是唯一的強制 flush ——「切走再切回來」之所以有效，
//        就是因為它順帶觸發了那條路徑。
//      - 寫入後 ConfigStore::changed 是**同步**的，會立刻繞回 refresh()，
//        setPlainText() 把游標打回開頭，使用者接著打的字就插錯位置。
//     自由文字欄位沒有可靠的提交點，所以改成明確的套用按鈕：使用者知道何時生效，
//     行為也才驗得到。引擎端本來就是每次合成現取設定（media/tts_engine_custom.h），
//     按下套用的下一句話就會用新設定。
//
//     GroupBox 常駐之後，本專案就**沒有任何依 engine 切 widget 可見性的地方**了。
//     引擎選到 custom 時語音下拉會停用（rebuildVoiceCombo），那是「這個引擎沒有
//     語音清單 API」，不是可見性。
//
//     「套用」按鈕只在**表單與設定不一致**時亮起（customDirty，每次現算）。
//     設定不完整（缺 URL 或 ${TEXT}）**照樣可以套用** —— 使用者常常先貼 URL
//     再回頭補參數，擋下來只會逼人在一次編輯裡湊齊。
//     驗證訊息（customHint 的紅字）直接用 buildCustomTtsRequest 的 error，
//     與按「測試語音」失敗時彈出的是**同一句**，只有一個真相來源。
//
//  5. 「伺服器位址」是 GPT-SoVITS 與 Voicebox 的 baseUrl（`http://主機:埠`），
//     以前只能手改 config.json。兩個位址共用一顆「套用」按鈕，一次送出同一份
//     tts patch（core/config_patch.h 的 ttsLocalServerPatch）—— 分兩包送會讓
//     ConfigStore 在中途以半套設定重跑一輪。
//
//     **不即時套用**的理由與自定義語音同一條：URL 是自由文字，打到一半的
//     `http://192.168.1.` 每個鍵擊都寫進 config 毫無意義，而且 ConfigStore::changed
//     是同步的，會立刻繞回 refresh() 把游標打回開頭。
//
//     套用成功之後**一定要重跑一次偵測**（redetect()）：位址換了，可用性與語音
//     清單都是舊主機的答案，不重問的話畫面會一直掛著「（不可用）」，
//     使用者只會以為自己填錯了。
//
//  6. **引擎不可用不再 disable 下拉項目**，只在標籤後面加「（不可用）」。
//     偵測是「現在連不連得上」的快照，而使用者常常是**先填位址、再去開服務**；
//     選不了就等於逼人先把服務跑起來才能設定它。選了不可用的引擎頂多是
//     說話時收到一句有 hint 的錯誤，比整個選項按不下去好解釋。
//
// **絕對不要把 refreshTtsInfo() 掛在 ConfigStore::changed 或每次 refresh()**：
// 引擎可用性偵測要連線，那等於說一句話就打一輪網路。只在「第一次開這一頁」
//「換引擎」「按重新偵測」三處呼叫（這條規則原本寫在 windows/tray.h）。
//
// 對應的表單是 voice_page.ui；下拉的項目由程式碼填。

#include <QWidget>

#include <memory>
#include <string>
#include <vector>

#include "core/config_schema.h"
#include "core/tts_types.h"
#include "settings_context.h"

class QTimer;

namespace Ui {
class VoicePage;
}

namespace l2m {

class VoicePage : public QWidget {
  Q_OBJECT

public:
  VoicePage(const SettingsContext& context, VoiceDeps deps, QWidget* parent = nullptr);
  ~VoicePage() override;

  void retranslate();
  void refresh();
  void refreshIfActive();

private:
  QString tr2(const char* key) const;

  // 重新查引擎與語音清單。只在「第一次開這一頁」「換引擎」「按重新偵測」
  //「套用伺服器位址」呼叫。
  void refreshTtsInfo();
  // 清掉可用性快取再重問一輪。「重新偵測語音」按鈕與「套用伺服器位址」共用 ——
  // 換了主機還讀舊快取的話，畫面上的「（不可用）」永遠不會消失。
  void redetect();
  void rebuildEngineCombo();
  void rebuildVoiceCombo();
  void updateSliderLabels();
  void applySliders();

  // ── 伺服器位址（GPT-SoVITS／Voicebox）──
  // 由目前**輸入框裡的值**組出來的 tts 設定。從 config 那一份複製再蓋掉兩個
  // baseUrl，presets 與 timeoutMs 這些沒有 UI 的欄位才不會被整包 patch 洗掉。
  TtsConfig serverFromForm() const;
  // 表單與設定不一致？「套用」按鈕亮不亮就看這個（理由同 customDirty）
  bool serverDirty() const;
  // 把設定灌回兩個位址欄位。呼叫點與 reloadCustomForm() 一樣只有兩個：
  // 第一次進這一頁，以及 applyPatch 失敗後的復原。
  void reloadServerForm();
  void applyServers();
  void updateServerApplyState();

  // ── 自定義語音 ──
  // 只填三個方法選項與它們的翻譯。**不負責選回 config 的值** —— 那是回灌表單，
  // 屬於 reloadCustomForm() 的事；混在一起的話換語系就會把使用者還沒套用的
  // 方法選擇洗掉。填完之後把原本選的那一項選回來。
  void rebuildMethodCombo();
  // 由目前**輸入框裡的值**組出來的設定（不是 config）：套用之前就要能驗證
  CustomTtsConfig customFromForm() const;
  // 表單與設定不一致？「套用」按鈕亮不亮就看這個。每次現算而不是存成員 ——
  // 四個字串比較很便宜（每個鍵擊本來就要跑一次 buildCustomTtsRequest），
  // 多一個 dirty_ 成員只是多一個會跟畫面不同步的真相來源。
  bool customDirty() const;
  // 把設定灌回四個自訂欄位。**只有兩個呼叫點**：第一次進這一頁，
  // 以及 applyPatch 失敗後的復原。其餘時候不 dirty 就恆等於 no-op，
  // dirty 就代表使用者有還沒套用的編輯，不能沖掉。
  void reloadCustomForm();
  // 按下「套用」才寫進 config
  void applyCustom();
  void updateCustomHint();
  // 紅字 ＋「套用」按鈕的 enabled，兩者一起更新才不會漏掉一邊
  void updateCustomApplyState();
  void updateCustomPlaceholders();

  SettingsContext ctx_;
  VoiceDeps deps_;
  std::unique_ptr<Ui::VoicePage> ui_;

  // 非同步查回來的快取
  std::vector<TtsEngineInfo> engines_;
  std::vector<VoiceInfo> voices_;
  // 發出語音清單請求時的引擎；回來時不一樣就丟棄（使用者已經換過了）
  std::string voiceRequestEngine_;
  bool requestedTtsInfo_ = false;

  // 目前語系的預設試聽文字。換語系時只有「使用者沒動過」（內容還等於這一份）
  // 才跟著換掉，不然會把使用者打好的句子洗掉。
  QString sampleDefault_;

  // 滑桿拖動中不寫設定，放開或停手 300 ms 才寫
  QTimer* sliderDebounce_ = nullptr;
  // 自訂欄位是否已經從設定灌過值。第一次進這一頁時表單是空的，
  // 而「空表單 != 設定」會被 customDirty() 判成有未套用的編輯而拒絕回灌 ——
  // 這道旗標就是用來區分「還沒載入」與「使用者真的改過」。
  bool customLoaded_ = false;
  // 位址欄位是否已經從設定灌過值。理由同 customLoaded_：第一次進這一頁時
  // 表單是空的，而「空表單 != 設定」會被 serverDirty() 判成有未套用的編輯。
  bool serverLoaded_ = false;
  bool updating_ = false;
};

}  // namespace l2m
