#pragma once

// 設定視窗的「一般」分頁。
// 內容來自原本系統匣的「一般」子選單（windows/tray.cpp buildGeneralMenu／
// buildLanguageMenu）與「外觀」子選單下半段的 7 個開關。
//
// 對應的表單是 general_page.ui。整頁內容包在一個 QScrollArea 裡
//（widgetResizable、水平捲軸永遠關閉）—— 群組已經多到超出視窗高度，
// 沒有捲軸的話下半的群組會被壓扁擠在一起。靜態控制項：視窗群組的兩個核取方塊、
// 閒置群組的三個秒數 QSpinBox、環境風群組（開關／方向下拉／強度滑桿）、
// 氣泡群組的陰影下拉與間距微調（各是標籤 + 控制項）、系統群組的開機啟動／停用硬體加速／
// 語言下拉／兩個開資料夾按鈕。
// **互動群組的核取方塊是程式碼依 core/settings_layout.h 的
// interactionToggles() 動態產生的**（QGridLayout 兩欄，先左後右逐列往下）——
// 那張表同時是測試的斷言對象，
// 讓「哪個核取方塊寫哪個 config 欄位」只有一個來源。
//
// 數值控制項有幾件事刻意寫在程式碼而不是 .ui：
// 1. 上下限來自 core/config_schema.h 的具名常數（kMinBubbleOffsetY、
//    kMinIdleResetMs 等），與 schema 的驗證共用一份，兩邊界限才不會各走各的
//    （.ui 只留 singleStep）。閒置的三個以**秒**顯示，寫入時乘回 1000。
// 2. setKeyboardTracking(false) —— 預設會逐字元發 valueChanged，打「-100」
//    途中就會先套用 -1、-10，每一步都寫設定又跳一次預覽氣泡。關掉之後只有
//    上下箭頭、Enter 與失焦才發訊號，預覽仍然即時。
// 3. 環境風的強度滑桿走 300 ms 防抖（比照語音分頁的 rate / volume）——
//    拖動中每個位置都寫一次 config 只是白白觸發一串 changed。
//    強度是滑桿值除以 100 的浮點，updating_ 護欄不能省：浮點往返誤差會讓
//    ConfigStore「序列化結果真的有變才 emit」擋不住，refresh → setValue →
//    valueChanged → patch → changed → refresh 繞不完。
//
// 全部即時套用（比照系統匣原本的行為），成功與否顯示在視窗底部的狀態列。
//
// **一個例外：天氣群組的「地點」欄位。** 城市名要打一趟 geocoding 才知道座標，
// 每個鍵擊查一次是災難（與語音分頁的伺服器位址同一條理由），所以它配一顆
// 「套用」按鈕，按下去才查、查到才把座標與顯示名**整包**寫回 config
//（core/config_patch.h 的 weatherLocationPatch —— 分三次送會讓 ConfigStore
// 在中途以「新座標配舊城市名」重跑一輪）。同一群組的其他控制項照舊即時套用。

#include <QWidget>

#include <memory>
#include <string>
#include <vector>

#include "settings_context.h"

class QCheckBox;
class QHideEvent;
class QTimer;

namespace Ui {
class GeneralPage;
}

namespace l2m {

class GeneralPage : public QWidget {
  Q_OBJECT

public:
  GeneralPage(const SettingsContext& context, BubbleDeps deps, WeatherDeps weatherDeps, QWidget* parent = nullptr);
  ~GeneralPage() override;

  // 重設所有靜態文字
  void retranslate();
  // 從設定與執行期狀態重讀所有控制項
  void refresh();
  // 訊號用的入口：分頁沒被選到就不必跟著重建
  void refreshIfActive();

protected:
  // 設定視窗一關（或切走分頁）就把預覽氣泡收掉，不要讓它賴在畫面上等逾時
  void hideEvent(QHideEvent* event) override;

private:
  QString tr2(const char* key) const;
  void buildInteractionToggles();
  void applyLanguage();
  void applyBubbleOffset(int value);
  // 借氣泡顯示一句預覽（間距微調與陰影共用）—— 這兩個設定看不到就調不了
  void showBubblePreview();
  // 地點欄位的「套用」：城市名 → geocoding → 三個欄位整包寫回 config
  void applyWeatherLocation();
  // 狀態列（目前天氣或最近一次的錯誤）
  void refreshWeatherStatus();

  SettingsContext ctx_;
  BubbleDeps deps_;
  WeatherDeps weatherDeps_;
  // 地點查詢還在飛：按鈕變灰，避免連按送出好幾發 geocoding
  bool weatherResolving_ = false;
  std::unique_ptr<Ui::GeneralPage> ui_;
  // 預覽氣泡的收尾計時器（放手之後撐一下再收）
  QTimer* previewHold_ = nullptr;
  // 環境風強度滑桿的防抖計時器（300 ms，比照語音分頁的 rate / volume）
  QTimer* windStrengthDebounce_ = nullptr;
  // 與 interactionToggles() 同順序，refresh() 直接照索引配對
  std::vector<QCheckBox*> interactionBoxes_;
  // 程式化更新控制項期間擋掉自己發出的訊號（見 settings_context.h 的 ScopedUpdate）
  bool updating_ = false;
};

}  // namespace l2m
