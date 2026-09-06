#pragma once

// 設定視窗的分頁清單與「一般」分頁的開關表。
//
// 為什麼放在 l2m_core 而不是視窗自己：
//  * 分頁 id 是對外介面的一部分 —— 系統匣的「設定…」開在「一般」、
//    MCP 工具 open_naming_editor 開在「模型」，兩邊得指名同一組 id。
//  * 開關表決定了「哪個核取方塊寫哪個 config 欄位、用哪個 i18n key」。
//    這種對照表用手改最容易悄悄壞掉（欄位打錯只會變成靜默失效），
//    只連 l2m_core 的測試拿得到它才驗得起來。
// 與 core/tray_label.h（vs windows/tray.cpp）是同一個切法。
//
// 對應的視窗實作在 src/windows/settings/。

#include <optional>
#include <string>
#include <vector>

#include "config_schema.h"

namespace l2m {

enum class SettingsTab { General, Model, Persona, Voice, Mcp, Llm, About };

// 分頁在 QTabWidget 裡的順序（由左到右）
const std::vector<SettingsTab>& settingsTabs();

// 分頁標題的 i18n key（"settings.tab.general" …）
const char* settingsTabKey(SettingsTab tab);

// 永遠不翻譯的穩定字串 id。給 MCP、日誌與未來的「記住上次分頁」用。
std::string settingsTabId(SettingsTab tab);
std::optional<SettingsTab> settingsTabFromId(const std::string& id);

// 與 QTabWidget 索引互轉；索引超出範圍回 nullopt
int settingsTabIndex(SettingsTab tab);
std::optional<SettingsTab> settingsTabAt(int index);

// 「一般」分頁上直接寫 config 的核取方塊。
// section/field 直接餵給 ConfigStore::patch（只吃兩層），labelKey 餵給 i18n。
struct ToggleSpec {
  const char* labelKey;
  const char* section;
  const char* field;
};

// 互動類開關（原本在系統匣「外觀」子選單的下半段）
const std::vector<ToggleSpec>& interactionToggles();

// 「MCP」分頁上的發話節奏開關。
// 跟同頁的 host／port／token 不一樣，這兩個**即時套用**、不走「套用並重新啟動」——
// 它們沒有跨欄位驗證，也不影響伺服器怎麼跑（只改 initialize 送出的 instructions），
// 跟著 apply 走只會白白重啟一次 httplib 伺服器。理由詳見 windows/settings/mcp_page.h。
const std::vector<ToggleSpec>& mcpToggles();

// 讀回某個開關目前的值。
// 寫入走 boolPatch(section, field)，讀取如果讓每個 UI 各寫一次 if-else 鏈，
// 兩邊很快就會對不上（改了 patch 的欄位卻忘了改讀的那一半，畫面看起來
// 完全正常但勾選狀態永遠是舊的）。讀寫成對放在這裡才驗得到。
bool toggleValue(const AppConfig& config, const ToggleSpec& spec);

}  // namespace l2m
