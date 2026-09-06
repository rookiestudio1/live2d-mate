// 設定視窗的分頁清單與一般分頁的開關表。
//
// 這裡擋住兩件 test_i18n 抓不到的事：
//  1. 加了分頁／開關卻忘了補翻譯（五份 JSON 一起漏掉時 key 集合仍然一致）。
//  2. 開關表的 section/field 打錯（打錯只會變成靜默失效，畫面上看不出來）。
#include <QTemporaryDir>
#include <QtTest>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "core/config_patch.h"
#include "core/config_schema.h"
#include "core/config_store.h"
#include "core/i18n.h"
#include "core/settings_layout.h"

using namespace l2m;
namespace fs = std::filesystem;

namespace {

// 每一張開關表都要走過同一組檢查。集中在這裡而不是各測試各列一次 ——
// 新增一張表（例如 MCP 分頁那兩個）時只要改這裡，不會有某條檢查被漏掉。
std::vector<ToggleSpec> allToggles() {
  std::vector<ToggleSpec> all = interactionToggles();
  const auto& mcp = mcpToggles();
  all.insert(all.end(), mcp.begin(), mcp.end());
  return all;
}

}  // namespace

class TestSettingsLayout : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() { i18n::setMessagesDir(fs::u8path(L2M_I18N_DIR)); }

  // === 分頁 ===

  // 順序是對外承諾：系統匣開「一般」、MCP 工具開「模型」
  void tabOrderIsStable() {
    const std::vector<SettingsTab> expected{SettingsTab::General, SettingsTab::Model, SettingsTab::Persona, SettingsTab::Voice, SettingsTab::Mcp, SettingsTab::Llm, SettingsTab::About};
    QCOMPARE(settingsTabs(), expected);
  }

  void indexRoundTrips() {
    for (const auto tab : settingsTabs()) {
      QCOMPARE(settingsTabAt(settingsTabIndex(tab)), std::optional<SettingsTab>(tab));
    }
    QCOMPARE(settingsTabAt(-1), std::optional<SettingsTab>());
    QCOMPARE(settingsTabAt(static_cast<int>(settingsTabs().size())), std::optional<SettingsTab>());
  }

  void idRoundTrips() {
    for (const auto tab : settingsTabs()) {
      QCOMPARE(settingsTabFromId(settingsTabId(tab)), std::optional<SettingsTab>(tab));
    }
    QCOMPARE(settingsTabFromId("nope"), std::optional<SettingsTab>());
  }

  void idsAndKeysAreDistinct() {
    std::vector<std::string> ids;
    std::vector<std::string> keys;
    for (const auto tab : settingsTabs()) {
      ids.push_back(settingsTabId(tab));
      keys.push_back(settingsTabKey(tab));
    }
    std::sort(ids.begin(), ids.end());
    std::sort(keys.begin(), keys.end());
    QCOMPARE(std::unique(ids.begin(), ids.end()), ids.end());
    QCOMPARE(std::unique(keys.begin(), keys.end()), keys.end());
  }

  // === 翻譯 ===

  // 五個語系都要有分頁標題與開關標籤；translate 查不到會回傳 key 本身
  void everyLocaleHasAllLabels() {
    std::vector<std::string> keys;
    for (const auto tab : settingsTabs()) keys.push_back(settingsTabKey(tab));
    for (const auto& toggle : allToggles()) keys.push_back(toggle.labelKey);

    for (const auto& locale : supportedLocales()) {
      for (const auto& key : keys) {
        const std::string text = i18n::translate(locale, key);
        QVERIFY2(text != key, (locale + " / " + key).c_str());
        QVERIFY2(!text.empty(), (locale + " / " + key).c_str());
      }
    }
  }

  // === 開關表 ===

  void toggleLabelsAreDistinct() {
    std::vector<std::string> keys;
    for (const auto& toggle : allToggles()) keys.push_back(toggle.labelKey);
    std::sort(keys.begin(), keys.end());
    QCOMPARE(std::unique(keys.begin(), keys.end()), keys.end());
  }

  // section/field 打錯的話 patch 會被 ConfigStore 拒絕；這裡直接餵一次
  void everyToggleWritesARealConfigField() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    for (const auto& toggle : allToggles()) {
      const std::string json = boolPatch(toggle.section, toggle.field, false);
      try {
        store.patch(json);
      } catch (const std::exception& err) {
        QFAIL(qPrintable(QStringLiteral("%1: %2").arg(QString::fromUtf8(json.c_str()), QString::fromUtf8(err.what()))));
      }
    }
    // 真的寫進去了（隨便挑一個驗，避免只是「沒丟例外」）
    QCOMPARE(store.get().interaction.lookAt, false);
    QCOMPARE(store.get().tts.showBubble, false);
    QCOMPARE(store.get().idle.perform, false);
    QCOMPARE(store.get().mcp.talkative, false);
    QCOMPARE(store.get().mcp.notifyOnComplete, false);
  }

  // 讀寫必須成對：patch 寫下去的值，toggleValue 要讀得回來。
  // 只改了其中一半的話畫面看起來完全正常，但勾選狀態永遠是舊的。
  void toggleValueMatchesWhatPatchWrote() {
    QTemporaryDir tempDir;
    const fs::path file = fs::u8path(tempDir.path().toStdString()) / "config.json";
    ConfigStore store(file);
    for (const auto& toggle : allToggles()) {
      for (const bool value : {false, true}) {
        store.patch(boolPatch(toggle.section, toggle.field, value));
        QVERIFY2(toggleValue(store.get(), toggle) == value, toggle.field);
      }
    }
  }

  // MCP 分頁的四個開關預設都是開的。多話是使用者指定的預設，完成通知與
  // 不等播放維持 instructions 本來就有的行為，工作途中報告則同樣照
  // 「角色是主要輸出通道，安靜才是要特地去選的那一邊」——
  // 四個都改成關，桌寵就會退回「只在回覆開頭講一次、之後一路沉默」。
  void mcpTogglesDefaultToOn() {
    const AppConfig config;
    for (const auto& toggle : mcpToggles()) {
      QVERIFY2(toggleValue(config, toggle), toggle.field);
    }
  }
};

QTEST_GUILESS_MAIN(TestSettingsLayout)
#include "test_settings_layout.moc"
