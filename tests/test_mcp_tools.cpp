// MCP 工具規格表與輸出格式。
//
// schema 是手寫的 JSON 字串，一個原始字串
// 被內容裡的 `)"` 提前截斷就會產出壞掉的 schema —— 而那種錯誤只會表現成
// 「AI 那邊看到的工具參數怪怪的」，從現象幾乎追不回來，所以必須釘死。
#include <QtTest>

#include <set>
#include <string>

#include "core/json_doc.h"
#include "core/mcp_tool_specs.h"
#include "core/perform_step.h"

using namespace l2m;

class TestMcpTools : public QObject {
  Q_OBJECT

private slots:
  // 24 個工具，名稱不重複
  void twentyFourUniqueTools() {
    const auto& specs = mcpToolSpecs();
    QCOMPARE(specs.size(), size_t(24));

    std::set<std::string> names;
    for (const auto& spec : specs) {
      QVERIFY2(names.insert(spec.name).second, spec.name);
    }
  }

  // 換模型的兩個出口都要在規格表裡。曾經整組拿掉過一次，是「AI 突然不會換模型了」
  // 這種只有實際連上去用才會發現的靜默退化，所以正反兩面都釘住。
  void modelSwitchingIsExposed() {
    QVERIFY(mcpToolExists("list_models"));
    QVERIFY(mcpToolExists("switch_model"));
  }

  // 唯讀工具正好是那六個查詢類的
  void readOnlyToolsAreTheQueries() {
    const std::set<std::string> expected{"list_models", "list_motions", "list_expressions", "list_parameters", "list_voices", "get_state"};
    std::set<std::string> actual;
    for (const auto& spec : mcpToolSpecs()) {
      if (spec.readOnly) actual.insert(spec.name);
    }
    QCOMPARE(actual, expected);
  }

  // 每個 schema 都是合法 JSON，且是 type: object
  void everySchemaIsValidJson() {
    for (const auto& spec : mcpToolSpecs()) {
      auto doc = jsonu::Doc::parse(spec.inputSchema);
      QVERIFY2(doc.has_value() && doc->root() != nullptr, spec.name);
      QCOMPARE(jsonu::getString(doc->root(), "type"), std::string("object"));
      QVERIFY2(jsonu::get(doc->root(), "properties") != nullptr, spec.name);
    }
  }

  // 說明不能是空的：那是 AI 挑工具的唯一依據
  void everyToolHasDescription() {
    for (const auto& spec : mcpToolSpecs()) {
      QVERIFY2(spec.description != nullptr && std::string(spec.description).size() > 20, spec.name);
    }
  }

  // 欄位名一律 snake_case 且把單位寫進名字，schema 沒被截斷才看得到這些
  void schemasUseSnakeCaseWithUnits() {
    const auto schemaOf = [](const char* name) {
      for (const auto& spec : mcpToolSpecs()) {
        if (std::string(spec.name) == name) return std::string(spec.inputSchema);
      }
      return std::string();
    };

    QVERIFY(schemaOf("set_expression").find("hold_ms") != std::string::npos);
    QVERIFY(schemaOf("animate").find("at_ms") != std::string::npos);
    QVERIFY(schemaOf("animate").find("fade_in_ms") != std::string::npos);
    QVERIFY(schemaOf("set_parameters").find("duration_ms") != std::string::npos);
    QVERIFY(schemaOf("schedule").find("delay_ms") != std::string::npos);
  }

  // 被截斷的原始字串會讓說明整段消失，這裡挑幾個最容易中招的釘住
  void schemasKeepParenthesesInDescriptions() {
    const auto schemaOf = [](const char* name) {
      for (const auto& spec : mcpToolSpecs()) {
        if (std::string(spec.name) == name) return std::string(spec.inputSchema);
      }
      return std::string();
    };

    // "(e.g. Idle, TapBody)" 後面緊接著引號，正是會提前結束 R"( )" 的形狀
    QVERIFY(schemaOf("play_motion").find("(e.g. Idle, TapBody)") != std::string::npos);
    QVERIFY(schemaOf("play_motion").find("so it plays)") != std::string::npos);
    QVERIFY(schemaOf("set_expression").find("(e.g. F01)") != std::string::npos);
    // schema 尾巴還在，代表整份沒有被截掉
    QVERIFY(schemaOf("play_motion").find("\"required\":[\"group\"]") != std::string::npos);
  }

  // perform 的 speak 步驟要收得下 thinking，schema 與解析**兩邊一起釘**：
  // 只加解析、schema 沒宣告，AI 永遠不會知道有這個欄位；只寫 schema 沒解析，
  // AI 照著送卻靜靜被當成一般台詞唸出來 —— 兩種都是不會有人回報的靜默失敗。
  // 順便釘住「沒寫就是 false」：預設值一旦漂掉，整場表演會全部變成內心話。
  void performSpeakStepCarriesThinking() {
    const auto schemaOf = [](const char* name) {
      for (const auto& spec : mcpToolSpecs()) {
        if (std::string(spec.name) == name) return std::string(spec.inputSchema);
      }
      return std::string();
    };
    QVERIFY(schemaOf("perform").find("\"thinking\"") != std::string::npos);
    // 說明裡也要提，不然 AI 只有翻 schema 才找得到
    for (const auto& spec : mcpToolSpecs()) {
      if (std::string(spec.name) == "perform") QVERIFY(std::string(spec.description).find("thinking:true") != std::string::npos);
    }

    auto doc = jsonu::Doc::parse(R"J([{"action":"speak","text":"hm","thinking":true},{"action":"speak","text":"hi"}])J");
    QVERIFY(doc.has_value());
    std::string error;
    const auto steps = parsePerformSteps(doc->root(), &error);
    QVERIFY2(steps.has_value(), error.c_str());
    QCOMPARE(steps->size(), size_t(2));
    QCOMPARE((*steps)[0].thinking, true);
    QCOMPARE((*steps)[1].thinking, false);
  }

  // tools/list 的輸出：24 筆，每筆帶 name / description / inputSchema / annotations / _meta
  void toolsListJsonShape() {
    auto doc = jsonu::Doc::parse(mcpToolsListJson());
    QVERIFY(doc.has_value());

    yyjson_val* tools = jsonu::get(doc->root(), "tools");
    QVERIFY(tools && yyjson_is_arr(tools));
    QCOMPARE(yyjson_arr_size(tools), size_t(24));

    yyjson_arr_iter iter;
    yyjson_arr_iter_init(tools, &iter);
    yyjson_val* item = nullptr;
    while ((item = yyjson_arr_iter_next(&iter))) {
      const std::string name = jsonu::getString(item, "name");
      QVERIFY(!name.empty());
      QVERIFY2(!jsonu::getString(item, "description").empty(), name.c_str());

      yyjson_val* schema = jsonu::get(item, "inputSchema");
      QVERIFY2(schema && yyjson_is_obj(schema), name.c_str());
      // schema 解析失敗時 toolsListJson 會塞一個空物件當保險，那代表出事了
      QVERIFY2(jsonu::getString(schema, "type") == "object", name.c_str());

      yyjson_val* annotations = jsonu::get(item, "annotations");
      QVERIFY2(annotations && jsonu::get(annotations, "readOnlyHint") != nullptr, name.c_str());

      // 少了這個鍵，host 會把工具延後載入（見 mcpToolsListJson 的註解）
      yyjson_val* meta = jsonu::get(item, "_meta");
      QVERIFY2(meta && yyjson_is_true(jsonu::get(meta, "anthropic/alwaysLoad")), name.c_str());
    }
  }

  // 工具查詢
  void lookupHelpers() {
    QVERIFY(mcpToolExists("speak"));
    QVERIFY(!mcpToolExists("nope"));
    QVERIFY(mcpToolIsReadOnly("get_state"));
    QVERIFY(!mcpToolIsReadOnly("speak"));
  }

  // 輸出格式：成功是 content 陣列，失敗多一個 isError 並把 hint 接在後面
  void outputShapes() {
    auto ok = jsonu::Doc::parse(mcpTextOutput("done"));
    QVERIFY(ok.has_value());
    yyjson_val* content = jsonu::get(ok->root(), "content");
    QVERIFY(content && yyjson_is_arr(content));
    QCOMPARE(jsonu::getString(yyjson_arr_get_first(content), "text"), std::string("done"));
    QVERIFY(jsonu::get(ok->root(), "isError") == nullptr);

    auto bad = jsonu::Doc::parse(mcpErrorOutput("Model \"x\" not found", "Available models: A, B"));
    QVERIFY(bad.has_value());
    QVERIFY(yyjson_get_bool(jsonu::get(bad->root(), "isError")));
    const std::string text = jsonu::getString(yyjson_arr_get_first(jsonu::get(bad->root(), "content")), "text");
    QCOMPARE(text, std::string("Model \"x\" not found\nAvailable models: A, B"));
  }

  // 加 note 不會破壞原本的物件
  void withNoteKeepsPayload() {
    const std::string merged = mcpWithNote(R"({"model":"Hiyori"})", "hello");
    auto doc = jsonu::Doc::parse(merged);
    QVERIFY(doc.has_value());
    QCOMPARE(jsonu::getString(doc->root(), "model"), std::string("Hiyori"));
    QCOMPARE(jsonu::getString(doc->root(), "note"), std::string("hello"));
  }

  // 三段固定註記都在，而且不是空的
  void notesArePresent() {
    QVERIFY(std::string(kNotNamedNote).find("Name Motions & Expressions") != std::string::npos);
    QVERIFY(std::string(kParamExpressionNote).find("mutually exclusive") != std::string::npos);
    QVERIFY(std::string(kParameterRoleNote).find("physics-output") != std::string::npos);
  }

  // initialize 的說明要叫 AI 先確認名稱
  void instructionsTellAiToListFirst() {
    const std::string text = mcpInstructions();
    QVERIFY(text.find("list_motions") != std::string::npos);
    QVERIFY(text.find("list_expressions") != std::string::npos);
  }

  // 互動協定的清單必須真的斷行。
  // C++ 相鄰字串字面值是直接黏起來的，漏一個 "\n" 就會變成
  // "...valid options.Interaction protocol...session:- When you start..." 這種連字，
  // 整份協定擠成一段送進 host 的 system prompt。踩過一次，所以釘住。
  // 加了四個開關之後，十六種組合都要各自成立 —— 條目是接出來的，
  // 接壞的症狀（少一個 "\n"、多一個空白）在任何一種組合都可能單獨發生。
  void instructionsAreLineBroken() {
    for (const bool talkative : {false, true}) {
      for (const bool notify : {false, true}) {
        for (const bool noWait : {false, true}) {
          for (const bool announce : {false, true}) {
            const std::string text = mcpInstructions("", "", SpeechProtocol{talkative, notify, noWait, announce});

            // 能力說明與協定之間空一行
            QVERIFY(text.find("options.\n\nInteraction protocol") != std::string::npos);

            // 每個條目都自己起一行，而且沒有句子直接黏在 "-" 前面。
            // 關掉「完成時通知」會少掉結尾 perform 那一條，所以基底是 4 或 3。
            // announceSteps 對第四條是**換掉**而不是多加（所以不影響基底），
            // 但它另外接上 think 那一條 —— 開著時整份多一條。
            QCOMPARE(countOccurrences(text, "\n- "), size_t((notify ? 4 : 3) + (announce ? 1 : 0)));
            QVERIFY(text.find(".- ") == std::string::npos);

            // 續行不該留下當初用來排版的行首空白。條目是用 += 接出來的，
            // 接縫兩邊各留一個空白就會變成兩個 —— 這裡一併守住。
            // speakNoWait 關掉時那段註記是空字串，接縫更容易多出一個空白。
            QVERIFY(text.find("  ") == std::string::npos);
          }
        }
      }
    }
  }

  // 「工作途中報告」開關要**換掉**最後一條，不是兩條並存 ——
  // 「中間全部安靜」與「每支工具呼叫報一句」同時出現的話，AI 讀完不知道要聽哪一句。
  // 另外釘住三件事：「no exceptions」—— 粒度從「階段」改成「每一支」之後，
  // 這幾個字就是整條規則的重點，少了它 AI 會自己劃出一個涵蓋整段工作的「階段」
  // 而全程安靜；「跟那支工具呼叫同一則訊息送出」少了就是每報一句多一趟完整往返；
  // 「會落後好幾分鐘」則是節流的理由，拿掉就沒東西擋語音積壓。
  void announceStepsReplacesTheSilentRule() {
    for (const bool talkative : {false, true}) {
      const std::string on = mcpInstructions("", "", SpeechProtocol{talkative, true, true, true});
      QVERIFY(on.find("Every tool call you make, no exceptions") != std::string::npos);
      QVERIFY(on.find("same message as the call") != std::string::npos);
      QVERIFY(on.find("minutes behind") != std::string::npos);
      QVERIFY(on.find("Keep everything in between silent") == std::string::npos);
      // think 那一條跟著這個開關走：協定說「工作途中全部安靜」時再叫它想出聲是自相矛盾。
      // 而開著時它必須在 —— think 的工具說明只讓 AI「可以用」，協定才讓它「一定要用」。
      QVERIFY(on.find("Use think, not speak") != std::string::npos);

      const std::string off = mcpInstructions("", "", SpeechProtocol{talkative, true, true, false});
      QVERIFY(off.find("Keep everything in between silent") != std::string::npos);
      QVERIFY(off.find("Every tool call you make") == std::string::npos);
      QVERIFY(off.find("Use think") == std::string::npos);
    }
  }

  // speak 變成「排進佇列就回覆」時一定要告訴 AI。
  // 少了這句，AI 收到成功回覆會以為那一句已經唸完，接著就可能送 stop_speaking
  // 收尾，把佇列裡還沒唸的整串砍掉 —— 這是靜默的，只會表現成「講到一半沒了」。
  void speakNoWaitIsAnnouncedToTheAi() {
    for (const bool talkative : {false, true}) {
      const std::string on = mcpInstructions("", "", SpeechProtocol{talkative, true, true});
      QVERIFY(on.find("speak returns when the line is queued") != std::string::npos);
      QVERIFY(on.find("not when it finishes playing") != std::string::npos);

      const std::string off = mcpInstructions("", "", SpeechProtocol{talkative, true, false});
      QVERIFY(off.find("speak returns when the line is queued") == std::string::npos);
    }
  }

  // 「多話」開關真的換掉了說話規則，而不是兩種組合送出同一份文字。
  // wait=false 那句是硬性的：speak 一律佔長呼叫名額（core/mcp_host.h），
  // 上限 4，AI 若每段都等播完才回，第 5 段會被硬拒絕。
  void talkativeSwitchesTheSpeakingRule() {
    const std::string on = mcpInstructions("", "", SpeechProtocol{true, true});
    QVERIFY(on.find("Each paragraph of your reply") != std::string::npos);
    QVERIFY(on.find("wait=false") != std::string::npos);
    QVERIFY(on.find("For every other reply") == std::string::npos);

    const std::string off = mcpInstructions("", "", SpeechProtocol{false, true});
    QVERIFY(off.find("condensed to one or two spoken sentences") != std::string::npos);
    QVERIFY(off.find("Each paragraph of your reply") == std::string::npos);
    QVERIFY(off.find("wait=false") == std::string::npos);
  }

  // 「多話」與「省一趟往返」是互斥的，不能同時寫進協定。
  // host 自己的 system prompt 幾乎都帶著「獨立的工具呼叫合併成同一則訊息、少跑
  // round trip」這一條；協定裡只要再出現一次省成本的措辭，AI 就會把它推廣成
  // 「講少一點比較好」，一場對話撐久了退化成只在頭尾各講一句（實測就是這樣壞的）。
  // 所以分工釘死：
  //   talkative=true  → 不准提省往返，改成一句反向的硬性規定；
  //   talkative=false → 才提省往返（安靜模式本來就該少跑）。
  // 兩種 announceSteps 都要成立 —— 省往返那句措辭原本是掛在階段規則上的。
  void talkativeOutranksRoundTripEconomy() {
    for (const bool announce : {false, true}) {
      const std::string on = mcpInstructions("", "", SpeechProtocol{true, true, true, announce});
      QVERIFY(on.find("Never merge, drop or delay a line to save a round trip or a tool call") != std::string::npos);
      QVERIFY(on.find("costs no extra round trip") == std::string::npos);

      const std::string off = mcpInstructions("", "", SpeechProtocol{false, true, true, announce});
      QVERIFY(off.find("Never merge, drop or delay a line") == std::string::npos);
      // 省往返的措辭只掛在階段規則上，關掉那條就整句不見（舊行為本來就不提）
      QCOMPARE(off.find("costs no extra round trip") != std::string::npos, announce);
    }
  }

  // 關掉「完成時通知」要把結尾 perform 整條抽掉，而且說話規則尾巴的「別重複」
  // 也不能再提 finish —— 沒有結尾 perform 時那句話是假的，AI 會去對照一個
  // 根本不存在的收尾動作。兩種 talkative 都要成立。
  void notifyOnCompleteRemovesTheClosingPerform() {
    for (const bool talkative : {false, true}) {
      const std::string on = mcpInstructions("", "", SpeechProtocol{talkative, true});
      QVERIFY(on.find("- Task end:") != std::string::npos);
      QVERIFY(on.find("a start or finish perform") != std::string::npos);

      const std::string off = mcpInstructions("", "", SpeechProtocol{talkative, false});
      QVERIFY(off.find("- Task end:") == std::string::npos);
      QVERIFY(off.find("finish perform") == std::string::npos);
      QVERIFY(off.find("the start perform already covered") != std::string::npos);

      // 開頭的 perform 不受這個開關影響
      QVERIFY(off.find("- Task start:") != std::string::npos);
    }
  }

private:
  static size_t countOccurrences(const std::string& haystack, const std::string& needle) {
    size_t count = 0;
    for (size_t at = haystack.find(needle); at != std::string::npos; at = haystack.find(needle, at + needle.size())) {
      ++count;
    }
    return count;
  }
};

QTEST_APPLESS_MAIN(TestMcpTools)
#include "test_mcp_tools.moc"
