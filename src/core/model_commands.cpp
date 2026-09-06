#include "model_commands.h"

#include <yyjson.h>

#include <algorithm>
#include <set>

#include "json_doc.h"
#include "string_util.h"

namespace l2m {

namespace {

// hint 裡最多列幾個參數。上百個全列出來 AI 讀不完，也會把工具輸出撐爆。
constexpr size_t kParameterHintLimit = 8;

std::string joinWith(const std::vector<std::string>& parts, const char* sep) {
  std::string out;
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i > 0) out += sep;
    out += parts[i];
  }
  return out;
}

std::string normalizeKey(const std::string& raw) { return strutil::toLowerAscii(strutil::trim(raw)); }

// 群組名可以是空字串（模型把動作放在無名群組），顯示時要有替代字樣
std::string groupLabel(const std::string& name) { return name.empty() ? "(unnamed group)" : name; }

// 把使用者命名（key → meaning）攤成 `key="meaning"` 的清單
std::string namedList(const std::map<std::string, std::string>& meanings, bool labelEmptyKey) {
  std::vector<std::string> parts;
  parts.reserve(meanings.size());
  for (const auto& entry : meanings) {
    const std::string shown = (labelEmptyKey && entry.first.empty()) ? groupLabel(entry.first) : entry.first;
    parts.push_back(shown + "=\"" + entry.second + "\"");
  }
  return joinWith(parts, ", ");
}

const ParameterInfo* findParameter(const ModelInfo& model, const std::string& id) {
  for (const auto& p : model.parameters) {
    if (p.id == id) return &p;
  }
  return nullptr;
}

void addStr(yyjson_mut_doc* doc, yyjson_mut_val* obj, const char* key, const std::string& value) { yyjson_mut_obj_add_strcpy(doc, obj, key, value.c_str()); }

bool contains(const std::vector<std::string>& list, const std::string& value) { return std::find(list.begin(), list.end(), value) != list.end(); }

CommandResult rejectDriven(const std::vector<std::string>& driven, const char* verb, const char* pickHint) {
  return CommandResult::failure("Parameter(s) " + joinWith(driven, ", ") + " are driven by the model's physics simulation, so " + verb + " them has no effect",
                                std::string("Physics overwrites these every frame. ") + pickHint);
}

}  // namespace

const char* roleName(ParameterRole role) {
  switch (role) {
    case ParameterRole::PhysicsInput:
      return "physics-input";
    case ParameterRole::PhysicsOutput:
      return "physics-output";
    case ParameterRole::Free:
    default:
      return "free";
  }
}

// ── 模型 ────────────────────────────────────────────────

const ModelInfo* resolveModel(const std::vector<ModelInfo>& models, const std::string& idOrName) {
  const std::string key = normalizeKey(idOrName);
  for (const auto& m : models) {
    if (strutil::toLowerAscii(m.id) == key) return &m;
  }
  for (const auto& m : models) {
    if (strutil::toLowerAscii(m.name) == key) return &m;
  }
  for (const auto& m : models) {
    if (!key.empty() && strutil::toLowerAscii(m.name).find(key) != std::string::npos) return &m;
  }
  return nullptr;
}

std::string modelListHint(const std::vector<ModelInfo>& models, const std::string& modelsDir) {
  if (models.empty()) return "Models folder is empty: " + modelsDir;
  std::vector<std::string> names;
  names.reserve(models.size());
  for (const auto& m : models) names.push_back(m.name);
  return "Available models: " + joinWith(names, ", ");
}

// ── 使用者命名 ──────────────────────────────────────────

std::optional<std::string> matchByMeaning(const std::map<std::string, std::string>& meanings, const std::string& query) {
  const std::string key = normalizeKey(query);
  if (key.empty()) return std::nullopt;

  for (const auto& entry : meanings) {
    if (normalizeKey(entry.second) == key) return entry.first;
  }
  for (const auto& entry : meanings) {
    const std::string value = normalizeKey(entry.second);
    if (value.empty()) continue;
    if (value.find(key) != std::string::npos || key.find(value) != std::string::npos) {
      return entry.first;
    }
  }
  return std::nullopt;
}

// ── 提示文字 ────────────────────────────────────────────

std::string motionHint(const ModelInfo& model) {
  if (model.motions.empty()) return "This model defines no motion groups";

  std::vector<std::string> listed;
  listed.reserve(model.motions.size());
  for (const auto& g : model.motions) {
    listed.push_back(groupLabel(g.name) + "(" + std::to_string(g.count) + ")");
  }
  const std::string named = namedList(model.annotations.motions, true);
  const std::string base = "Available motion groups: " + joinWith(listed, ", ");
  if (named.empty()) return base;
  return base + ". Named motions (callable by meaning): " + named;
}

std::string expressionHint(const ModelInfo& model) {
  if (model.expressions.empty()) return "This model defines no expressions";

  const std::string named = namedList(model.annotations.expressions, false);
  const std::string base = "Available expressions: " + joinWith(model.expressions, ", ");
  if (named.empty()) return base;
  return base + ". Named expressions (callable by meaning): " + named;
}

std::string parameterHint(const ModelInfo& model, const std::vector<std::string>& wanted) {
  const auto& params = model.parameters;
  if (params.empty()) {
    return "This model exposes no parameters. "
           "Call list_motions or list_expressions instead.";
  }

  std::vector<std::string> keys;
  keys.reserve(wanted.size());
  for (const auto& w : wanted) keys.push_back(strutil::toLowerAscii(w));

  std::vector<const ParameterInfo*> near;
  for (const auto& p : params) {
    if (near.size() >= kParameterHintLimit) break;
    const std::string id = strutil::toLowerAscii(p.id);
    const bool related = std::any_of(keys.begin(), keys.end(), [&id](const std::string& k) { return id.find(k) != std::string::npos || (!id.empty() && k.find(id) != std::string::npos); });
    if (related) near.push_back(&p);
  }
  if (near.empty()) {
    for (const auto& p : params) {
      if (near.size() >= kParameterHintLimit) break;
      near.push_back(&p);
    }
  }

  std::vector<std::string> shown;
  shown.reserve(near.size());
  for (const auto* p : near) shown.push_back(p->id + "(\"" + p->name + "\")");

  return "Closest parameters: " + joinWith(shown, ", ") + ". Call list_parameters for the full list of " + std::to_string(params.size()) + ".";
}

// ── 解析 ────────────────────────────────────────────────

CommandResult resolveMotion(const ModelInfo& model, const std::string& group, std::optional<int> index, ResolvedMotion* out) {
  const std::string key = normalizeKey(group);
  const MotionGroupInfo* match = nullptr;

  for (const auto& g : model.motions) {
    if (strutil::toLowerAscii(g.name) == key) {
      match = &g;
      break;
    }
  }
  if (!match) {
    for (const auto& g : model.motions) {
      if (strutil::startsWithInsensitive(g.name, key)) {
        match = &g;
        break;
      }
    }
  }
  if (!match) {
    // 名稱對不上時改用使用者寫的意義；命名可能精確到某一個索引
    const auto namedKey = matchByMeaning(model.annotations.motions, group);
    if (namedKey.has_value()) {
      const MotionKeyParts parsed = parseMotionKey(*namedKey);
      for (const auto& g : model.motions) {
        if (g.name == parsed.group) {
          match = &g;
          break;
        }
      }
      if (match && !index.has_value() && parsed.index >= 0) index = parsed.index;
    }
  }

  if (!match) {
    return CommandResult::failure("Model \"" + model.name + "\" has no motion \"" + group + "\"", motionHint(model));
  }
  if (index.has_value() && (*index < 0 || *index >= match->count)) {
    return CommandResult::failure("Motion index " + std::to_string(*index) + " is out of range",
                                  "Group \"" + match->name + "\" has " + std::to_string(match->count) + " motions (0 ~ " + std::to_string(match->count - 1) + ")");
  }

  if (out) {
    out->group = match->name;
    out->index = index;
  }
  return CommandResult::success();
}

CommandResult resolveExpression(const ModelInfo& model, const std::string& name, std::string* out) {
  const auto& list = model.expressions;
  const std::string key = normalizeKey(name);

  std::optional<std::string> match;
  for (const auto& e : list) {
    if (strutil::toLowerAscii(e) == key) {
      match = e;
      break;
    }
  }
  if (!match) {
    for (const auto& e : list) {
      if (strutil::startsWithInsensitive(e, key)) {
        match = e;
        break;
      }
    }
  }
  if (!match) match = matchByMeaning(model.annotations.expressions, name);

  const bool known = match.has_value() && std::find(list.begin(), list.end(), *match) != list.end();
  if (!known) {
    return CommandResult::failure("Model \"" + model.name + "\" has no expression \"" + name + "\"", expressionHint(model));
  }

  if (out) *out = *match;
  return CommandResult::success();
}

std::vector<SetParameterRequest> virtualExpressionParams(const ModelInfo& model, const std::optional<std::string>& name) {
  std::vector<SetParameterRequest> params;
  params.reserve(model.paramExpressions.size());
  for (const auto& e : model.paramExpressions) {
    SetParameterRequest req;
    req.id = e.param;
    req.value = (name.has_value() && e.name == *name) ? e.value : 0;
    req.durationMs = kExpressionFadeMs;
    params.push_back(std::move(req));
  }
  return params;
}

// ── 驗證 ────────────────────────────────────────────────

CommandResult validateSetParameters(const ModelInfo& model, const std::vector<SetParameterRequest>& requests) {
  if (requests.empty()) return CommandResult::failure("No parameters given");

  std::vector<std::string> wanted;
  wanted.reserve(requests.size());
  for (const auto& r : requests) wanted.push_back(r.id);

  if (model.parameters.empty()) {
    return CommandResult::failure("Model \"" + model.name + "\" exposes no parameters", parameterHint(model, wanted));
  }

  std::vector<std::string> unknown;
  for (const auto& r : requests) {
    if (!findParameter(model, r.id)) unknown.push_back(r.id);
  }
  if (!unknown.empty()) {
    return CommandResult::failure("Unknown parameter(s): " + joinWith(unknown, ", "), parameterHint(model, unknown));
  }

  std::vector<std::string> driven;
  for (const auto& r : requests) {
    const ParameterInfo* info = findParameter(model, r.id);
    if (info && info->role == ParameterRole::PhysicsOutput) driven.push_back(r.id);
  }
  if (!driven.empty()) {
    return rejectDriven(driven, "writing", "Pick a parameter whose role is \"free\" or \"physics-input\" from list_parameters.");
  }

  return CommandResult::success();
}

CommandResult validateAnimateParams(const ModelInfo& model, const std::vector<std::string>& used) {
  // 沒有參數表的模型（缺 cdi3.json）不擋，交給 Cubism 自己吞掉不存在的 id
  if (model.parameters.empty()) return CommandResult::success();

  std::vector<std::string> unknown;
  for (const auto& id : used) {
    if (!findParameter(model, id)) unknown.push_back(id);
  }
  if (!unknown.empty()) {
    return CommandResult::failure("Unknown parameter(s): " + joinWith(unknown, ", "), parameterHint(model, unknown));
  }

  std::vector<std::string> driven;
  for (const auto& id : used) {
    const ParameterInfo* info = findParameter(model, id);
    if (info && info->role == ParameterRole::PhysicsOutput) driven.push_back(id);
  }
  if (!driven.empty()) {
    return rejectDriven(driven, "animating", "Pick parameters whose role is \"free\" or \"physics-input\".");
  }

  return CommandResult::success();
}

std::vector<std::string> uniqueKeyframeParams(const std::vector<std::map<std::string, double>>& frameParams) {
  std::vector<std::string> out;
  std::set<std::string> seen;
  for (const auto& frame : frameParams) {
    for (const auto& entry : frame) {
      if (seen.insert(entry.first).second) out.push_back(entry.first);
    }
  }
  return out;
}

// ── 報告與清單 ──────────────────────────────────────────

std::string buildParameterReportJson(const ModelInfo& model, bool supported, const std::vector<ParameterSnapshot>& snapshots, const std::vector<std::string>& overridden) {
  std::map<std::string, const ParameterSnapshot*> byId;
  for (const auto& s : snapshots) byId[s.id] = &s;

  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_mut_obj(d);
  doc.setRoot(root);

  addStr(d, root, "model", model.name);
  yyjson_mut_obj_add_bool(d, root, "supported", supported);

  yyjson_mut_val* over = yyjson_mut_arr(d);
  for (const auto& id : overridden) yyjson_mut_arr_add_strcpy(d, over, id.c_str());
  yyjson_mut_obj_add_val(d, root, "overridden", over);

  yyjson_mut_val* arr = yyjson_mut_arr(d);
  for (const auto& info : model.parameters) {
    yyjson_mut_val* item = yyjson_mut_obj(d);
    addStr(d, item, "id", info.id);
    addStr(d, item, "name", info.name);
    if (!info.group.empty()) addStr(d, item, "group", info.group);
    yyjson_mut_obj_add_str(d, item, "role", roleName(info.role));

    // 執行期沒回報就別補 0，那會讓 AI 以為讀到了真的現值
    const auto it = byId.find(info.id);
    if (it != byId.end()) {
      yyjson_mut_obj_add_real(d, item, "value", it->second->value);
      yyjson_mut_obj_add_real(d, item, "min", it->second->min);
      yyjson_mut_obj_add_real(d, item, "max", it->second->max);
      yyjson_mut_obj_add_real(d, item, "default", it->second->defaultValue);
    }
    yyjson_mut_arr_add_val(arr, item);
  }
  yyjson_mut_obj_add_val(d, root, "parameters", arr);

  return doc.write(true);
}

// 一份「目錄」：每個模型的動作群組壓成 "name(count)" 一行、表情只留名字，
// 目的只是讓 AI 一眼挑得出要切到哪一個。細節（群組內每個索引的檔名、使用者寫的意義、
// 哪些是合成的內建項目）留給 list_motions／list_expressions —— 那兩支是對著
// 目前模型的，攤在這裡會讓每多一個模型就多幾十行。
// list_models 與 get_state 的 "models" 欄位共用這一支。
std::string modelListJson(const std::vector<ModelInfo>& models, const std::string& currentModelId) {
  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_mut_obj(d);
  doc.setRoot(root);

  const ModelInfo* current = nullptr;
  for (const auto& m : models) {
    if (m.id == currentModelId) current = &m;
  }
  if (current) {
    addStr(d, root, "current", current->name);
  } else {
    yyjson_mut_obj_add_null(d, root, "current");
  }

  yyjson_mut_val* arr = yyjson_mut_arr(d);
  for (const auto& m : models) {
    yyjson_mut_val* item = yyjson_mut_obj(d);
    addStr(d, item, "name", m.name);
    addStr(d, item, "id", m.id);

    yyjson_mut_val* groups = yyjson_mut_arr(d);
    for (const auto& g : m.motions) {
      const std::string label = g.name + "(" + std::to_string(g.count) + ")";
      yyjson_mut_arr_add_strcpy(d, groups, label.c_str());
    }
    yyjson_mut_obj_add_val(d, item, "motionGroups", groups);

    yyjson_mut_val* exps = yyjson_mut_arr(d);
    for (const auto& e : m.expressions) yyjson_mut_arr_add_strcpy(d, exps, e.c_str());
    yyjson_mut_obj_add_val(d, item, "expressions", exps);

    yyjson_mut_obj_add_int(d, item, "namedMotions", static_cast<int64_t>(m.annotations.motions.size()));
    yyjson_mut_obj_add_int(d, item, "namedExpressions", static_cast<int64_t>(m.annotations.expressions.size()));
    yyjson_mut_arr_add_val(arr, item);
  }
  yyjson_mut_obj_add_val(d, root, "models", arr);

  return doc.write(true);
}

std::string motionListJson(const ModelInfo& model) {
  const auto& named = model.annotations.motions;

  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_mut_obj(d);
  doc.setRoot(root);
  addStr(d, root, "model", model.name);

  yyjson_mut_val* arr = yyjson_mut_arr(d);
  for (const auto& group : model.motions) {
    yyjson_mut_val* item = yyjson_mut_obj(d);
    addStr(d, item, "group", group.name);
    yyjson_mut_obj_add_int(d, item, "count", group.count);

    const auto meaning = named.find(motionKey(group.name));
    if (meaning != named.end() && !meaning->second.empty()) {
      addStr(d, item, "meaning", meaning->second);
    }
    // 合成出來的內建動作要標出來：AI 得知道這一段不是模型作者做的
    if (contains(model.builtinMotions, group.name)) {
      yyjson_mut_obj_add_bool(d, item, "builtin", true);
    }

    // 同一個群組常常塞了好幾段完全不同的動作，逐一列出才有辦法挑
    if (group.count > 1) {
      yyjson_mut_val* motions = yyjson_mut_arr(d);
      for (int i = 0; i < group.count; ++i) {
        yyjson_mut_val* entry = yyjson_mut_obj(d);
        yyjson_mut_obj_add_int(d, entry, "index", i);
        if (i < static_cast<int>(group.files.size()) && !group.files[i].empty()) {
          addStr(d, entry, "file", group.files[i]);
        }
        const auto sub = named.find(motionKey(group.name, i));
        if (sub != named.end() && !sub->second.empty()) addStr(d, entry, "meaning", sub->second);
        yyjson_mut_arr_add_val(motions, entry);
      }
      yyjson_mut_obj_add_val(d, item, "motions", motions);
    }
    yyjson_mut_arr_add_val(arr, item);
  }
  yyjson_mut_obj_add_val(d, root, "groups", arr);

  return doc.write(true);
}

std::string expressionListJson(const ModelInfo& model) {
  const auto& named = model.annotations.expressions;

  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_mut_obj(d);
  doc.setRoot(root);
  addStr(d, root, "model", model.name);

  yyjson_mut_val* arr = yyjson_mut_arr(d);
  for (const auto& name : model.expressions) {
    yyjson_mut_val* item = yyjson_mut_obj(d);
    addStr(d, item, "name", name);
    const auto meaning = named.find(name);
    if (meaning != named.end() && !meaning->second.empty()) {
      addStr(d, item, "meaning", meaning->second);
    }
    if (contains(model.builtinExpressions, name)) {
      yyjson_mut_obj_add_bool(d, item, "builtin", true);
    }
    yyjson_mut_arr_add_val(arr, item);
  }
  yyjson_mut_obj_add_val(d, root, "expressions", arr);

  return doc.write(true);
}

}  // namespace l2m
