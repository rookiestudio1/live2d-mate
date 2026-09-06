// i18n：五份語系 JSON 的鍵一致性、佔位字替換、複數規則、語系比對與綁定
#include <QtTest>

#include <filesystem>

#include "core/config_schema.h"
#include "core/i18n.h"

using namespace l2m;
namespace fs = std::filesystem;

class TestI18n : public QObject {
  Q_OBJECT

private slots:
  void initTestCase() {
    // 訊息表就在 repo 的 i18n/（測試由 L2M_I18N_DIR 指入）
    i18n::setMessagesDir(fs::u8path(L2M_I18N_DIR));
  }

  // === 訊息表完整性 ===

  // 各語系的 key 與英文表完全一致
  void allLocalesHaveSameKeys() {
    const auto expected = i18n::messageKeys("en");
    QVERIFY(!expected.empty());
    for (const auto& locale : supportedLocales()) {
      QCOMPARE(i18n::messageKeys(locale), expected);
    }
  }

  // 沒有空字串的翻譯
  void noBlankTranslations() {
    for (const auto& locale : supportedLocales()) {
      QVERIFY2(i18n::blankMessages(locale).empty(), locale.c_str());
    }
  }

  // 每個語系都有自稱名
  void everyLocaleHasAutonym() {
    for (const auto& locale : supportedLocales()) {
      QVERIFY(!i18n::localeAutonyms().at(locale).empty());
    }
  }

  // === 插值 ===

  // 取代 {name} 佔位符
  void replacesPlaceholder() { QCOMPARE(i18n::translate("en", "settings.about.version", i18n::TParams().arg("version", "1.2.3")), std::string("Version 1.2.3")); }

  // 組合多個佔位符
  void combinesPlaceholders() { QCOMPARE(i18n::translate("en", "tray.namedLabel", i18n::TParams().arg("meaning", "揮手").arg("raw", "Idle").arg("suffix", "!")), std::string("揮手 (Idle!)")); }

  // 缺參數時佔位符原樣保留，翻譯出錯才看得出來
  void missingParamKeptVerbatim() { QCOMPARE(i18n::translate("en", "settings.about.version", i18n::TParams().arg("x", "y")), std::string("Version {version}")); }

  // 完全不給參數時不做取代
  void noParamsNoReplacement() { QCOMPARE(i18n::translate("en", "mcp.status.running"), std::string("Running · {url}")); }

  // 自定義語音的範例字串裡有 ${TEXT}，而 {TEXT} 剛好符合佔位符的形狀。
  // 被 interpolate 吃掉的話畫面上的範例就會少一段，使用者照抄就是錯的。
  void textTokenSurvivesInterpolation() {
    for (const auto& locale : supportedLocales()) {
      for (const char* key : {"settings.voice.custom.paramsFormPlaceholder", "settings.voice.custom.paramsJsonPlaceholder", "settings.voice.custom.insertToken", "settings.voice.custom.hint"}) {
        const std::string text = i18n::translate(locale, key);
        QVERIFY2(text.find("${TEXT}") != std::string::npos, (locale + " / " + key).c_str());
      }
    }
  }

  // 代入的值不會被再掃一次，使用者寫的意義裡有大括號也不會被吃掉
  void insertedValuesNotRescanned() { QCOMPARE(i18n::translate("en", "tray.plainLabel", i18n::TParams().arg("raw", "{suffix}").arg("suffix", "!")), std::string("{suffix}!")); }

  // === 單複數 ===

  // count 為 1 時取 one；不是 1 時取 other
  void pluralSelection() {
    QCOMPARE(i18n::translate("en", "naming.motionsHeading", i18n::TParams().count(1)), std::string("Motions (1 group)"));
    QCOMPARE(i18n::translate("en", "naming.motionsHeading", i18n::TParams().count(9)), std::string("Motions (9 groups)"));
    QCOMPARE(i18n::translate("en", "naming.motionsHeading", i18n::TParams().count(0)), std::string("Motions (0 groups)"));
  }

  // 中日韓不分單複數，直接用同一句
  void cjkNoPluralDistinction() {
    QCOMPARE(i18n::translate("zh-TW", "naming.motionsHeading", i18n::TParams().count(1)), std::string("動作（1 個群組）"));
    QCOMPARE(i18n::translate("zh-TW", "naming.motionsHeading", i18n::TParams().count(9)), std::string("動作（9 個群組）"));
  }

  // === matchLocale ===

  void matchLocaleMapping() {
    QCOMPARE(i18n::matchLocale("zh-Hant-TW"), std::string("zh-TW"));
    QCOMPARE(i18n::matchLocale("zh-TW"), std::string("zh-TW"));
    QCOMPARE(i18n::matchLocale("zh-HK"), std::string("zh-TW"));
    QCOMPARE(i18n::matchLocale("zh-CN"), std::string("zh-CN"));
    QCOMPARE(i18n::matchLocale("zh-Hans"), std::string("zh-CN"));
    QCOMPARE(i18n::matchLocale("zh"), std::string("zh-CN"));
    QCOMPARE(i18n::matchLocale("ja-JP"), std::string("ja"));
    QCOMPARE(i18n::matchLocale("ko-KR"), std::string("ko"));
    QCOMPARE(i18n::matchLocale("en-GB"), std::string("en"));
    QCOMPARE(i18n::matchLocale("de-DE"), std::string("en"));
  }

  // 空值退回英文
  void emptyFallsBackToEnglish() { QCOMPARE(i18n::matchLocale(""), std::string("en")); }

  // === 語系綁定 ===

  // 綁定語系後就不必每次帶（直接驗 ja 的翻譯結果）
  void boundTranslator() { QCOMPARE(i18n::translate("ja", "tray.quit"), std::string("終了")); }
};

QTEST_APPLESS_MAIN(TestI18n)
#include "test_i18n.moc"
