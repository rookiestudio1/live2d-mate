#pragma once

// 設定視窗的「LLM」分頁（內建 LLM 第一期）。
//
// 兩個區塊，套用策略刻意不同：
//  1. 連線設定（provider / baseUrl / apiKey / model）——「套用」按鈕制，
//     照抄語音分頁自訂語音那一套（voice_page.h 第 4 點的三個坑）：
//     自由文字欄位沒有可靠的提交點，即時套用會讓「打到一半的金鑰」被存進去。
//     按鈕只在 llmFieldsEqual（core/config_patch.h）為假時亮起；設定不完整
//     照樣可以套用，只出紅字 —— 紅字與執行失敗共用同一句
//     （core/llm_http.h 的 llmConfigIssue）。
//  2. 行為（enabled / driveIdle / temperature / cooldown）—— 布林與數值
//     有可靠的提交點，即時套用。
//
// Designer 靜態骨架在 llm_page.ui；provider 下拉的項目、model 下拉的清單
//（測試連線的結果）由程式碼填。兩個 provider 共用同一組
// apiKey / model 控制項：切換 provider 時把畫面上的值收進對應的暫存
//（openai*_ / anthropic*_ 成員），再載入另一邊的 —— 分成兩組控制項的話
// 表單會長一倍，而且九成使用者只填一邊。
//
// 表單只在「切到這一頁」與「套用之後」重讀，不掛 ConfigStore::changed ——
// 打到一半的金鑰不該被別處的設定變更沖掉（mcp_page.h 的同一條規則）。

#include <QWidget>

#include <memory>
#include <string>

#include "settings_context.h"

namespace Ui {
class LlmPage;
}

namespace l2m {

class LlmPage : public QWidget {
  Q_OBJECT

public:
  LlmPage(const SettingsContext& context, LlmDeps deps, QWidget* parent = nullptr);
  ~LlmPage() override;

  void retranslate();
  void refresh();
  void refreshIfActive();

private:
  QString tr2(const char* key) const;

  void reloadForm();
  // 把畫面上的值收進目前 provider 的暫存（切換 provider 與比對 dirty 前都要做）
  void commitFormToCache();
  // 用暫存組出一份 LlmConfig（連線欄位來自表單、其餘沿用存檔值）
  LlmConfig formConfig() const;
  // 依表單現況更新紅字與套用按鈕
  void updateWarnings();
  void applyProviderVisibility();
  void apply();
  void testConnection();

  SettingsContext ctx_;
  LlmDeps deps_;
  std::unique_ptr<Ui::LlmPage> ui_;
  bool updating_ = false;

  // 表單的暫存（兩個 provider 各一份，切換不弄丟另一邊還沒套用的值）
  std::string openaiKey_;
  std::string openaiModel_;
  std::string anthropicKey_;
  std::string anthropicModel_;
  // 目前控制項顯示的是哪個 provider 的值（與下拉的選擇同步）
  std::string formProvider_ = "openai";
  // 測試連線的世代序號：非同步回覆要比對送出時的序號，
  // 快速改設定連按兩次時舊清單才不會蓋掉新的（語音分頁的同一個坑）
  int testGeneration_ = 0;
};

}  // namespace l2m
