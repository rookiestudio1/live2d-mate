#include "persona_doc.h"

#include <algorithm>
#include <utility>

#include "persona.h"
#include "string_util.h"

namespace l2m {

namespace {

// 只有這幾個英文標題會被當成分區點（trim 後大小寫不敏感）。
// Greeting by Time 從第一版就放進清單：之後加第四區時只是補 UI，
// 不必再處理格式遷移。
constexpr const char* kDescriptionHeading = "Character Description";
constexpr const char* kLinesHeading = "Dialogue List";
constexpr const char* kWelcomeHeading = "Welcome Text";
constexpr const char* kGreetingsHeading = "Greeting by Time";
constexpr const char* kBreaksHeading = "Break Reminder";
constexpr const char* kPettedHeading = "Petted";
constexpr const char* kWeatherHeading = "Weather Alert";

const std::vector<std::string>& reservedHeadings() {
  static const std::vector<std::string> headings{kDescriptionHeading, kLinesHeading, kWelcomeHeading, kGreetingsHeading, kBreaksHeading, kPettedHeading, kWeatherHeading};
  return headings;
}

// 這一行是不是保留字標題（只認一級標題「# 」開頭）；是的話回標準寫法
std::optional<std::string> matchReservedHeading(const std::string& line) {
  if (line.rfind("# ", 0) != 0) return std::nullopt;
  const std::string title = strutil::trim(line.substr(2));
  for (const auto& heading : reservedHeadings()) {
    if (strutil::equalsInsensitive(title, heading)) return heading;
  }
  return std::nullopt;
}

std::vector<std::string> splitLines(const std::string& raw) {
  std::vector<std::string> lines;
  std::string current;
  for (const char ch : raw) {
    if (ch == '\n') {
      lines.push_back(std::move(current));
      current.clear();
    } else if (ch != '\r') {  // CRLF 一律在這裡吸掉
      current += ch;
    }
  }
  lines.push_back(std::move(current));
  return lines;
}

// 去掉一段內文前後的空行（行內縮排保留）
std::string trimBlankEdges(const std::vector<std::string>& lines) {
  size_t begin = 0;
  size_t end = lines.size();
  while (begin < end && strutil::trim(lines[begin]).empty()) ++begin;
  while (end > begin && strutil::trim(lines[end - 1]).empty()) --end;
  std::string out;
  for (size_t i = begin; i < end; ++i) {
    if (i > begin) out += '\n';
    out += lines[i];
  }
  return out;
}

}  // namespace

PersonaDoc parsePersonaDoc(const std::string& raw) {
  PersonaDoc doc;
  const std::vector<std::string> lines = splitLines(raw);

  // 先掃一遍：一個保留字標題都沒有 → 整份原封不動當 description。
  // 這條壞掉就是所有現有角色卡一起壞，所以刻意連換行格式都不動。
  const bool hasReserved = std::any_of(lines.begin(), lines.end(), [](const std::string& line) { return matchReservedHeading(line).has_value(); });
  if (!hasReserved) {
    doc.description = raw;
    return doc;
  }

  // 分區切割。第一個保留字標題之前的文字視為 description 的開頭。
  std::string section = kDescriptionHeading;
  std::vector<std::string> body;

  const auto flush = [&doc, &section, &body] {
    const std::string text = trimBlankEdges(body);
    body.clear();
    if (section == kDescriptionHeading) {
      if (text.empty()) return;
      if (!doc.description.empty()) doc.description += "\n\n";
      doc.description += text;
      return;
    }
    std::vector<std::string>* target = nullptr;
    if (section == kLinesHeading)
      target = &doc.lines;
    else if (section == kWelcomeHeading)
      target = &doc.welcome;
    else if (section == kGreetingsHeading)
      target = &doc.greetings;
    else if (section == kBreaksHeading)
      target = &doc.breaks;
    else if (section == kPettedHeading)
      target = &doc.petted;
    else if (section == kWeatherHeading)
      target = &doc.weatherAlerts;
    if (target) {
      for (const auto& line : splitLines(text)) {
        const std::string trimmed = strutil::trim(line);
        if (!trimmed.empty()) target->push_back(trimmed);
      }
      return;
    }
    // 保留字裡還沒有 UI 的區塊：原樣保留（同名多段就合併）
    for (auto& entry : doc.reserved) {
      if (entry.first == section) {
        if (!text.empty()) {
          if (!entry.second.empty()) entry.second += "\n\n";
          entry.second += text;
        }
        return;
      }
    }
    doc.reserved.emplace_back(section, text);
  };

  for (const auto& line : lines) {
    if (const auto heading = matchReservedHeading(line)) {
      flush();
      section = *heading;
      continue;
    }
    body.push_back(line);
  }
  flush();
  return doc;
}

std::string serializePersonaDoc(const PersonaDoc& doc) {
  // 沒有任何分區內容：自由格式維持原樣，不硬加標題
  if (doc.lines.empty() && doc.welcome.empty() && doc.greetings.empty() && doc.breaks.empty() && doc.petted.empty() && doc.weatherAlerts.empty() && doc.reserved.empty()) {
    return doc.description;
  }

  std::string out;
  out += std::string("# ") + kDescriptionHeading + "\n";
  if (!doc.description.empty()) out += doc.description + "\n";
  if (!doc.lines.empty()) {
    out += std::string("\n# ") + kLinesHeading + "\n";
    for (const auto& line : doc.lines) out += line + "\n";
  }
  if (!doc.welcome.empty()) {
    out += std::string("\n# ") + kWelcomeHeading + "\n";
    for (const auto& line : doc.welcome) out += line + "\n";
  }
  if (!doc.greetings.empty()) {
    out += std::string("\n# ") + kGreetingsHeading + "\n";
    for (const auto& line : doc.greetings) out += line + "\n";
  }
  if (!doc.breaks.empty()) {
    out += std::string("\n# ") + kBreaksHeading + "\n";
    for (const auto& line : doc.breaks) out += line + "\n";
  }
  if (!doc.petted.empty()) {
    out += std::string("\n# ") + kPettedHeading + "\n";
    for (const auto& line : doc.petted) out += line + "\n";
  }
  if (!doc.weatherAlerts.empty()) {
    out += std::string("\n# ") + kWeatherHeading + "\n";
    for (const auto& line : doc.weatherAlerts) out += line + "\n";
  }
  // 還沒有對應欄位的保留區塊一律放最後
  for (const auto& entry : doc.reserved) {
    out += "\n# " + entry.first + "\n";
    if (!entry.second.empty()) out += entry.second + "\n";
  }
  return out;
}

std::optional<std::string> personaLinesIssue(const std::vector<std::string>& lines, const char* sectionName) {
  size_t total = 0;
  for (const auto& line : lines) {
    const size_t length = strutil::utf8Length(line);
    total += length;
    if (length > static_cast<size_t>(kPersonaLineMaxChars)) {
      return std::string(sectionName) + " has a line that is too long (" + std::to_string(length) + " characters, max " + std::to_string(kPersonaLineMaxChars) + " per line)";
    }
  }
  if (total > static_cast<size_t>(kPersonaLinesMaxChars)) {
    return std::string(sectionName) + " is too long (" + std::to_string(total) + " characters, max " + std::to_string(kPersonaLinesMaxChars) + ")";
  }
  return std::nullopt;
}

std::optional<std::string> personaDocIssue(const PersonaDoc& doc) {
  if (const auto issue = personaTextIssue(doc.description)) return issue;
  if (const auto issue = personaLinesIssue(doc.lines, "Dialogue list")) return issue;
  if (const auto issue = personaLinesIssue(doc.welcome, "Welcome text")) return issue;
  if (const auto issue = personaLinesIssue(doc.greetings, "Greeting by time")) return issue;
  if (const auto issue = personaLinesIssue(doc.breaks, "Break reminder")) return issue;
  if (const auto issue = personaLinesIssue(doc.petted, "Petted reaction")) return issue;
  if (const auto issue = personaLinesIssue(doc.weatherAlerts, "Weather alert")) return issue;
  return std::nullopt;
}

}  // namespace l2m
