#include "annotations.h"

#include <QDebug>

#include <algorithm>
#include <fstream>
#include <optional>

#include "json_doc.h"
#include "model_assets.h"
#include "string_util.h"

namespace l2m {

namespace fs = std::filesystem;

namespace {

// 驗證單一模型的命名物件：
// 根必須是物件；motions / expressions 缺席時補空，存在時必須是「字串 → 字串」的物件。
std::optional<ModelAnnotations> validateAnnotations(yyjson_val* root) {
  if (!root || !yyjson_is_obj(root)) return std::nullopt;

  ModelAnnotations result;
  const auto readBucket = [](yyjson_val* bucket, std::map<std::string, std::string>& out) -> bool {
    if (!bucket) return true;  // 缺席 → 預設空
    if (!yyjson_is_obj(bucket)) return false;
    size_t idx, max;
    yyjson_val* k;
    yyjson_val* v;
    yyjson_obj_foreach(bucket, idx, max, k, v) {
      const char* value = yyjson_get_str(v);
      if (!value) return false;
      out[yyjson_get_str(k)] = value;
    }
    return true;
  };

  if (!readBucket(jsonu::get(root, "motions"), result.motions)) return std::nullopt;
  if (!readBucket(jsonu::get(root, "expressions"), result.expressions)) return std::nullopt;
  return result;
}

// 命名檔序列化：兩層物件、2 空白縮排、結尾換行
std::string serializeAnnotations(const ModelAnnotations& data) {
  jsonu::MutDoc doc;
  yyjson_mut_val* root = yyjson_mut_obj(doc.get());
  doc.setRoot(root);

  const auto writeBucket = [&doc, root](const char* name, const std::map<std::string, std::string>& bucket) {
    yyjson_mut_val* obj = yyjson_mut_obj(doc.get());
    for (const auto& [key, value] : bucket) {
      yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), key.c_str()), yyjson_mut_strcpy(doc.get(), value.c_str()));
    }
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), name), obj);
  };
  writeBucket("motions", data.motions);
  writeBucket("expressions", data.expressions);
  return doc.write(true) + "\n";
}

bool writeTextFile(const fs::path& path, const std::string& text) {
  std::ofstream file(path, std::ios::binary);
  if (!file) return false;
  file << text;
  return file.good();
}

// 把 ".json" 結尾換成 ".bak.json"
fs::path bakPathFor(const fs::path& path) {
  std::string name = path.filename().u8string();
  const std::string suffix = ".json";
  if (strutil::endsWithInsensitive(name, suffix)) name = name.substr(0, name.size() - suffix.size());
  return path.parent_path() / fs::u8path(name + ".bak.json");
}

void backup(const fs::path& path, const std::string& reason) {
  const fs::path bak = bakPathFor(path);
  std::error_code ec;
  fs::rename(path, bak, ec);
  if (!ec) {
    qWarning() << "[annotations]" << reason.c_str() << "；已備份至" << QString::fromStdString(bak.u8string());
  } else {
    qWarning() << "[annotations]" << reason.c_str() << "；備份失敗，將以空命名繼續";
  }
}

}  // namespace

std::string motionKey(const std::string& group, int index) { return group + "#" + std::to_string(index); }

std::string motionKey(const std::string& group) { return group; }

MotionKeyParts parseMotionKey(const std::string& key) {
  const size_t at = key.rfind('#');
  if (at == std::string::npos) return {key, -1};

  // 全數字（含空字串算 0）才是合法索引，其餘整串當群組名
  const std::string tail = key.substr(at + 1);
  for (char c : tail) {
    if (c < '0' || c > '9') return {key, -1};
  }
  const int index = tail.empty() ? 0 : std::stoi(tail);
  return {key.substr(0, at), index};
}

// 入口檔名 → 命名檔名（純字串，不碰 std::filesystem）。
// 刻意不走 fs::path：zip 內的條目名稱理論上可以是任何位元組，
// MSVC 的 u8path 遇到非法 UTF-8 會**丟例外**，而這支函式的呼叫端（掃描迴圈）
// 沒有理由為了組一個檔名而準備接例外。
std::string annotationsNameFor(const std::string& entryName) {
  // 去掉 .model3.json / .json / .zip 結尾（不分大小寫）。
  // zip 模型的入口就是壓縮檔本身，所以 Foo.zip 會得到 Foo.annotations.json。
  std::string base = entryName;
  for (const char* suffix : {".model3.json", ".json", ".zip"}) {
    if (strutil::endsWithInsensitive(base, suffix)) {
      base = base.substr(0, base.size() - std::string(suffix).size());
      break;
    }
  }
  return base + ".annotations.json";
}

fs::path annotationsPathFor(const fs::path& modelEntryPath) { return modelEntryPath.parent_path() / fs::u8path(annotationsNameFor(modelEntryPath.filename().u8string())); }

ModelAnnotations readAnnotations(const fs::path& modelEntryPath) {
  const fs::path path = annotationsPathFor(modelEntryPath);
  std::error_code ec;
  if (!fs::exists(path, ec)) return {};

  const auto text = jsonu::readFileUtf8(path);
  const auto doc = text ? jsonu::Doc::parse(*text) : std::nullopt;
  if (!doc) {
    backup(path, "JSON 解析失敗");
    return {};
  }

  auto parsed = validateAnnotations(doc->root());
  if (!parsed) {
    backup(path, "欄位驗證失敗");
    return {};
  }
  return *parsed;
}

ModelAnnotations readAnnotations(const fs::path& modelEntryPath, const ModelAssets& assets) {
  // sidecar 優先：使用者自己改的命名一律落在容器外面，蓋掉作者打包進去的那一份
  std::error_code ec;
  if (fs::exists(annotationsPathFor(modelEntryPath), ec)) {
    return readAnnotations(modelEntryPath);
  }

  // 容器內建的那一份。壞掉不備份也不報錯 —— zip 是唯讀的，備份無處可寫，
  // 而且使用者只要重新命名一次就會有 sidecar 蓋過去。
  const auto inner = assets.read(annotationsNameFor(assets.entryName()));
  if (!inner) return {};
  const auto doc = jsonu::Doc::parse(*inner);
  if (!doc) return {};
  auto parsed = validateAnnotations(doc->root());
  return parsed ? *parsed : ModelAnnotations{};
}

void writeAnnotations(const fs::path& modelEntryPath, const ModelAnnotations& data) {
  const fs::path path = annotationsPathFor(modelEntryPath);

  if (data.empty()) {
    std::error_code ec;
    if (fs::exists(path, ec)) {
      fs::rename(path, path.parent_path() / fs::u8path(path.filename().u8string() + ".removed"), ec);
      if (ec) writeTextFile(path, serializeAnnotations({}));
    }
    return;
  }

  writeTextFile(path, serializeAnnotations(data));
}

ModelAnnotations applyMeaning(const ModelAnnotations& current, AnnotationKind kind, const std::string& key, const std::string& meaning) {
  ModelAnnotations next = current;
  auto& bucket = kind == AnnotationKind::Motions ? next.motions : next.expressions;
  const std::string trimmed = strutil::trim(meaning);
  if (!trimmed.empty())
    bucket[key] = trimmed;
  else
    bucket.erase(key);
  return next;
}

ModelAnnotations fillDefaultNames(const ModelInfo& model, const ModelAnnotations& current) {
  ModelAnnotations next = current;

  // 只補空缺：已有非空命名的鍵不動（理由見標頭註解）
  const auto fillIfEmpty = [](std::map<std::string, std::string>& bucket, const std::string& key, const std::string& name) {
    if (name.empty()) return;
    const auto it = bucket.find(key);
    if (it != bucket.end() && !it->second.empty()) return;
    bucket[key] = name;
  };

  // 內建動作／表情跳過：名稱本身（wave、smile…）就已經是意義，
  // 拿它去填自己的意義只是噪音，還會讓使用者以為那一列已經命名過了
  const auto isBuiltin = [](const std::vector<std::string>& list, const std::string& name) { return std::find(list.begin(), list.end(), name) != list.end(); };

  for (const auto& group : model.motions) {
    if (isBuiltin(model.builtinMotions, group.name)) continue;
    fillIfEmpty(next.motions, motionKey(group.name), group.name);

    // 對齊命名區的列：單一動作的群組畫面上沒有 #索引 那幾列
    if (group.count <= 1) continue;
    for (int index = 0; index < group.count; ++index) {
      std::string name = index < static_cast<int>(group.files.size()) ? group.files[index] : std::string();
      const std::string suffix = ".motion3.json";
      if (strutil::endsWithInsensitive(name, suffix)) name = name.substr(0, name.size() - suffix.size());
      fillIfEmpty(next.motions, motionKey(group.name, index), name);
    }
  }

  for (const auto& name : model.expressions) {
    if (isBuiltin(model.builtinExpressions, name)) continue;
    fillIfEmpty(next.expressions, name, name);
  }
  return next;
}

int migrateLegacyAnnotations(const fs::path& legacyPath, const fs::path& modelsDir, const std::vector<std::string>& knownModelIds) {
  std::error_code ec;
  if (!fs::exists(legacyPath, ec)) return 0;

  const auto doc = jsonu::Doc::parseFile(legacyPath);
  if (!doc || !yyjson_is_obj(doc->root())) {
    qWarning() << "[annotations] 舊版命名檔無法解析，略過搬移";
    return 0;
  }

  // 任何一個模型的命名不合法，整份就不搬
  std::vector<std::pair<std::string, ModelAnnotations>> entries;
  {
    size_t idx, max;
    yyjson_val* k;
    yyjson_val* v;
    yyjson_obj_foreach(doc->root(), idx, max, k, v) {
      auto parsed = validateAnnotations(v);
      if (!parsed) return 0;
      entries.emplace_back(yyjson_get_str(k), std::move(*parsed));
    }
  }

  int migrated = 0;
  for (const auto& [modelId, annotations] : entries) {
    if (std::find(knownModelIds.begin(), knownModelIds.end(), modelId) == knownModelIds.end()) {
      qWarning() << "[annotations] 舊版命名對應到不存在的模型，略過:" << QString::fromStdString(modelId);
      continue;
    }
    const fs::path entryPath = modelsDir / fs::u8path(modelId);
    // 模型資料夾已經有命名檔就以它為準，不覆蓋
    if (fs::exists(annotationsPathFor(entryPath), ec)) continue;
    writeAnnotations(entryPath, annotations);
    migrated++;
  }

  fs::rename(legacyPath, legacyPath.parent_path() / fs::u8path([&legacyPath] {
                           std::string name = legacyPath.filename().u8string();
                           const std::string suffix = ".json";
                           if (strutil::endsWithInsensitive(name, suffix)) name = name.substr(0, name.size() - suffix.size());
                           return name + ".migrated.json";
                         }()),
             ec);
  // 改名失敗不影響已經搬好的資料

  if (migrated > 0) qInfo() << "[annotations] 已把" << migrated << "個模型的命名搬進各自的資料夾";
  return migrated;
}

}  // namespace l2m
