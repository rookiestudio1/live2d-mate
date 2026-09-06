#include "settings_layout.h"

#include <algorithm>

namespace l2m {

namespace {

struct TabMeta {
  SettingsTab tab;
  const char* id;
  const char* titleKey;
};

const std::vector<TabMeta>& tabMeta() {
  // 由常用到少用：一般（開關）→ 模型（換模型與命名）→ 角色（個性描述）
  // → 語音 → MCP → 關於。角色接在模型後面：兩者都是「這隻桌寵是誰」的設定，
  // 一個管長相、一個管講話的樣子。
  static const std::vector<TabMeta> meta{
    {SettingsTab::General, "general", "settings.tab.general"}, {SettingsTab::Model, "model", "settings.tab.model"}, {SettingsTab::Persona, "persona", "settings.tab.persona"},
    {SettingsTab::Voice, "voice", "settings.tab.voice"},       {SettingsTab::Mcp, "mcp", "settings.tab.mcp"},       {SettingsTab::Llm, "llm", "settings.tab.llm"},
    {SettingsTab::About, "about", "settings.tab.about"},
  };
  return meta;
}

}  // namespace

const std::vector<SettingsTab>& settingsTabs() {
  static const std::vector<SettingsTab> tabs = [] {
    std::vector<SettingsTab> out;
    for (const auto& meta : tabMeta()) out.push_back(meta.tab);
    return out;
  }();
  return tabs;
}

const char* settingsTabKey(SettingsTab tab) {
  for (const auto& meta : tabMeta()) {
    if (meta.tab == tab) return meta.titleKey;
  }
  return tabMeta().front().titleKey;
}

std::string settingsTabId(SettingsTab tab) {
  for (const auto& meta : tabMeta()) {
    if (meta.tab == tab) return meta.id;
  }
  return tabMeta().front().id;
}

std::optional<SettingsTab> settingsTabFromId(const std::string& id) {
  for (const auto& meta : tabMeta()) {
    if (id == meta.id) return meta.tab;
  }
  return std::nullopt;
}

int settingsTabIndex(SettingsTab tab) {
  const auto& tabs = settingsTabs();
  const auto it = std::find(tabs.begin(), tabs.end(), tab);
  if (it == tabs.end()) return 0;
  return static_cast<int>(std::distance(tabs.begin(), it));
}

std::optional<SettingsTab> settingsTabAt(int index) {
  const auto& tabs = settingsTabs();
  if (index < 0 || index >= static_cast<int>(tabs.size())) return std::nullopt;
  return tabs[static_cast<size_t>(index)];
}

const std::vector<ToggleSpec>& interactionToggles() {
  // 順序即為畫面上由上到下的順序
  static const std::vector<ToggleSpec> toggles{
    {"settings.general.lookAt", "interaction", "lookAt"},
    // 說話回正是視線追蹤的附屬行為，排在 lookAt 正下方
    {"settings.general.speakFacingFront", "interaction", "speakFacingFront"},
    {"settings.general.tapMotion", "interaction", "tapMotion"},
    // dragMove 原本是唯一沒有 UI 的互動開關，順手補上（排在 dragSwing 前面：
    // 拖曳搖晃是拖曳移動的附屬效果）
    {"settings.general.dragMove", "interaction", "dragMove"},
    {"settings.general.dragSwing", "interaction", "dragSwing"},
    {"settings.general.wheelZoom", "interaction", "wheelZoom"},
    {"settings.general.clickThrough", "interaction", "clickThrough"},
    {"settings.general.showBubble", "tts", "showBubble"},
    {"settings.general.autoReset", "idle", "autoReset"},
    {"settings.general.idlePerform", "idle", "perform"},
    {"settings.general.autonomy", "autonomy", "enabled"},
    {"settings.general.autonomyExpression", "autonomy", "expression"},
    {"settings.general.autonomyMove", "autonomy", "move"},
    {"settings.general.autonomyGreet", "autonomy", "greet"},
    {"settings.general.autonomyPauseAway", "autonomy", "pauseWhenAway"},
    {"settings.general.breakReminder", "autonomy", "breakReminder"},
  };
  return toggles;
}

const std::vector<ToggleSpec>& mcpToggles() {
  // 順序即為畫面上由上到下的順序
  static const std::vector<ToggleSpec> toggles{
    {"settings.mcp.talkative", "mcp", "talkative"},
    {"settings.mcp.notifyOnComplete", "mcp", "notifyOnComplete"},
    {"settings.mcp.speakNoWait", "mcp", "speakNoWait"},
    {"settings.mcp.announceSteps", "mcp", "announceSteps"},
  };
  return toggles;
}

bool toggleValue(const AppConfig& config, const ToggleSpec& spec) {
  const std::string field = spec.field;
  const std::string section = spec.section;
  if (section == "interaction") {
    if (field == "lookAt") return config.interaction.lookAt;
    if (field == "clickThrough") return config.interaction.clickThrough;
    if (field == "dragMove") return config.interaction.dragMove;
    if (field == "wheelZoom") return config.interaction.wheelZoom;
    if (field == "lockPosition") return config.interaction.lockPosition;
    if (field == "tapMotion") return config.interaction.tapMotion;
    if (field == "dragSwing") return config.interaction.dragSwing;
    if (field == "speakFacingFront") return config.interaction.speakFacingFront;
  } else if (section == "tts") {
    if (field == "showBubble") return config.tts.showBubble;
  } else if (section == "idle") {
    if (field == "autoReset") return config.idle.autoReset;
    if (field == "perform") return config.idle.perform;
  } else if (section == "autonomy") {
    if (field == "enabled") return config.autonomy.enabled;
    if (field == "expression") return config.autonomy.expression;
    if (field == "move") return config.autonomy.move;
    if (field == "greet") return config.autonomy.greet;
    if (field == "pauseWhenAway") return config.autonomy.pauseWhenAway;
    if (field == "breakReminder") return config.autonomy.breakReminder;
  } else if (section == "mcp") {
    if (field == "talkative") return config.mcp.talkative;
    if (field == "notifyOnComplete") return config.mcp.notifyOnComplete;
    if (field == "speakNoWait") return config.mcp.speakNoWait;
    if (field == "announceSteps") return config.mcp.announceSteps;
  } else if (section == "app") {
    if (field == "openAtLogin") return config.app.openAtLogin;
    if (field == "alwaysOnTop") return config.app.alwaysOnTop;
    if (field == "disableHardwareAcceleration") return config.app.disableHardwareAcceleration;
  }
  // 走到這裡代表開關表寫了一個不存在的欄位；測試會抓到（讀寫對不起來）
  return false;
}

}  // namespace l2m
