// LLM 行為大腦：prompt 的 context 注入與回應→PerformStep 的解析／過濾。
#include <QtTest>

#include <string>

#include "core/llm_behavior_prompt.h"

using namespace l2m;

namespace {

LlmBehaviorPromptInput sampleInput() {
  LlmBehaviorPromptInput input;
  MotionGroupInfo tap;
  tap.name = "TapBody";
  tap.count = 2;
  input.world.motions.push_back(tap);
  MotionGroupInfo idle;
  idle.name = "Idle";
  idle.count = 1;
  input.world.motions.push_back(idle);
  input.world.expressions = {"f01", "f02"};
  input.world.annotations.motions["TapBody"] = "waving happily";
  input.world.annotations.expressions["f01"] = "smiling";
  input.world.lines = {"Line one.", "Line two."};
  input.world.canSpeak = true;
  input.world.canMove = true;
  input.world.windowXRatio = 0.8;
  input.world.windowYRatio = 1.0;
  input.options.occasion = "idle";
  input.options.level = IdleLevel::Bored;
  input.options.cheerful = true;
  input.options.familiarityLevel = 2;
  input.personaDescription = "A gentle secretary persona.";
  input.hour = 9;
  return input;
}

}  // namespace

class TestLlmBehaviorPrompt : public QObject {
  Q_OBJECT

private slots:
  // 天氣**每一輪都要進 context**，歡迎詞那一輪尤其 —— 使用者要的就是
  // 「開機時角色能依天氣打招呼」。少了這一行，LLM 只能寫出千篇一律的問候
  void weatherRidesAlongEveryOccasion() {
    for (const char* occasion : {"idle", "welcome", "breakReminder"}) {
      LlmBehaviorPromptInput input = sampleInput();
      input.options.occasion = occasion;
      input.weather = "Taichung, now 28C (feels 32C), overcast; next 6h: no rain expected";
      const auto messages = buildBehaviorPromptMessages(input);
      const std::string user = messages[1].text;
      QVERIFY2(user.find("weather: Taichung, now 28C") != std::string::npos, occasion);
    }
  }

  // 歡迎詞的指示要**主動提到**可以帶天氣（只送資料而不說能用，模型多半不會用），
  // 但也要講明「照本宣科報一次預報不算打招呼」
  void welcomeNoteInvitesTheWeatherWithoutForcingIt() {
    LlmBehaviorPromptInput input = sampleInput();
    input.options.occasion = "welcome";
    const std::string user = buildBehaviorPromptMessages(input)[1].text;
    QVERIFY(user.find("weather line") != std::string::npos);
    QVERIFY(user.find("only when it fits naturally") != std::string::npos);
  }

  // 沒有天氣資料時整行省略 —— 空的 weather: 會讓模型自己編一個出來
  void weatherLineIsOmittedWithoutData() {
    LlmBehaviorPromptInput input = sampleInput();
    input.weather.clear();
    const std::string user = buildBehaviorPromptMessages(input)[1].text;
    QVERIFY(user.find("weather:") == std::string::npos);
  }

  void promptCarriesContext() {
    const auto messages = buildBehaviorPromptMessages(sampleInput());
    QCOMPARE(messages.size(), size_t(2));
    QCOMPARE(messages[0].role, std::string("system"));
    QCOMPARE(messages[1].role, std::string("user"));

    // system：輸出格式與 persona
    QVERIFY(messages[0].text.find("JSON array") != std::string::npos);
    QVERIFY(messages[0].text.find("A gentle secretary persona.") != std::string::npos);

    // user：occasion、時段、動作與表情（含命名語意）、台詞樣本
    const std::string& user = messages[1].text;
    QVERIFY(user.find("occasion: idle") != std::string::npos);
    QVERIFY(user.find("morning") != std::string::npos);  // hour 9 → morning
    QVERIFY(user.find("TapBody") != std::string::npos);
    QVERIFY(user.find("waving happily") != std::string::npos);
    QVERIFY(user.find("f01 (meaning: smiling)") != std::string::npos);
    QVERIFY(user.find("can_speak: true") != std::string::npos);
    QVERIFY(user.find("Line one.") != std::string::npos);
    QVERIFY(user.find("familiarity: 2 of 3") != std::string::npos);
  }

  // 長期記憶進 system（與 persona 同屬「你是誰／你知道什麼」）；
  // 最近講過的句子進 user 並明講不准重複。兩者空著時整段省略。
  void memoryAndRecentLinesInjected() {
    LlmBehaviorPromptInput input = sampleInput();
    input.memory = "The boss ships on Fridays.";
    input.recentLines = {"Line said A", "Line said B"};
    const auto messages = buildBehaviorPromptMessages(input);
    QVERIFY(messages[0].text.find("The boss ships on Fridays.") != std::string::npos);
    QVERIFY(messages[0].text.find("Long-term notes") != std::string::npos);
    QVERIFY(messages[1].text.find("Line said A") != std::string::npos);
    QVERIFY(messages[1].text.find("do NOT repeat") != std::string::npos);

    const auto plain = buildBehaviorPromptMessages(sampleInput());
    QVERIFY(plain[0].text.find("Long-term notes") == std::string::npos);
    QVERIFY(plain[1].text.find("do NOT repeat") == std::string::npos);
  }

  // 沒有 persona：明講不准說話
  void promptWithoutPersonaStaysQuiet() {
    LlmBehaviorPromptInput input = sampleInput();
    input.personaDescription.clear();
    const auto messages = buildBehaviorPromptMessages(input);
    QVERIFY(messages[0].text.find("No persona is set") != std::string::npos);
  }

  void parsesBareArray() {
    std::string error;
    const auto steps = parseBehaviorSteps(R"([{"action":"motion","group":"TapBody"},{"action":"speak","text":"Hi there."}])", /*allowMove=*/true, &error);
    QVERIFY(steps.has_value());
    QCOMPARE(steps->size(), size_t(2));
    QCOMPARE((*steps)[0].action, std::string("motion"));
    QCOMPARE((*steps)[1].text, std::string("Hi there."));
    QCOMPARE((*steps)[1].speakWait, true);
  }

  // 小模型硬加 code fence 與外包一層物件都要收得下
  void parsesFencedAndWrapped() {
    std::string error;
    const auto steps = parseBehaviorSteps("```json\n{\"steps\":[{\"action\":\"expression\",\"name\":\"f01\"}]}\n```", /*allowMove=*/true, &error);
    QVERIFY(steps.has_value());
    QCOMPARE(steps->size(), size_t(1));
    // 表情補上自動退回（與 IdleDirector 同值）
    QVERIFY((*steps)[0].holdMs.has_value());
  }

  // parameters / animate 不開放給自主表演；move 補滑行並夾 0..1；wait 夾上限
  void filtersAndNormalizes() {
    std::string error;
    const auto steps = parseBehaviorSteps(R"([{"action":"parameters","params":[{"id":"p","value":1}]},)"
                                          R"({"action":"move","x":1.7,"y":-0.2},)"
                                          R"({"action":"wait","ms":999999}])",
                                          /*allowMove=*/true, &error);
    QVERIFY(steps.has_value());
    QCOMPARE(steps->size(), size_t(2));
    QCOMPARE((*steps)[0].action, std::string("move"));
    QCOMPARE(*(*steps)[0].x, 1.0);
    QCOMPARE(*(*steps)[0].y, 0.0);
    QVERIFY((*steps)[0].glideMs.has_value());
    QCOMPARE((*steps)[1].ms, kMaxWaitMs);
  }

  // allowMove 為真時 preset 形狀照樣留下 —— 沒有這一條，哪天有人把 preset 一律
  // 當非法丟掉，下面那個「不准動時兩種形狀都要擋」的測試會照樣綠
  void keepsPresetMoveWhenAllowed() {
    std::string error;
    const auto steps = parseBehaviorSteps(R"([{"action":"move","preset":"bottom-left"}])", /*allowMove=*/true, &error);
    QVERIFY(steps.has_value());
    QCOMPARE(steps->size(), size_t(1));
    QCOMPARE(*(*steps)[0].preset, std::string("bottom-left"));
  }

  // 不准移動時 move 步驟整個丟掉（x/y 與 preset 兩種形狀都要擋）。
  // prompt 的 can_move 只是拿自然語言拜託模型，而 runAutonomous / moveTo
  // 都不看設定 —— 這裡是「隨機移動關著卻自己散步」的唯一關卡
  void dropsMoveStepsWhenNotAllowed() {
    std::string error;
    const auto steps = parseBehaviorSteps(R"([{"action":"move","x":0.2,"y":0.3},)"
                                          R"({"action":"move","preset":"bottom-left"},)"
                                          R"({"action":"speak","text":"Still here."}])",
                                          /*allowMove=*/false, &error);
    QVERIFY(steps.has_value());
    QCOMPARE(steps->size(), size_t(1));
    QCOMPARE((*steps)[0].action, std::string("speak"));
  }

  // can_move 為假時連 move 的格式都不列給模型看：沒看過格式就不太會生出來
  //（只是省白工，真正的關卡是上面那條）
  void promptHidesMoveShapeWhenCannotMove() {
    LlmBehaviorPromptInput input = sampleInput();
    input.world.canMove = false;
    const auto messages = buildBehaviorPromptMessages(input);
    QVERIFY(messages[0].text.find("\"action\":\"move\"") == std::string::npos);
    QVERIFY(messages[1].text.find("can_move: false") != std::string::npos);
    // 能動的時候照樣列出來
    QVERIFY(buildBehaviorPromptMessages(sampleInput())[0].text.find("\"action\":\"move\"") != std::string::npos);
  }

  // 空台詞與超長台詞的 speak 步驟整個丟掉；語音一律清空（用使用者設定的）
  void dropsBadSpeakSteps() {
    std::string error;
    const std::string longLine(300, 'a');
    const auto steps =
      parseBehaviorSteps(R"([{"action":"speak","text":"  "},{"action":"speak","text":")" + longLine + R"("},{"action":"speak","text":"ok","voice":"someone"}])", /*allowMove=*/true, &error);
    QVERIFY(steps.has_value());
    QCOMPARE(steps->size(), size_t(1));
    QCOMPARE((*steps)[0].text, std::string("ok"));
    QVERIFY(!(*steps)[0].voice.has_value());
  }

  // 空陣列＝「這一輪什麼都不做」，是合法輸出不是錯誤
  void emptyArrayIsValid() {
    std::string error;
    const auto steps = parseBehaviorSteps("[]", /*allowMove=*/true, &error);
    QVERIFY(steps.has_value());
    QVERIFY(steps->empty());
  }

  // 整包解不開 → nullopt（呼叫端退回 IdleDirector）
  void invalidInputFallsBack() {
    std::string error;
    QVERIFY(!parseBehaviorSteps("I would love to help!", /*allowMove=*/true, &error).has_value());
    QVERIFY(!error.empty());
    QVERIFY(!parseBehaviorSteps(R"({"nothing":true})", /*allowMove=*/true, &error).has_value());
    // 未知 action 走 parsePerformSteps 的嚴格路徑：整包拒絕
    QVERIFY(!parseBehaviorSteps(R"([{"action":"dance"}])", /*allowMove=*/true, &error).has_value());
  }

  // 超過 20 步截斷
  void capsStepCount() {
    std::string out = "[";
    for (int i = 0; i < 30; ++i) {
      if (i > 0) out += ",";
      out += R"({"action":"wait","ms":100})";
    }
    out += "]";
    std::string error;
    const auto steps = parseBehaviorSteps(out, /*allowMove=*/true, &error);
    QVERIFY(steps.has_value());
    QCOMPARE(steps->size(), kMaxPerformSteps);
  }
};

QTEST_GUILESS_MAIN(TestLlmBehaviorPrompt)
#include "test_llm_behavior_prompt.moc"
