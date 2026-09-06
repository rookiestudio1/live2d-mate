// 角色 .md 的 # 分區解析與序列化。
// 最重要的一條是第一個測試：沒有任何保留字標題的舊檔，description 必須
// 等於整份原文 —— 這條壞掉就是所有現有角色卡一起壞。
#include <QtTest>

#include "core/persona.h"
#include "core/persona_doc.h"

using namespace l2m;

class TestPersonaDoc : public QObject {
  Q_OBJECT

private slots:
  // 沒有任何 # 的舊檔 → description 等於整份原文且另兩區為空
  void plainTextIsAllDescription() {
    const std::string raw = "你是一個傲嬌的角色。\n- 特徵一\n- 特徵二\n";
    const PersonaDoc doc = parsePersonaDoc(raw);
    QCOMPARE(doc.description, raw);
    QVERIFY(doc.lines.empty());
    QVERIFY(doc.welcome.empty());
    QVERIFY(doc.reserved.empty());
  }

  // 有 # 標題但不在保留字清單裡 → 仍然整份當 description（一個位元組都不變）
  void unknownHeadingsAloneStayVerbatim() {
    const std::string raw = "# 背景設定\n她來自北方。\n\n# 口癖\n雜魚♪\n";
    const PersonaDoc doc = parsePersonaDoc(raw);
    QCOMPARE(doc.description, raw);
    QVERIFY(doc.lines.empty());
  }

  // 三區齊全的標準格式
  void threeSectionsParse() {
    const std::string raw =
      "# Character Description\n描述本文。\n\n"
      "# Dialogue List\n台詞一\n台詞二\n\n"
      "# Welcome Text\n歡迎詞一\n";
    const PersonaDoc doc = parsePersonaDoc(raw);
    QCOMPARE(doc.description, std::string("描述本文。"));
    QCOMPARE(doc.lines, (std::vector<std::string>{"台詞一", "台詞二"}));
    QCOMPARE(doc.welcome, (std::vector<std::string>{"歡迎詞一"}));
  }

  // 三區任意順序都解析得出來；標題比對忽略大小寫與前後空白
  void sectionsParseInAnyOrder() {
    const std::string raw =
      "# welcome text\n嗨。\n\n"
      "#  DIALOGUE LIST \n哈囉。\n\n"
      "# Character description\n描述。\n";
    const PersonaDoc doc = parsePersonaDoc(raw);
    QCOMPARE(doc.description, std::string("描述。"));
    QCOMPARE(doc.lines, (std::vector<std::string>{"哈囉。"}));
    QCOMPARE(doc.welcome, (std::vector<std::string>{"嗨。"}));
  }

  // 啟用分區之後，description 裡的非保留字標題不會被抽走
  void unknownHeadingStaysInsideItsSection() {
    const std::string raw =
      "開場白。\n\n# 背景設定\n她來自北方。\n\n"
      "# Dialogue List\n台詞一\n";
    const PersonaDoc doc = parsePersonaDoc(raw);
    QCOMPARE(doc.description, std::string("開場白。\n\n# 背景設定\n她來自北方。"));
    QCOMPARE(doc.lines, (std::vector<std::string>{"台詞一"}));
  }

  // parse → serialize → parse 冪等（比 serialize 輸出，不比原字串 ——
  // 行尾與空行由 serializePersonaDoc 正規化）
  void roundTripIsIdempotent() {
    const std::string raw =
      "描述開頭。\r\n# Dialogue List\r\n  台詞一  \r\n\r\n台詞二\r\n"
      "# Welcome Text\r\n嗨\r\n"
      "# Greeting by Time\r\nmorning: 早安\r\n";
    const std::string once = serializePersonaDoc(parsePersonaDoc(raw));
    const std::string twice = serializePersonaDoc(parsePersonaDoc(once));
    QCOMPARE(twice, once);
  }

  // # Greeting by Time 解析成第四個台詞區，來回一趟不掉行
  void greetingsSectionRoundTrips() {
    const std::string raw =
      "# Character Description\n描述。\n"
      "# Greeting by Time\nmorning: 早安\nnight: 晚安\n";
    const PersonaDoc doc = parsePersonaDoc(raw);
    QCOMPARE(doc.greetings, (std::vector<std::string>{"morning: 早安", "night: 晚安"}));
    QVERIFY(doc.reserved.empty());

    const PersonaDoc again = parsePersonaDoc(serializePersonaDoc(doc));
    QCOMPARE(again.greetings, doc.greetings);
  }

  // 久坐提醒與摸摸反應兩個新區（LLM 第一期加入）：解析、序列化往返、
  // 序列化順序在 greetings 之後、reserved 之前
  void breakAndPettedSectionsRoundTrip() {
    const std::string raw =
      "# Character Description\n描述。\n"
      "# Break Reminder\n休息一下吧。\n"
      "# Petted\n嘿嘿。\n";
    const PersonaDoc doc = parsePersonaDoc(raw);
    QCOMPARE(doc.breaks, (std::vector<std::string>{"休息一下吧。"}));
    QCOMPARE(doc.petted, (std::vector<std::string>{"嘿嘿。"}));
    QVERIFY(doc.reserved.empty());

    const std::string out = serializePersonaDoc(doc);
    QVERIFY(out.find("# Break Reminder") != std::string::npos);
    QVERIFY(out.find("# Petted") != std::string::npos);
    const PersonaDoc again = parsePersonaDoc(out);
    QCOMPARE(again.breaks, doc.breaks);
    QCOMPARE(again.petted, doc.petted);
  }

  // CRLF 與檔尾沒有換行都不炸；台詞區的空行與行首行尾空白被 trim 掉
  void crlfAndMissingTrailingNewlineAreFine() {
    const std::string raw =
      "# Character Description\r\n描述。\r\n\r\n# Dialogue List\r\n"
      "  台詞一\t\r\n\r\n台詞二";
    const PersonaDoc doc = parsePersonaDoc(raw);
    QCOMPARE(doc.description, std::string("描述。"));
    QCOMPARE(doc.lines, (std::vector<std::string>{"台詞一", "台詞二"}));
  }

  // 自由格式（沒有台詞）序列化時不硬加標題
  void serializePlainDescriptionAddsNoHeadings() {
    PersonaDoc doc;
    doc.description = "只有描述。\n";
    QCOMPARE(serializePersonaDoc(doc), std::string("只有描述。\n"));
  }

  // === 驗證 ===

  void lineLimitsAreEnforced() {
    std::vector<std::string> ok(3, std::string("hello"));
    QVERIFY(!personaLinesIssue(ok, "Dialogue list"));

    // 單行超過 kPersonaLineMaxChars
    std::vector<std::string> longLine{std::string(kPersonaLineMaxChars + 1, 'a')};
    QVERIFY(personaLinesIssue(longLine, "Dialogue list").has_value());

    // 總長超過 kPersonaLinesMaxChars（每行都在單行上限內）
    std::vector<std::string> many(kPersonaLinesMaxChars / 100 + 1, std::string(100, 'a'));
    QVERIFY(personaLinesIssue(many, "Dialogue list").has_value());
  }

  // 整份驗證：description 與兩個台詞區各自算，互不佔額度
  void docIssueChecksEachSectionSeparately() {
    PersonaDoc doc;
    doc.description = std::string(1000, 'a');
    doc.lines = std::vector<std::string>(30, std::string(100, 'b'));  // 3000 字，各區合法
    QVERIFY(!personaDocIssue(doc));

    doc.description = std::string(kPersonaMaxChars + 1, 'a');
    QVERIFY(personaDocIssue(doc).has_value());
  }
};

QTEST_GUILESS_MAIN(TestPersonaDoc)
#include "test_persona_doc.moc"
