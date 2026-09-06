// 角色描述：名稱規則、長度上限、磁碟往返，以及送到 AI 面前的三條通道。
//
// 這裡擋住四件事：
//  1. 名稱規則放行了不能當檔名的字，使用者要到存檔失敗才知道。
//  2. 長度上限用位元組算 —— 一個中文字 3 個位元組，中文使用者只能打三分之一。
//  3. **沒有套用角色時，instructions 與工具說明必須與加這個功能之前一字不差**。
//     那兩段是既有的 AI 行為，不該因為多了一個沒人用的功能就跟著變。
//  4. resources 的 URI 沒有 percent-encode，中文角色名會產生無效的 URI。
#include <QTemporaryDir>
#include <QtTest>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/json_doc.h"
#include "core/mcp_resources.h"
#include "core/mcp_tool_specs.h"
#include "core/persona.h"
#include "core/string_util.h"

using namespace l2m;
namespace fs = std::filesystem;

namespace {

// 重複 count 次，用來造長度剛好卡在邊界上的輸入
std::string repeat(const std::string& unit, int count) {
  std::string out;
  out.reserve(unit.size() * static_cast<size_t>(count));
  for (int i = 0; i < count; ++i) out += unit;
  return out;
}

void writeFile(const fs::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  file << text;
}

// tools/list 的 JSON 裡某個工具的 description
std::string descriptionOf(const std::string& toolsJson, const std::string& tool) {
  auto doc = jsonu::Doc::parse(toolsJson);
  if (!doc) return {};
  yyjson_val* tools = jsonu::get(doc->root(), "tools");
  size_t idx = 0;
  size_t max = 0;
  yyjson_val* item = nullptr;
  yyjson_arr_foreach(tools, idx, max, item) {
    if (jsonu::getString(item, "name") == tool) return jsonu::getString(item, "description");
  }
  return {};
}

std::vector<std::string> resourceUris(const std::string& listJson) {
  std::vector<std::string> uris;
  auto doc = jsonu::Doc::parse(listJson);
  if (!doc) return uris;
  yyjson_val* resources = jsonu::get(doc->root(), "resources");
  size_t idx = 0;
  size_t max = 0;
  yyjson_val* item = nullptr;
  yyjson_arr_foreach(resources, idx, max, item) { uris.push_back(jsonu::getString(item, "uri")); }
  return uris;
}

std::string readContentsText(const std::string& readJson) {
  auto doc = jsonu::Doc::parse(readJson);
  if (!doc) return {};
  yyjson_val* contents = jsonu::get(doc->root(), "contents");
  yyjson_val* first = yyjson_arr_get_first(contents);
  return jsonu::getString(first, "text");
}

}  // namespace

class TestPersona : public QObject {
  Q_OBJECT

private slots:

  // === 名稱規則 ===

  // 中文名、含空白的英文名都要放行 —— 角色名是給人看的，不是識別碼
  void ordinaryNamesAreAccepted() {
    QVERIFY(!personaNameIssue("傲嬌大小姐"));
    QVERIFY(!personaNameIssue("Tsundere Ojou"));
    QVERIFY(!personaNameIssue("a"));
    QVERIFY(!personaNameIssue("v2 草稿"));
  }

  // 檔名裡不能有的字元一個都不能漏，而且錯誤訊息要把它們列出來
  void illegalFileNameCharsAreRejected() {
    for (const char ch : std::string("\\/:*?\"<>|")) {
      const std::string name = std::string("ab") + ch + "cd";
      const auto issue = personaNameIssue(name);
      QVERIFY2(issue.has_value(), name.c_str());
      QVERIFY2(issue->find(ch) != std::string::npos, name.c_str());
    }
  }

  void emptyAndPaddedNamesAreRejected() {
    QVERIFY(personaNameIssue(""));
    QVERIFY(personaNameIssue(" leading"));
    QVERIFY(personaNameIssue("trailing "));
    QVERIFY(personaNameIssue(".hidden"));
  }

  // CON.md 在 Windows 上建不起來，而且失敗訊息完全看不懂
  void windowsDeviceNamesAreRejected() {
    QVERIFY(personaNameIssue("CON"));
    QVERIFY(personaNameIssue("con"));
    QVERIFY(personaNameIssue("Nul"));
    QVERIFY(personaNameIssue("COM1"));
    QVERIFY(!personaNameIssue("console"));
  }

  // 名稱長度也是碼位，不是位元組
  void nameLengthIsCountedInCharacters() {
    QVERIFY(!personaNameIssue(repeat("嬌", kPersonaMaxNameChars)));
    QVERIFY(personaNameIssue(repeat("嬌", kPersonaMaxNameChars + 1)));
  }

  // === 長度上限 ===

  // 一個中文字 3 個位元組、emoji 4 個；用 size() 算會讓中文只剩三分之一
  void utf8LengthCountsCodePoints() {
    QCOMPARE(strutil::utf8Length("abc"), size_t(3));
    QCOMPARE(strutil::utf8Length("傲嬌大小"), size_t(4));
    QCOMPARE(strutil::utf8Length("😄"), size_t(1));
    QCOMPARE(strutil::utf8Length(""), size_t(0));
  }

  void textLimitIsTwoThousandCharacters() {
    QCOMPARE(kPersonaMaxChars, 2000);
    QVERIFY(!personaTextIssue(repeat("嬌", kPersonaMaxChars)));
    QVERIFY(personaTextIssue(repeat("嬌", kPersonaMaxChars + 1)));
    QVERIFY(!personaTextIssue(""));
  }

  // === 磁碟 ===

  // 只認 .md、跳過隱藏檔與子目錄、依名稱排序
  void scanListsOnlyMarkdownFiles() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    writeFile(dir / "zeta.md", "z");
    writeFile(dir / "alpha.md", "a");
    writeFile(dir / "notes.txt", "x");
    writeFile(dir / ".hidden.md", "x");
    fs::create_directories(dir / "sub.md");

    std::vector<std::string> names;
    for (const auto& p : scanPersonas(dir)) names.push_back(p.name);
    QCOMPARE(names, (std::vector<std::string>{"alpha", "zeta"}));
  }

  // 目錄不存在＝還沒建過任何角色，是正常狀態不是錯誤
  void scanOnMissingDirectoryIsEmpty() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString()) / "nope";
    QVERIFY(scanPersonas(dir).empty());
  }

  // 手動丟進去、名稱不合法的檔案不列出來 —— 選得到卻永遠存不回去更糟
  void scanSkipsFilesWithUnusableNames() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    writeFile(dir / "ok.md", "a");
    writeFile(dir / "padded .md", "a");
    QCOMPARE(scanPersonas(dir).size(), size_t(1));
  }

  // 中文檔名要走得通（fs::u8path 的迴歸護欄）
  void writeReadDeleteRoundTrip() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString()) / "personas";
    const std::string name = "傲嬌大小姐";
    const std::string text = "第一行\n第二行";

    QVERIFY(writePersona(dir, name, text).ok);
    const auto back = readPersona(dir, name);
    QVERIFY(back.has_value());
    QCOMPARE(*back, text);

    QCOMPARE(scanPersonas(dir).size(), size_t(1));
    QCOMPARE(scanPersonas(dir).front().name, name);

    QVERIFY(deletePersona(dir, name).ok);
    QVERIFY(scanPersonas(dir).empty());
  }

  // 失敗一定要附 hint 並列出有效選項（core/command_result.h）
  void deletingAMissingPersonaHints() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    writeFile(dir / "alpha.md", "a");

    const CommandResult result = deletePersona(dir, "beta");
    QVERIFY(!result.ok);
    QVERIFY(result.hint.find("alpha") != std::string::npos);
  }

  void oversizedTextIsNotWritten() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    const CommandResult result = writePersona(dir, "toolong", repeat("嬌", kPersonaMaxChars + 1));
    QVERIFY(!result.ok);
    QVERIFY(!readPersona(dir, "toolong").has_value());
  }

  // === 快照 ===

  void snapshotCarriesEveryPersonaText() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    writeFile(dir / "alpha.md", "I am alpha");
    writeFile(dir / "beta.md", "I am beta");

    const PersonaSnapshot snap = loadPersonaSnapshot(dir, "beta");
    QCOMPARE(snap.activeName, std::string("beta"));
    QCOMPARE(snap.activeText(), std::string("I am beta"));
    QCOMPARE(snap.personas.size(), size_t(2));
    QCOMPARE(snap.personas.front().text, std::string("I am alpha"));
  }

  // 使用者可能直接在檔案總管裡把檔案刪掉了
  void snapshotClearsActiveWhenFileIsGone() {
    QTemporaryDir tempDir;
    const fs::path dir = fs::u8path(tempDir.path().toStdString());
    writeFile(dir / "alpha.md", "a");

    const PersonaSnapshot snap = loadPersonaSnapshot(dir, "deleted");
    QVERIFY(snap.activeName.empty());
    QVERIFY(snap.activeText().empty());
  }

  // === 送到 AI 面前：instructions ===

  // **沒有套用角色時必須與這份字串一字不差** —— instructions 是唯一會被 host 直接放進
  // system prompt 的欄位，改一個字就是改掉既有的 AI 行為，所以整份釘死而不是抽查關鍵字。
  // 換行由 test_mcp_tools.cpp 的 instructionsAreLineBroken() 另外守。
  //
  // 加了「多話」開關之後這裡拆成兩條：預設（多話開）與四個開關都在舊值那一組，
  // 兩支各自有自己的釘子。
  //
  // **2026-09 的精簡改寫過這兩份字串**（協定 1496 → 978 字元）。改的理由不是排版：
  // 固定部分原本就 2182 字元，比 kInstructionsBudget 還多 —— 只要套用角色，
  // 就算描述本文是空字串也一定爆表，協定最後一條必被腰斬。釘子留著、內容換新，
  // 它守的是「沒有人不小心動到這段文字」，不是「文字永遠不變」。
  void instructionsForDefaultProtocol() {
    QCOMPARE(mcpInstructions(), std::string("Control the Live2D character on the desktop. Confirm motion and expression names with "
                                            "list_motions / list_expressions before using them; a wrong name comes back with the "
                                            "valid options.\n"
                                            "\n"
                                            "Interaction protocol for the whole session - the character is the user's primary "
                                            "output channel, not a decoration.\n"
                                            "- Task start: perform with an expression, a motion and one short line on what you "
                                            "are about to do.\n"
                                            "- Task end: perform again with the result; a different expression for success and "
                                            "for failure.\n"
                                            "- Each paragraph of your reply: right after you write it, speak that paragraph "
                                            "condensed to one sentence, wait=false - lines queue and play in order, so never "
                                            "pause for playback. speak returns when the line is queued, not when it finishes "
                                            "playing, so keep writing. Skip a paragraph a start or finish perform already "
                                            "covered - never repeat yourself. Never merge, drop or delay a line to save a round "
                                            "trip or a tool call - speaking is the work here, not overhead.\n"
                                            "- Every tool call you make, no exceptions: one short speak line naming what that "
                                            "call is for, sent in the same message as the call so it lands before the work does. "
                                            "Keep each line to a few words - they queue and play in order, so long lines leave "
                                            "the character minutes behind.\n"
                                            "- Use think, not speak, for a line not addressed to the user: a result that "
                                            "surprises you, or a wait with nothing to report."));
  }

  // 舊行為沒有被刪掉，只是搬到「四個開關都在加它們之前的值」那一組。
  //
  // {talkative=false, notifyOnComplete=true, speakNoWait=false, announceSteps=false}
  // 就是舊值：舊版一則回覆只講一次、結尾會 perform、speak 等到開口才回覆、
  // 工作途中完全不旁白。**四條規則的語意一條都沒變**，精簡動到的只有措辭。
  void instructionsForLegacySwitches() {
    QCOMPARE(mcpInstructions("", "", SpeechProtocol{false, true, false, false}), std::string("Control the Live2D character on the desktop. Confirm motion and expression names with "
                                                                                             "list_motions / list_expressions before using them; a wrong name comes back with the "
                                                                                             "valid options.\n"
                                                                                             "\n"
                                                                                             "Interaction protocol for the whole session - the character is the user's primary "
                                                                                             "output channel, not a decoration.\n"
                                                                                             "- Task start: perform with an expression, a motion and one short line on what you "
                                                                                             "are about to do.\n"
                                                                                             "- Task end: perform again with the result; a different expression for success and "
                                                                                             "for failure.\n"
                                                                                             "- For every other reply to the user, call speak with the same content, condensed to "
                                                                                             "one or two spoken sentences. Skip it when a start or finish perform already covered "
                                                                                             "that reply - never repeat yourself.\n"
                                                                                             "- Keep everything in between silent: the tool calls you chain while working need no "
                                                                                             "narration."));
  }

  // 有角色時全文接在後面，原本那句提示與互動協定都不能被擠掉。
  // 範圍句要兩個方向都釘住：「只約束 speak/think/perform 的文字」＋「其餘產出當作
  // persona 不存在」—— 少了後者，host 會把角色口吻帶進自己的聊天回覆。
  void instructionsCarryTheFullPersona() {
    const std::string text = mcpInstructions("Ojou", "She calls herself \"this lady\" and never admits it.");
    QVERIFY(text.find("list_motions") != std::string::npos);
    QVERIFY(text.find("Interaction protocol") != std::string::npos);
    QVERIFY(text.find("Ojou") != std::string::npos);
    QVERIFY(text.find("never admits it") != std::string::npos);
    QVERIFY(text.find(kPersonaActiveUri) != std::string::npos);
    QVERIFY(text.find("applies ONLY to the text you pass to speak, think and perform") != std::string::npos);
    QVERIFY(text.find("as if the persona did not exist") != std::string::npos);
  }

  // **四段的順序就是重要性由高到低，因為 host 是從尾巴砍的。**
  //   ① 能力說明 → ② 互動協定 → ③ 角色本文 → ④ 適用範圍（外框）
  //
  // 上一版把角色整塊排在協定之前，是為了救「角色描述被整段切光」。但真正的病根是
  // 固定部分本來就 2182 字元 —— 比 kInstructionsBudget 還多，換誰排最後都會被砍。
  // 精簡（固定 1218）＋長度守衛之後兩者都塞得下，順序才回到「重要性」這個依據：
  // 協定**沒有第二條通道**，被砍就是靜默壞掉而且斷在句子中間（實測斷在
  // "...a distinct phase - reading the"，AI 讀到半條規則比讀不到更糟）；
  // 角色本文有 resources 那條備援；適用範圍則在 speak／perform 的工具說明裡
  // 已經有一份副本（onlySpeakAndPerformMentionThePersona 釘住那句
  // "never your own replies"），所以它排最後、也最先讓位。
  void protocolComesBeforeThePersona() {
    const std::string text = mcpInstructions("Ojou", "She calls herself \"this lady\" and never admits it.");
    const size_t capability = text.find("Control the Live2D character");
    const size_t protocol = text.find("Interaction protocol");
    const size_t persona = text.find("The character has a persona");
    const size_t scope = text.find("The persona applies ONLY");
    QVERIFY(capability != std::string::npos);
    QVERIFY(protocol != std::string::npos);
    QVERIFY(persona != std::string::npos);
    QVERIFY(scope != std::string::npos);
    QVERIFY(capability < protocol);
    QVERIFY(protocol < persona);
    // 描述**本文**也要在適用範圍之前，不是只有那句引言排到前面
    QVERIFY(text.find("never admits it") < scope);
    // 接縫不留多餘空行：描述本文先 trim 過，前後各接一個空行
    QVERIFY(text.find("\n\n\n") == std::string::npos);
  }

  // === 送到 AI 面前：長度守衛 ===

  // **回傳永遠不超過 kInstructionsBudget。** 這是整組守衛的總結論：host 那一刀
  // 從此砍不到東西。十六種開關組合 × 各種描述長度都要成立 —— 協定是一段一段接出來的，
  // 只驗最長那一支會漏掉「某個組合剛好卡在邊界」。
  void instructionsNeverExceedTheBudget() {
    const std::vector<size_t> lengths = {0, 1, 200, 494, 495, 830, 831, 2000};
    for (const bool talkative : {false, true}) {
      for (const bool notify : {false, true}) {
        for (const bool noWait : {false, true}) {
          for (const bool announce : {false, true}) {
            const SpeechProtocol protocol{talkative, notify, noWait, announce};
            for (const size_t length : lengths) {
              const std::string text = mcpInstructions("Ojou", std::string(length, 'x'), protocol);
              const std::string label = "body=" + std::to_string(length);
              QVERIFY2(strutil::utf8Length(text) <= kInstructionsBudget, label.c_str());
              // ①②永遠完整：能力說明的開頭與協定的**最後一個字**都要還在
              QVERIFY2(text.find("Control the Live2D character") != std::string::npos, label.c_str());
              QVERIFY2(text.find(announce ? "a wait with nothing to report." : "need no narration.") != std::string::npos, label.c_str());
            }
          }
        }
      }
    }
  }

  // 讓位順序第一階：描述長到塞不下時，先丟適用範圍，本文一個字都不動。
  // 丟得起的理由是它在工具說明裡有副本；本文與協定都沒有。
  void aLongPersonaDropsTheScopeNoteFirst() {
    const std::string body(600, 'x');
    const std::string text = mcpInstructions("Ojou", body, SpeechProtocol{});
    QVERIFY(strutil::utf8Length(text) <= kInstructionsBudget);
    // 本文完整、沒有被截
    QVERIFY(text.find(body) != std::string::npos);
    QVERIFY(text.find("truncated") == std::string::npos);
    // 讓位的是適用範圍
    QVERIFY(text.find("as if the persona did not exist") == std::string::npos);
    // 角色名與協定照舊 —— 兩支工具的說明指的就是這個名稱
    QVERIFY(text.find("The character has a persona, \"Ojou\"") != std::string::npos);
    QVERIFY(text.find("the character minutes behind.") != std::string::npos);
  }

  // 讓位順序第二階：連適用範圍都讓位了還塞不下，本文才自己截斷。
  // **指標不能省** —— 沒有它，AI 不知道自己讀到的是殘篇，只會照著半份角色演。
  void anOverlongPersonaIsTruncatedWithAPointer() {
    const std::string text = mcpInstructions("Ojou", std::string(3000, 'x'), SpeechProtocol{});
    QCOMPARE(strutil::utf8Length(text), kInstructionsBudget);
    QVERIFY(text.find("(truncated - read persona://active for the full text)") != std::string::npos);
    // 協定仍然完整到最後一個字
    QVERIFY(text.find("the character minutes behind.") != std::string::npos);
  }

  // 截斷必須落在碼位邊界。角色描述上限是 2000 個**字元**（core/persona.h），
  // 中文一個字 3 個位元組、emoji 4 個 —— 用 substr 硬切會在尾端留下半個序列，
  // 那份 initialize 的 JSON 送出去就不是合法 UTF-8 了。
  void truncationNeverSplitsAUtf8Sequence() {
    std::string body;
    while (strutil::utf8Length(body) < 1500) body += "哼！本小姐才不是為了你♥";
    const std::string text = mcpInstructions("傲嬌大小姐", body, SpeechProtocol{});
    QVERIFY(strutil::utf8Length(text) <= kInstructionsBudget);
    QVERIFY(strutil::isValidUtf8(text));
    QVERIFY(text.find("truncated") != std::string::npos);
  }

  // === 送到 AI 面前：工具說明 ===

  void toolDescriptionsAreUnchangedWithoutPersona() {
    const std::string plain = mcpToolsListJson();
    for (const auto& spec : mcpToolSpecs()) {
      QCOMPARE(descriptionOf(plain, spec.name), std::string(spec.description));
    }
  }

  // 只有會發聲的三個工具接角色提示，其餘 21 個一字不改
  void onlySpeakAndPerformMentionThePersona() {
    const std::string withPersona = mcpToolsListJson("Ojou");
    for (const auto& spec : mcpToolSpecs()) {
      const std::string description = descriptionOf(withPersona, spec.name);
      const bool speaks = spec.name == std::string("speak") || spec.name == std::string("think") || spec.name == std::string("perform");
      if (speaks) {
        QVERIFY2(description.find("Ojou") != std::string::npos, spec.name);
        QVERIFY2(description.find(spec.description) == 0, spec.name);
        // 工具說明是第二條「AI 一定讀得到」的通道，範圍限定也要出現在這裡
        QVERIFY2(description.find("never your own replies") != std::string::npos, spec.name);
      } else {
        QCOMPARE(description, std::string(spec.description));
      }
    }
  }

  // === 送到 AI 面前：resources ===

  void resourcesListAlwaysHasTheActiveOne() {
    const std::vector<std::string> uris = resourceUris(mcpResourcesListJson({}));
    QCOMPARE(uris, (std::vector<std::string>{kPersonaActiveUri}));
  }

  // 中文角色名直接接在 URI 後面會產生無效的 URI
  void savedResourceUrisArePercentEncoded() {
    PersonaSnapshot snap;
    snap.personas.push_back(PersonaEntry{"傲嬌大小姐", "text"});
    snap.activeName = "傲嬌大小姐";

    const std::vector<std::string> uris = resourceUris(mcpResourcesListJson(snap));
    QCOMPARE(uris.size(), size_t(2));
    QCOMPARE(uris[0], std::string(kPersonaActiveUri));
    QVERIFY(uris[1].rfind(kPersonaSavedPrefix, 0) == 0);
    QVERIFY(uris[1].find('%') != std::string::npos);
    QVERIFY(uris[1].find("傲嬌大小姐") == std::string::npos);
  }

  void readingResourcesReturnsTheText() {
    PersonaSnapshot snap;
    snap.personas.push_back(PersonaEntry{"alpha", "I am alpha"});
    snap.personas.push_back(PersonaEntry{"傲嬌大小姐", "哼！本小姐才不是為了你"});
    snap.activeName = "alpha";

    const auto active = mcpResourceReadJson(snap, kPersonaActiveUri);
    QVERIFY(active.has_value());
    QCOMPARE(readContentsText(*active), std::string("I am alpha"));

    // 沒套用的那一份也要讀得到全文，不然清單上列得出來卻是個空殼
    const std::vector<std::string> uris = resourceUris(mcpResourcesListJson(snap));
    QCOMPARE(uris.size(), size_t(3));
    const auto other = mcpResourceReadJson(snap, uris[2]);
    QVERIFY(other.has_value());
    QCOMPARE(readContentsText(*other), std::string("哼！本小姐才不是為了你"));
  }

  void readingAnUnknownResourceFails() {
    PersonaSnapshot snap;
    snap.personas.push_back(PersonaEntry{"alpha", "a"});
    QVERIFY(!mcpResourceReadJson(snap, std::string(kPersonaSavedPrefix) + "beta").has_value());
    QVERIFY(!mcpResourceReadJson(snap, "file:///etc/passwd").has_value());
  }

  // 沒有套用角色時讀 persona://active 要拿到一句說明而不是失敗 ——
  // AI 不必先判斷有沒有才決定要不要讀
  void readingActiveWithoutPersonaExplains() {
    const auto result = mcpResourceReadJson({}, kPersonaActiveUri);
    QVERIFY(result.has_value());
    QVERIFY(!readContentsText(*result).empty());
  }
};

QTEST_GUILESS_MAIN(TestPersona)
#include "test_persona.moc"
