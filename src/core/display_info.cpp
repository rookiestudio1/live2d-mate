#include "display_info.h"

#include "json_doc.h"
#include "model_assets.h"
#include "string_util.h"

namespace l2m {

namespace {

using jsonu::Doc;

// 名稱裡帶「物理」的參數是給物理引擎用的補正值，不是給人挑的。
// 中日英一起認：日文模型寫「物理」，英文模型寫 physics。
bool isPhysicsName(const std::string& text) { return text.find("物理") != std::string::npos || strutil::containsInsensitive(text, "physics"); }

// 判定一個參數群組是不是「表情」群組。作者自己的分類比我們猜得準。
// （「表情差分」包含「表情」，不必另列）
bool isExpressionGroup(const std::string& text) {
  return text.find("表情") != std::string::npos || text.find("顔") != std::string::npos || strutil::containsInsensitive(text, "expression") || strutil::containsInsensitive(text, "emote") ||
         strutil::containsInsensitive(text, "face");
}

// 參數 id 以 expression 開頭是 VTube Studio 模型的通用慣例，沒分組也認
bool isExpressionId(const std::string& id) { return strutil::startsWithInsensitive(id, "expression"); }

// 讀 model3.json 的 FileReferences.<key> 指到的 JSON 檔；
// 缺檔或格式壞掉都不該擋住模型載入，當作沒有這份資訊
std::optional<Doc> readReferencedJson(const ModelAssets& assets, yyjson_val* modelJson, const char* key) {
  yyjson_val* refs = jsonu::get(modelJson, "FileReferences");
  const std::string file = jsonu::getString(refs, key);
  if (file.empty()) return std::nullopt;
  const auto text = assets.read(file);
  if (!text) return std::nullopt;
  return Doc::parse(*text);
}

}  // namespace

PhysicsIO readPhysicsIO(const std::filesystem::path& modelDir, yyjson_val* modelJson) { return readPhysicsIO(*openDirectoryAssets(modelDir), modelJson); }

PhysicsIO readPhysicsIO(const ModelAssets& assets, yyjson_val* modelJson) {
  PhysicsIO io;
  const auto physics = readReferencedJson(assets, modelJson, "Physics");
  if (!physics) return io;

  yyjson_val* settings = jsonu::get(physics->root(), "PhysicsSettings");
  if (!yyjson_is_arr(settings)) return io;

  size_t idx, max;
  yyjson_val* setting;
  yyjson_arr_foreach(settings, idx, max, setting) {
    yyjson_val* inputs = jsonu::get(setting, "Input");
    if (yyjson_is_arr(inputs)) {
      size_t i, m;
      yyjson_val* input;
      yyjson_arr_foreach(inputs, i, m, input) {
        const std::string id = jsonu::getString(jsonu::get(input, "Source"), "Id");
        if (!id.empty()) io.inputs.insert(id);
      }
    }
    yyjson_val* outputs = jsonu::get(setting, "Output");
    if (yyjson_is_arr(outputs)) {
      size_t i, m;
      yyjson_val* output;
      yyjson_arr_foreach(outputs, i, m, output) {
        const std::string id = jsonu::getString(jsonu::get(output, "Destination"), "Id");
        if (!id.empty()) io.outputs.insert(id);
      }
    }
  }
  return io;
}

std::vector<ParameterInfo> describeParameters(const std::filesystem::path& modelDir, yyjson_val* modelJson) { return describeParameters(*openDirectoryAssets(modelDir), modelJson); }

std::vector<ParameterInfo> describeParameters(const ModelAssets& assets, yyjson_val* modelJson) {
  const auto cdi = readReferencedJson(assets, modelJson, "DisplayInfo");
  if (!cdi) return {};
  yyjson_val* parameters = jsonu::get(cdi->root(), "Parameters");
  if (!yyjson_is_arr(parameters)) return {};

  // 群組 id → 群組名稱，用來判斷參數是不是被作者歸在「表情」底下
  std::map<std::string, std::string> groupNames;
  yyjson_val* groups = jsonu::get(cdi->root(), "ParameterGroups");
  if (yyjson_is_arr(groups)) {
    size_t idx, max;
    yyjson_val* group;
    yyjson_arr_foreach(groups, idx, max, group) {
      const std::string id = jsonu::getString(group, "Id");
      if (!id.empty()) groupNames[id] = jsonu::getString(group, "Name");
    }
  }

  const PhysicsIO io = readPhysicsIO(assets, modelJson);

  std::vector<ParameterInfo> params;
  size_t idx, max;
  yyjson_val* entry;
  yyjson_arr_foreach(parameters, idx, max, entry) {
    const std::string id = jsonu::getString(entry, "Id");
    if (id.empty()) continue;

    const std::string rawName = strutil::trim(jsonu::getString(entry, "Name"));
    const std::string name = rawName.empty() ? id : rawName;
    const std::string groupId = jsonu::getString(entry, "GroupId");

    // 輸出優先：同時是輸入與輸出的參數寫下去照樣會被蓋掉，先講最重要的限制
    const ParameterRole role = io.outputs.count(id) ? ParameterRole::PhysicsOutput : io.inputs.count(id) ? ParameterRole::PhysicsInput : ParameterRole::Free;

    const auto groupIt = groupNames.find(groupId);
    params.push_back({id, name, groupIt != groupNames.end() ? groupIt->second : "", role});
  }
  return params;
}

std::vector<ParamExpressionRef> expressionsFromDisplayInfo(const std::vector<ParameterInfo>& params) {
  std::vector<ParamExpressionRef> found;
  for (const auto& param : params) {
    // 作者沒取名的參數（Name 就是 Id）對 AI 毫無意義
    if (param.name == param.id) continue;
    // 寫了會被物理蓋掉的參數當不了表情
    if (param.role == ParameterRole::PhysicsOutput) continue;
    if (isPhysicsName(param.name) || isPhysicsName(param.group)) continue;
    if (!isExpressionGroup(param.group) && !isExpressionId(param.id)) continue;

    found.push_back({param.name, param.id, 1});
  }
  return found;
}

}  // namespace l2m
