// 初次啟動種入的預設角色。
// 「五個語系都讀得到非空內容」同時是 personas.qrc 有沒有真的被連進 l2m_core
// 的安全網 —— 靜態程式庫裡的資源 TU 被連結器丟掉時，這裡會整排紅燈。
#include <QtTest>

#include <set>

#include "core/config_schema.h"
#include "core/persona.h"
#include "core/persona_doc.h"
#include "core/persona_default.h"

using namespace l2m;

class TestPersonaDefault : public QObject {
  Q_OBJECT

private slots:
  // 五個語系都讀得到非空內容且互不相同（不是五份同一個檔）
  void everyLocaleHasDistinctContent() {
    std::set<std::string> seen;
    for (const auto& locale : supportedLocales()) {
      const std::string text = defaultPersonaMarkdown(locale);
      QVERIFY2(!text.empty(), locale.c_str());
      QVERIFY2(seen.insert(text).second, locale.c_str());
    }
  }

  // 每一份都通得過 persona 的寫入驗證（逐區長度上限）；名稱本身也要是合法檔名。
  // 這條壞掉的症狀是 seedDefaultPersona 安靜放棄，使用者只看到「沒有種」。
  void everyLocalePassesPersonaValidation() {
    QCOMPARE(personaNameIssue(kDefaultPersonaName), std::optional<std::string>());
    for (const auto& locale : supportedLocales()) {
      const auto issue = personaDocIssue(parsePersonaDoc(defaultPersonaMarkdown(locale)));
      QVERIFY2(!issue.has_value(), (locale + ": " + issue.value_or("")).c_str());
    }
  }

  // 每一份都能被 parsePersonaDoc 解析出四個區塊都非空，
  // 且 description ≤ kPersonaMaxChars、每行台詞 ≤ kPersonaLineMaxChars
  void everyLocaleParsesIntoAllSections() {
    for (const auto& locale : supportedLocales()) {
      const PersonaDoc doc = parsePersonaDoc(defaultPersonaMarkdown(locale));
      QVERIFY2(!doc.description.empty(), locale.c_str());
      QVERIFY2(!doc.lines.empty(), locale.c_str());
      QVERIFY2(!doc.welcome.empty(), locale.c_str());
      QVERIFY2(!doc.greetings.empty(), locale.c_str());
      QVERIFY(!personaTextIssue(doc.description).has_value());
      QVERIFY(!personaLinesIssue(doc.lines, "Dialogue list").has_value());
      QVERIFY(!personaLinesIssue(doc.welcome, "Welcome text").has_value());
      QVERIFY(!personaLinesIssue(doc.greetings, "Greeting by time").has_value());
    }
  }

  // parse → serialize → parse 冪等（不與原字串逐位元組比 ——
  // 行尾與檔尾換行會因 checkout 而異，靠 serializePersonaDoc 正規化就好）
  void roundTripIsIdempotent() {
    for (const auto& locale : supportedLocales()) {
      const std::string once = serializePersonaDoc(parsePersonaDoc(defaultPersonaMarkdown(locale)));
      const std::string twice = serializePersonaDoc(parsePersonaDoc(once));
      QCOMPARE(twice, once);
    }
  }

  // 對不到的 locale 退回 en；未知與空字串都不會回空內容
  void unknownLocaleFallsBackToEnglish() {
    const std::string english = defaultPersonaMarkdown("en");
    QCOMPARE(defaultPersonaMarkdown("fr-FR"), english);
    QCOMPARE(defaultPersonaMarkdown(""), english);
    // 帶地區碼的變體要對應到同語系（en-US → en、zh-Hant-TW → zh-TW）
    QCOMPARE(defaultPersonaMarkdown("en-US"), english);
    QCOMPARE(defaultPersonaMarkdown("zh-Hant-TW"), defaultPersonaMarkdown("zh-TW"));
  }
};

QTEST_GUILESS_MAIN(TestPersonaDefault)
#include "test_persona_default.moc"
