// 角色卡 AI 擴寫（分階段對話式生成）：階段順序、prompt 的語言與身分注入、
// 對話歷史的累積、回覆整理（列表符號、超長行）與長度驗證。
#include <QtTest>

#include <string>

#include "core/persona.h"
#include "core/persona_generate.h"

using namespace l2m;

class TestPersonaGenerate : public QObject {
  Q_OBJECT

private slots:
  // 七個階段依序走完：描述 → 台詞 → 歡迎詞 → 時段問候 → 久坐提醒 → 摸摸反應 → 天氣預警
  void stagesRunInOrder() {
    PersonaGenerateSession session(PersonaGenerateInput{});
    QCOMPARE(session.stageCount(), 7);
    QCOMPARE(session.stageIndex(), 0);
    QVERIFY(!session.done());
    QVERIFY(session.currentStage() == PersonaGenStage::Description);

    QCOMPARE(session.accept("A helper."), std::string());
    QVERIFY(session.currentStage() == PersonaGenStage::Lines);
    QCOMPARE(session.accept("line a\nline b"), std::string());
    QVERIFY(session.currentStage() == PersonaGenStage::Welcome);
    QCOMPARE(session.accept("hello"), std::string());
    QVERIFY(session.currentStage() == PersonaGenStage::Greetings);
    QCOMPARE(session.accept("morning: hi"), std::string());
    QVERIFY(session.currentStage() == PersonaGenStage::Breaks);
    QCOMPARE(session.accept("take a break"), std::string());
    QVERIFY(session.currentStage() == PersonaGenStage::Petted);
    QCOMPARE(session.accept("hehe"), std::string());
    QVERIFY(session.currentStage() == PersonaGenStage::Weather);
    QCOMPARE(session.accept("rain: bring an umbrella"), std::string());
    QVERIFY(session.done());

    const PersonaDoc doc = session.result();
    QCOMPARE(doc.description, std::string("A helper."));
    QCOMPARE(doc.lines, (std::vector<std::string>{"line a", "line b"}));
    QCOMPARE(doc.welcome, (std::vector<std::string>{"hello"}));
    QCOMPARE(doc.greetings, (std::vector<std::string>{"morning: hi"}));
    QCOMPARE(doc.breaks, (std::vector<std::string>{"take a break"}));
    QCOMPARE(doc.petted, (std::vector<std::string>{"hehe"}));
    QCOMPARE(doc.weatherAlerts, (std::vector<std::string>{"rain: bring an umbrella"}));
  }

  // system 帶現有卡片（身分不走鐘）與使用者的方向；每一階段的指示要求
  // 「只輸出內容本身」。**不下語言指示**：語言由使用者寫在 brief 裡
  //（persona_generate.cpp 的 nextMessages()），這條反向斷言就是把那個決定釘住
  void promptCarriesIdentityAndBrief() {
    PersonaGenerateInput input;
    input.current.description = "既有的描述。";
    input.brief = "更活潑一點";
    PersonaGenerateSession session(std::move(input));

    const auto messages = session.nextMessages();
    QCOMPARE(messages.size(), size_t(2));  // system + 這一階段的指示
    const std::string& system = messages[0].text;
    QVERIFY(system.find("Write ALL content in") == std::string::npos);
    QVERIFY(system.find("既有的描述。") != std::string::npos);
    QVERIFY(system.find("do not change who") != std::string::npos);
    QVERIFY(system.find("更活潑一點") != std::string::npos);
    QVERIFY(system.find("ONLY the requested content") != std::string::npos);
    QCOMPARE(messages[1].role, std::string("user"));
    QVERIFY(messages[1].text.find("Character Description") != std::string::npos);
  }

  // 「同一場對話」：後面的階段帶著前面的問答，台詞階段看得到自己寫的描述
  void historyAccumulatesAcrossStages() {
    PersonaGenerateSession session(PersonaGenerateInput{});
    session.accept("A cheerful secretary.");

    const auto messages = session.nextMessages();
    // system + (user 描述指示 + assistant 描述) + 這一階段的指示
    QCOMPARE(messages.size(), size_t(4));
    QCOMPARE(messages[1].role, std::string("user"));
    QCOMPARE(messages[2].role, std::string("assistant"));
    QCOMPARE(messages[2].text, std::string("A cheerful secretary."));
    QVERIFY(messages[3].text.find("idle mutterings") != std::string::npos);
  }

  // 回覆整理：code fence、列表符號（-、*、•、編號）剝掉、空行略過、超長行丟掉
  void cleansUpLineResponses() {
    PersonaGenerateSession session(PersonaGenerateInput{});
    session.accept("desc");
    const std::string longLine(kPersonaLineMaxChars + 1, 'x');
    QCOMPARE(session.accept("```\n- line one\n* line two\n\n1. line three\n2) line four\n" + longLine + "\n```"), std::string());
    const PersonaDoc doc = session.result();
    QCOMPARE(doc.lines, (std::vector<std::string>{"line one", "line two", "line three", "line four"}));
  }

  // 描述階段是嚴格的：空回覆與超長都報錯、階段不前進；台詞階段空回覆是合法的
  void descriptionStageValidates() {
    PersonaGenerateSession session(PersonaGenerateInput{});
    QVERIFY(!session.accept("").empty());
    QVERIFY(session.currentStage() == PersonaGenStage::Description);
    const std::string tooLong(kPersonaMaxChars + 1, 'x');
    QVERIFY(!session.accept(tooLong).empty());
    QVERIFY(session.currentStage() == PersonaGenStage::Description);
    QCOMPARE(session.accept("fine"), std::string());
    // 台詞階段：模型偶爾整段空白，當作這一區沒有內容，不擋流程
    QCOMPARE(session.accept(""), std::string());
    QVERIFY(session.currentStage() == PersonaGenStage::Welcome);
  }

  // 現有卡片的 reserved 區（未來保留字）原樣進結果，不參與生成
  void reservedSectionsSurvive() {
    PersonaGenerateInput input;
    input.current.reserved.emplace_back("Future Section", "keep me");
    PersonaGenerateSession session(std::move(input));
    session.accept("desc");
    for (int i = 0; i < 6; ++i) session.accept("line");
    QVERIFY(session.done());
    const PersonaDoc doc = session.result();
    QCOMPARE(doc.reserved.size(), size_t(1));
    QCOMPARE(doc.reserved[0].second, std::string("keep me"));
  }
};

QTEST_GUILESS_MAIN(TestPersonaGenerate)
#include "test_persona_generate.moc"
