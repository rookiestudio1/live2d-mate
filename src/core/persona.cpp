#include "persona.h"

#include <algorithm>
#include <fstream>
#include <set>

#include "json_doc.h"
#include "persona_doc.h"
#include "string_util.h"

namespace l2m {

namespace fs = std::filesystem;

namespace {

// 檔名裡不能出現的字元。Windows 的一律擋掉，順便也擋掉 POSIX 的 '/'，
// 這樣同一份 personas 資料夾在三個平台之間搬過去都還開得起來。
constexpr const char* kIllegalNameChars = "\\/:*?\"<>|";

// Windows 的裝置名：CON.md 這種檔案在 Windows 上建不起來，而且失敗訊息
// 是「存取被拒」之類完全看不懂的東西，所以在這裡就先攔下來。
bool isReservedDeviceName(const std::string& name) {
  static const std::set<std::string> reserved{"con",  "prn",  "aux",  "nul",  "com1", "com2", "com3", "com4", "com5", "com6", "com7",
                                              "com8", "com9", "lpt1", "lpt2", "lpt3", "lpt4", "lpt5", "lpt6", "lpt7", "lpt8", "lpt9"};
  return reserved.count(strutil::toLowerAscii(name)) > 0;
}

// 與 model_scanner.cpp 的 localeLess 同一條規則：先做大小寫不敏感的比較，
// 相同時再用原字串分勝負（穩定，且兩份清單的次序看起來一致）。
bool localeLess(const std::string& a, const std::string& b) {
  const std::string la = strutil::toLowerAscii(a);
  const std::string lb = strutil::toLowerAscii(b);
  if (la != lb) return la < lb;
  return a < b;
}

fs::path pathFor(const fs::path& dir, const std::string& name) { return dir / fs::u8path(name + kPersonaSuffix); }

std::string joinWith(const std::vector<std::string>& items, const std::string& sep) {
  std::string out;
  for (size_t i = 0; i < items.size(); ++i) {
    if (i > 0) out += sep;
    out += items[i];
  }
  return out;
}

}  // namespace

std::string PersonaSnapshot::activeText() const {
  if (activeName.empty()) return {};
  for (const auto& entry : personas) {
    if (entry.name == activeName) return entry.text;
  }
  return {};
}

// ── 驗證 ──

std::optional<std::string> personaNameIssue(const std::string& name) {
  if (name.empty()) return "Persona name must not be empty";
  if (strutil::trim(name) != name) {
    return "Persona name must not start or end with spaces";
  }
  if (name[0] == '.') return "Persona name must not start with a dot";
  if (name.find_first_of(kIllegalNameChars) != std::string::npos) {
    return std::string("Persona name must not contain any of ") + kIllegalNameChars;
  }
  for (const char ch : name) {
    if (static_cast<unsigned char>(ch) < 0x20) {
      return "Persona name must not contain control characters";
    }
  }
  if (isReservedDeviceName(name)) {
    return "\"" + name + "\" is a reserved device name on Windows; pick another name";
  }
  if (strutil::utf8Length(name) > static_cast<size_t>(kPersonaMaxNameChars)) {
    return "Persona name is too long (max " + std::to_string(kPersonaMaxNameChars) + " characters)";
  }
  return std::nullopt;
}

std::optional<std::string> personaTextIssue(const std::string& text) {
  const size_t length = strutil::utf8Length(text);
  if (length > static_cast<size_t>(kPersonaMaxChars)) {
    return "Persona description is too long (" + std::to_string(length) + " characters, max " + std::to_string(kPersonaMaxChars) + ")";
  }
  return std::nullopt;
}

std::string personaListHint(const std::vector<PersonaInfo>& personas, const std::string& personasDir) {
  if (personas.empty()) return "Personas folder is empty: " + personasDir;
  std::vector<std::string> names;
  names.reserve(personas.size());
  for (const auto& p : personas) names.push_back(p.name);
  return "Available personas: " + joinWith(names, ", ");
}

// ── 磁碟 ──

std::vector<PersonaInfo> scanPersonas(const fs::path& dir) {
  std::vector<PersonaInfo> found;

  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  // 目錄不存在就是「還沒有任何角色」，不是錯誤
  if (ec) return found;

  const std::string suffix = kPersonaSuffix;
  for (const auto& entry : it) {
    std::error_code statEc;
    if (entry.is_directory(statEc)) continue;

    const std::string filename = entry.path().filename().u8string();
    if (filename.empty() || filename[0] == '.') continue;
    if (!strutil::endsWithInsensitive(filename, suffix)) continue;

    const std::string name = filename.substr(0, filename.size() - suffix.size());
    // 手動丟進資料夾的檔案不一定合法（例如 "  留白.md"）；名稱過不了驗證的
    // 不列出來，不然使用者會選得到卻永遠存不回去
    if (name.empty() || personaNameIssue(name)) continue;
    found.push_back(PersonaInfo{name});
  }

  std::sort(found.begin(), found.end(), [](const PersonaInfo& a, const PersonaInfo& b) { return localeLess(a.name, b.name); });
  return found;
}

std::optional<std::string> readPersona(const fs::path& dir, const std::string& name) {
  if (personaNameIssue(name)) return std::nullopt;
  // readFileUtf8 只是「把整個檔案的位元組讀進來」，內容不必是 JSON
  return jsonu::readFileUtf8(pathFor(dir, name));
}

CommandResult writePersona(const fs::path& dir, const std::string& name, const std::string& text) {
  if (const auto issue = personaNameIssue(name)) {
    return CommandResult::failure("Cannot save persona: " + *issue);
  }
  // 逐區驗證：description 的 2000 字上限只管 description，台詞區另有寬鬆上限
  //（core/persona_doc.h）。沒有分區的自由格式檔整份就是 description，行為不變。
  if (const auto issue = personaDocIssue(parsePersonaDoc(text))) {
    return CommandResult::failure("Cannot save persona: " + *issue);
  }

  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    return CommandResult::failure("Could not create the personas folder", "Folder: " + dir.u8string());
  }

  const fs::path path = pathFor(dir, name);
  std::ofstream file(path, std::ios::binary | std::ios::trunc);
  if (!file) {
    return CommandResult::failure("Could not write \"" + name + "\"", "File: " + path.u8string());
  }
  file << text;
  if (!file.good()) {
    return CommandResult::failure("Could not write \"" + name + "\"", "File: " + path.u8string());
  }
  return CommandResult::success();
}

PersonaSnapshot loadPersonaSnapshot(const fs::path& dir, const std::string& activeName) {
  PersonaSnapshot snapshot;
  for (const auto& info : scanPersonas(dir)) {
    const auto text = readPersona(dir, info.name);
    // 送到 AI 面前的只有 # Character Description 那一區：台詞與歡迎詞是純本機
    // 素材（氣泡與 TTS），塞進 prompt 只會稀釋描述。沒有分區的檔整份就是描述。
    snapshot.personas.push_back(PersonaEntry{info.name, parsePersonaDoc(text.value_or(std::string())).description});
  }

  const bool stillThere = std::any_of(snapshot.personas.begin(), snapshot.personas.end(), [&activeName](const PersonaEntry& e) { return e.name == activeName; });
  if (stillThere) snapshot.activeName = activeName;
  return snapshot;
}

CommandResult deletePersona(const fs::path& dir, const std::string& name) {
  if (const auto issue = personaNameIssue(name)) {
    return CommandResult::failure("Cannot delete persona: " + *issue);
  }

  const fs::path path = pathFor(dir, name);
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    return CommandResult::failure("No persona named \"" + name + "\"", personaListHint(scanPersonas(dir), dir.u8string()));
  }
  fs::remove(path, ec);
  if (ec) {
    return CommandResult::failure("Could not delete \"" + name + "\"", "File: " + path.u8string());
  }
  return CommandResult::success();
}

}  // namespace l2m
