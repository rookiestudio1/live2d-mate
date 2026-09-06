#include "persona_generate.h"

#include <algorithm>

#include "persona.h"
#include "string_util.h"

namespace l2m {

namespace {

const std::vector<PersonaGenStage>& stages() {
  static const std::vector<PersonaGenStage> order{PersonaGenStage::Description, PersonaGenStage::Lines,  PersonaGenStage::Welcome, PersonaGenStage::Greetings,
                                                  PersonaGenStage::Breaks,      PersonaGenStage::Petted, PersonaGenStage::Weather};
  return order;
}

// 各階段的指示。都以「Output only the lines/text themselves」收尾 ——
// 一階段一種內容，模型沒有機會把標題、說明或 markdown 混進來
std::string stageInstruction(PersonaGenStage stage) {
  switch (stage) {
    case PersonaGenStage::Description:
      return "Write the Character Description: personality, tone, how it addresses the "
             "user, quirks, what it never says. Written as instructions to an AI actor, "
             "max 1800 characters. Output only the description text itself - no heading, "
             "no markdown.";
    case PersonaGenStage::Lines:
      return "Now write 10-15 short idle mutterings this character would say to itself "
             "during the day. One per line, each under 100 characters, spoken not "
             "written. Output only the lines - no numbering, no bullets, no heading.";
    case PersonaGenStage::Welcome:
      return "Now write 3-5 greeting lines for when the app starts or the user comes "
             "back. One per line, under 100 characters. Output only the lines.";
    case PersonaGenStage::Greetings:
      return "Now write 4-8 time-of-day greetings. Each line may start with one of the "
             "English prefixes morning: / afternoon: / evening: / night: (they are "
             "parsing markers and must stay in English; no prefix = any time). One per "
             "line, under 100 characters. Output only the lines.";
    case PersonaGenStage::Breaks:
      return "Now write 3-5 gentle lines suggesting the user take a short break after "
             "working for a long stretch. In character, not naggy. One per line, under "
             "100 characters. Output only the lines.";
    case PersonaGenStage::Petted:
      return "Now write 3-5 short reactions to being petted by the user. One per line, "
             "under 100 characters. Output only the lines.";
    case PersonaGenStage::Weather:
      return "Now write 6-10 lines warning the user about bad weather that is about to "
             "start. Each line must begin with one of the English prefixes rain: / snow: "
             "/ thunder: / hot: / cold: / wind: (they are parsing markers and must stay "
             "in English; no prefix = any bad weather). Cover rain and snow at least. "
             "Practical and in character - suggest an umbrella, a coat, closing the "
             "window. One per line, under 100 characters. Output only the lines.";
  }
  return {};
}

// 剝行首的列表符號與編號（"- "、"* "、"• "、"1. "、"1) "…）：
// 指示說了不要，小模型照加是常態，照單全收會讓台詞開頭多一顆點
std::string stripListMarker(const std::string& line) {
  std::string out = strutil::trim(line);
  if (out.rfind("- ", 0) == 0 || out.rfind("* ", 0) == 0) {
    return strutil::trim(out.substr(2));
  }
  if (out.rfind("\xE2\x80\xA2", 0) == 0) {  // "•"
    return strutil::trim(out.substr(3));
  }
  size_t digits = 0;
  while (digits < out.size() && out[digits] >= '0' && out[digits] <= '9') ++digits;
  if (digits > 0 && digits <= 2 && digits < out.size() && (out[digits] == '.' || out[digits] == ')')) {
    return strutil::trim(out.substr(digits + 1));
  }
  return out;
}

// 回覆 → 一行一句：trim、剝列表符號、去空行、超長行直接丟
//（少一句無傷大雅，整包報錯要使用者重骰更煩）
std::vector<std::string> toLines(const std::string& text) {
  std::vector<std::string> out;
  size_t begin = 0;
  while (begin <= text.size()) {
    size_t end = text.find('\n', begin);
    if (end == std::string::npos) end = text.size();
    const std::string line = stripListMarker(text.substr(begin, end - begin));
    if (!line.empty() && strutil::utf8Length(line) <= static_cast<size_t>(kPersonaLineMaxChars)) {
      out.push_back(line);
    }
    if (end == text.size()) break;
    begin = end + 1;
  }
  return out;
}

}  // namespace

PersonaGenerateSession::PersonaGenerateSession(PersonaGenerateInput input) : input_(std::move(input)) {
  // reserved 區（未來保留字）原樣帶進結果，不參與生成
  doc_.reserved = input_.current.reserved;
}

bool PersonaGenerateSession::done() const { return stage_ >= stages().size(); }

PersonaGenStage PersonaGenerateSession::currentStage() const { return stages()[std::min(stage_, stages().size() - 1)]; }

int PersonaGenerateSession::stageIndex() const { return static_cast<int>(stage_); }

int PersonaGenerateSession::stageCount() const { return static_cast<int>(stages().size()); }

std::vector<LlmMessage> PersonaGenerateSession::nextMessages() const {
  std::vector<LlmMessage> messages;

  std::string system;
  system +=
    "You write persona content for a Live2D desktop mascot, one section per request in "
    "this conversation. Follow each instruction exactly and output ONLY the requested "
    "content - no headings, no markdown, no commentary, no code fences.\n";
  // **刻意不指定語言**：改由使用者在 brief 裡自己講（「用繁體中文寫」之類）。
  // 舊版是照 UI 語系硬寫一句 "Write ALL content in <語言>"，好處是小模型不會
  // 突然吐英文，代價是介面語系綁死了角色卡的語言 —— 想寫日語角色就得先把整個
  // UI 切成日文。取捨反過來之後，沒交代語言時模型會跟著現有卡片與 brief 走，
  // 空白卡片配空白 brief 才可能生出英文；輸入框的說明文字有提示可以指定。
  const std::string existing = serializePersonaDoc(input_.current);
  if (!strutil::trim(existing).empty()) {
    // 現有內容進 system：整場對話（每一階段）都看得到，身分不會走鐘
    system +=
      "Existing card (keep this character's identity; improve and expand, do not "
      "change who the character is):\n---\n" +
      existing + "\n---\n";
  }
  if (!strutil::trim(input_.brief).empty()) {
    system += "Direction from the user: " + strutil::trim(input_.brief) + "\n";
  }
  messages.push_back({"system", system});

  // 已完成階段的問答附回去 —— 這就是「同一場對話」：
  // 台詞階段看得到自己剛寫的描述，語氣才會一致
  for (const auto& turn : history_) messages.push_back(turn);

  messages.push_back({"user", stageInstruction(currentStage())});
  return messages;
}

std::string PersonaGenerateSession::accept(const std::string& responseText) {
  const std::string text = strutil::stripCodeFence(responseText);
  const PersonaGenStage stage = currentStage();

  if (stage == PersonaGenStage::Description) {
    if (text.empty()) return "The model returned an empty description.";
    // 與磁碟層（writePersona → personaTextIssue）同一句錯誤，同一個真相來源
    if (const auto issue = personaTextIssue(text)) return *issue;
    doc_.description = text;
  } else {
    std::vector<std::string> lines = toLines(text);
    // 台詞區是寬容的：空回覆就當這一區沒有內容，不擋整個流程
    switch (stage) {
      case PersonaGenStage::Lines:
        doc_.lines = std::move(lines);
        break;
      case PersonaGenStage::Welcome:
        doc_.welcome = std::move(lines);
        break;
      case PersonaGenStage::Greetings:
        doc_.greetings = std::move(lines);
        break;
      case PersonaGenStage::Breaks:
        doc_.breaks = std::move(lines);
        break;
      case PersonaGenStage::Petted:
        doc_.petted = std::move(lines);
        break;
      case PersonaGenStage::Weather:
        doc_.weatherAlerts = std::move(lines);
        break;
      case PersonaGenStage::Description:
        break;  // 不會到這裡
    }
  }

  // 問答進歷史（回覆存清理後的版本：短、乾淨，重送也省 token）
  history_.push_back({"user", stageInstruction(stage)});
  history_.push_back({"assistant", text});
  ++stage_;
  return {};
}

PersonaDoc PersonaGenerateSession::result() const { return doc_; }

}  // namespace l2m
