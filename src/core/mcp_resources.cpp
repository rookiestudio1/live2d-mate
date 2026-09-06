#include "mcp_resources.h"

#include <QByteArray>
#include <QString>
#include <QUrl>

#include <algorithm>

#include "json_doc.h"

namespace l2m {

namespace {

constexpr const char* kMarkdown = "text/markdown";

// 與 model_scanner.cpp 的 encodeUriComponent 同一條規則。
// 角色名可以是中文、可以有空白，直接接在 URI 後面
// 會產生無效的 URI。
std::string encodeUriComponent(const std::string& segment) {
  const QByteArray encoded = QUrl::toPercentEncoding(QString::fromStdString(segment), QByteArrayLiteral("!*'()"));
  return encoded.toStdString();
}

std::string savedUri(const std::string& name) { return std::string(kPersonaSavedPrefix) + encodeUriComponent(name); }

// URI → 角色名；不是 saved 前綴就回 nullopt
std::optional<std::string> nameFromSavedUri(const std::string& uri) {
  const std::string prefix = kPersonaSavedPrefix;
  if (uri.size() <= prefix.size() || uri.compare(0, prefix.size(), prefix) != 0) {
    return std::nullopt;
  }
  const QByteArray raw = QByteArray::fromStdString(uri.substr(prefix.size()));
  return QUrl::fromPercentEncoding(raw).toStdString();
}

void addResource(jsonu::MutDoc& doc, yyjson_mut_val* arr, const std::string& uri, const std::string& name, const std::string& description) {
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* item = yyjson_mut_obj(d);
  yyjson_mut_obj_add_strcpy(d, item, "uri", uri.c_str());
  yyjson_mut_obj_add_strcpy(d, item, "name", name.c_str());
  yyjson_mut_obj_add_strcpy(d, item, "title", name.c_str());
  yyjson_mut_obj_add_strcpy(d, item, "description", description.c_str());
  yyjson_mut_obj_add_str(d, item, "mimeType", kMarkdown);
  yyjson_mut_arr_add_val(arr, item);
}

// resources/read 的回應：單一筆 contents
std::string contentsJson(const std::string& uri, const std::string& name, const std::string& text) {
  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_mut_obj(d);
  doc.setRoot(root);

  yyjson_mut_val* contents = yyjson_mut_arr(d);
  yyjson_mut_val* item = yyjson_mut_obj(d);
  yyjson_mut_obj_add_strcpy(d, item, "uri", uri.c_str());
  yyjson_mut_obj_add_strcpy(d, item, "name", name.c_str());
  yyjson_mut_obj_add_str(d, item, "mimeType", kMarkdown);
  // 角色描述是使用者自由輸入的長文（換行、引號、tab 都會有）。
  // 一律讓 yyjson 逸出，絕不就地拼字串 —— 理由同 core/config_patch.h。
  yyjson_mut_obj_add_strcpy(d, item, "text", text.c_str());
  yyjson_mut_arr_add_val(contents, item);
  yyjson_mut_obj_add_val(d, root, "contents", contents);

  return doc.write(false);
}

}  // namespace

std::string mcpResourcesListJson(const PersonaSnapshot& persona) {
  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_mut_obj(d);
  doc.setRoot(root);

  yyjson_mut_val* resources = yyjson_mut_arr(d);

  const std::string activeName = persona.activeName.empty() ? std::string("(none)") : persona.activeName;
  const std::string activeDescription = persona.activeName.empty() ? std::string("No persona is active right now, so speak freely in your own voice.")
                                                                   : "The personality and speaking style the character is currently using. Everything you "
                                                                     "say through speak and perform should sound like \"" +
                                                                       persona.activeName + "\".";
  addResource(doc, resources, kPersonaActiveUri, activeName, activeDescription);

  for (const auto& entry : persona.personas) {
    const bool isActive = entry.name == persona.activeName;
    addResource(doc, resources, savedUri(entry.name), entry.name, isActive ? "A saved persona. This is the one currently in use." : "A saved persona. Not in use right now.");
  }

  yyjson_mut_obj_add_val(d, root, "resources", resources);
  return doc.write(false);
}

std::optional<std::string> mcpResourceReadJson(const PersonaSnapshot& persona, const std::string& uri) {
  if (uri == kPersonaActiveUri) {
    if (persona.activeName.empty()) {
      return contentsJson(uri, "(none)", "No persona is active right now, so speak freely in your own voice.");
    }
    return contentsJson(uri, persona.activeName, persona.activeText());
  }

  const auto name = nameFromSavedUri(uri);
  if (!name) return std::nullopt;
  const auto entry = std::find_if(persona.personas.begin(), persona.personas.end(), [&name](const PersonaEntry& e) { return e.name == *name; });
  if (entry == persona.personas.end()) return std::nullopt;
  return contentsJson(uri, entry->name, entry->text);
}

}  // namespace l2m
