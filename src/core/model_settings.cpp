#include "model_settings.h"

#include <algorithm>
#include <set>
#include <stdexcept>
#include <vector>

#include "model_assets.h"
#include "string_util.h"

namespace l2m {

namespace fs = std::filesystem;
using strutil::endsWithInsensitive;

namespace {

// 搜尋孤兒檔的最大深度：模型根目錄本身，加上 exp/、motions/ 這類子資料夾
constexpr int kSearchDepth = 2;

struct ExpressionRef {
  std::string name;
  std::string file;
};

// 取檔名（去掉路徑）；JSON 裡的引用一律是 POSIX 斜線，但保險起見兩種都切
std::string baseNameOf(const std::string& path) {
  const size_t pos = path.find_last_of("/\\");
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

// 名稱去掉副檔名；VTS hotkey 沒名字時用檔名頂著
std::string stripSuffix(const std::string& file, const std::string& suffix) {
  const std::string base = baseNameOf(file);
  if (endsWithInsensitive(base, suffix)) return base.substr(0, base.size() - suffix.size());
  return base;
}

// 同名的表情會讓依名稱挑選變成擲骰子，重複時補上序號
std::vector<ExpressionRef> dedupeNames(const std::vector<ExpressionRef>& refs) {
  std::set<std::string> used;
  std::vector<ExpressionRef> result;
  for (const auto& ref : refs) {
    std::string name = ref.name;
    int n = 2;
    while (used.count(name)) name = ref.name + " " + std::to_string(n++);
    used.insert(name);
    result.push_back({name, ref.file});
  }
  return result;
}

// 從 VTS 的 Hotkeys 取出表情綁定 —— 名稱是模型作者寫的，比檔名好懂太多
std::vector<ExpressionRef> expressionsFromVTube(yyjson_val* vtube) {
  std::vector<ExpressionRef> refs;
  yyjson_val* hotkeys = jsonu::get(vtube, "Hotkeys");
  if (!yyjson_is_arr(hotkeys)) return refs;

  size_t idx, max;
  yyjson_val* hk;
  yyjson_arr_foreach(hotkeys, idx, max, hk) {
    const std::string file = jsonu::getString(hk, "File");
    if (file.empty() || !endsWithInsensitive(file, ".exp3.json")) continue;
    const std::string rawName = strutil::trim(jsonu::getString(hk, "Name"));
    const std::string name = rawName.empty() ? stripSuffix(file, ".exp3.json") : rawName;
    refs.push_back({name, file});
  }
  return refs;
}

// VTS 的待機動畫。這是唯一能確定語意的動作，給它 Idle 這個群組名。
std::string idleFromVTube(yyjson_val* vtube) {
  yyjson_val* refs = jsonu::get(vtube, "FileReferences");
  const std::string idle = jsonu::getString(refs, "IdleAnimation");
  return endsWithInsensitive(idle, ".motion3.json") ? idle : "";
}

// 把宣告了但抄不到檔案的路徑接回去：拿檔名到資料夾樹裡找同名檔。
// March 7th 那種「檔案搬進 exp/ 卻沒改 model3.json」就是靠這個救回來。
// 回傳空字串代表不需要（或無法）修復。
std::string repairPath(const ModelAssets& assets, const std::string& file, const std::vector<std::string>& pool) {
  if (assets.exists(file)) return "";
  const std::string target = strutil::toLowerAscii(baseNameOf(file));
  for (const auto& p : pool) {
    if (strutil::toLowerAscii(baseNameOf(p)) == target) return p != file ? p : "";
  }
  return "";
}

yyjson_mut_val* key(jsonu::MutDoc& doc, const char* text) { return yyjson_mut_strcpy(doc.get(), text); }

yyjson_mut_val* str(jsonu::MutDoc& doc, const std::string& text) { return yyjson_mut_strcpy(doc.get(), text.c_str()); }

}  // namespace

std::optional<jsonu::Doc> readVTubeConfig(const ModelAssets& assets) {
  // 只看模型根目錄那一層。list() 已排序，取第一個行為才穩定
  const auto matches = assets.list(".vtube.json", 1);
  if (matches.empty()) return std::nullopt;

  // VTS 設定壞掉不該擋住模型，退回自行掃描
  const auto text = assets.read(matches.front());
  if (!text) return std::nullopt;
  return jsonu::Doc::parse(*text);
}

std::optional<jsonu::Doc> readVTubeConfig(const fs::path& modelDir) { return readVTubeConfig(*openDirectoryAssets(modelDir)); }

EnrichedSettings enrichCubism4Settings(yyjson_val* json, const fs::path& modelDir) { return enrichCubism4Settings(json, *openDirectoryAssets(modelDir)); }

EnrichedSettings enrichCubism4Settings(yyjson_val* json, const ModelAssets& assets) {
  EnrichedSettings result;
  jsonu::MutDoc& doc = result.doc;
  EnrichReport& report = result.report;

  // 深拷貝整份 model3.json，原輸入完全不動
  yyjson_mut_val* root = doc.copyOf(json);
  if (!root || !yyjson_mut_is_obj(root)) {
    root = yyjson_mut_obj(doc.get());
  }
  doc.setRoot(root);

  yyjson_mut_val* refs = yyjson_mut_obj_get(root, "FileReferences");
  if (!refs || !yyjson_mut_is_obj(refs)) {
    refs = yyjson_mut_obj(doc.get());
    yyjson_mut_obj_put(root, key(doc, "FileReferences"), refs);
  }

  const auto expFiles = assets.list(".exp3.json", kSearchDepth);
  const auto motionFiles = assets.list(".motion3.json", kSearchDepth);
  const auto vtube = readVTubeConfig(assets);

  // ── 表情 ──────────────────────────────────────────────
  yyjson_mut_val* declaredExp = yyjson_mut_obj_get(refs, "Expressions");
  const bool hasDeclaredExp = declaredExp && yyjson_mut_is_arr(declaredExp) && yyjson_mut_arr_size(declaredExp) > 0;
  if (hasDeclaredExp) {
    // 作者有宣告：只修斷掉的路徑，名稱與順序都不動
    size_t idx, max;
    yyjson_mut_val* e;
    yyjson_mut_arr_foreach(declaredExp, idx, max, e) {
      if (!yyjson_mut_is_obj(e)) continue;
      yyjson_mut_val* fileVal = yyjson_mut_obj_get(e, "File");
      const char* file = fileVal ? yyjson_mut_get_str(fileVal) : nullptr;
      if (!file) continue;
      const std::string fixed = repairPath(assets, file, expFiles);
      if (fixed.empty()) continue;
      report.repairedPaths++;
      yyjson_mut_obj_put(e, key(doc, "File"), str(doc, fixed));
    }
  } else {
    std::vector<ExpressionRef> fromVTube = vtube ? expressionsFromVTube(vtube->root()) : std::vector<ExpressionRef>{};
    // VTS 的 hotkey 名稱是作者寫的中文，優先採用；沒有才退回檔名
    std::vector<ExpressionRef> picked = fromVTube;
    if (picked.empty()) {
      for (const auto& f : expFiles) picked.push_back({stripSuffix(f, ".exp3.json"), f});
    }
    if (!picked.empty()) {
      // hotkey 記的路徑也可能因為搬檔而失效，一併修
      std::vector<ExpressionRef> resolved;
      for (const auto& e : picked) {
        const std::string fixed = repairPath(assets, e.file, expFiles);
        resolved.push_back({e.name, fixed.empty() ? e.file : fixed});
      }
      std::vector<ExpressionRef> existing;
      for (const auto& e : resolved) {
        if (assets.exists(e.file)) existing.push_back(e);
      }
      const auto deduped = dedupeNames(existing);

      yyjson_mut_val* arr = yyjson_mut_arr(doc.get());
      for (const auto& e : deduped) {
        yyjson_mut_val* obj = yyjson_mut_arr_add_obj(doc.get(), arr);
        yyjson_mut_obj_put(obj, key(doc, "Name"), str(doc, e.name));
        yyjson_mut_obj_put(obj, key(doc, "File"), str(doc, e.file));
      }
      yyjson_mut_obj_put(refs, key(doc, "Expressions"), arr);
      report.addedExpressions = static_cast<int>(deduped.size());
      if (report.addedExpressions > 0) report.source = !fromVTube.empty() ? EnrichReport::Source::VTube : EnrichReport::Source::Scan;
    }
  }

  // ── 動作 ──────────────────────────────────────────────
  yyjson_mut_val* declaredMotions = yyjson_mut_obj_get(refs, "Motions");
  bool hasMotions = false;
  if (declaredMotions && yyjson_mut_is_obj(declaredMotions)) {
    size_t idx, max;
    yyjson_mut_val* k;
    yyjson_mut_val* v;
    yyjson_mut_obj_foreach(declaredMotions, idx, max, k, v) {
      if (yyjson_mut_is_arr(v) && yyjson_mut_arr_size(v) > 0) hasMotions = true;
    }
  }
  if (!hasMotions && !motionFiles.empty()) {
    const std::string idle = vtube ? idleFromVTube(vtube->root()) : "";
    const std::string idleBase = strutil::toLowerAscii(baseNameOf(idle));

    // 群組名 →（保留第一次出現順序的）動作檔清單
    std::vector<std::pair<std::string, std::vector<std::string>>> groups;
    for (const auto& file : motionFiles) {
      // VTS 指定的待機動畫用 Idle 當群組名，互動邏輯與 AI 都認得這個慣例
      const std::string group = (!idle.empty() && strutil::toLowerAscii(baseNameOf(file)) == idleBase) ? "Idle" : stripSuffix(file, ".motion3.json");
      auto it = std::find_if(groups.begin(), groups.end(), [&group](const auto& g) { return g.first == group; });
      if (it == groups.end()) {
        groups.push_back({group, {file}});
      } else {
        it->second.push_back(file);
      }
    }

    yyjson_mut_val* motionsObj = yyjson_mut_obj(doc.get());
    for (const auto& [group, files] : groups) {
      yyjson_mut_val* arr = yyjson_mut_arr(doc.get());
      for (const auto& file : files) {
        yyjson_mut_val* obj = yyjson_mut_arr_add_obj(doc.get(), arr);
        yyjson_mut_obj_put(obj, key(doc, "File"), str(doc, file));
      }
      yyjson_mut_obj_put(motionsObj, str(doc, group), arr);
    }
    yyjson_mut_obj_put(refs, key(doc, "Motions"), motionsObj);
    report.addedMotions = static_cast<int>(motionFiles.size());
    if (report.source == EnrichReport::Source::None)
      report.source = !idle.empty() ? EnrichReport::Source::VTube : EnrichReport::Source::Scan;
    else if (!idle.empty())
      report.source = EnrichReport::Source::VTube;
  }

  // ── Pose ──────────────────────────────────────────────
  // 重新打包的模型常有 pose3.json 躺在資料夾裡卻沒被引用。沒有 Pose 管理的話，
  // 手臂互斥失效，切換手臂的動作一被中斷就會留下「兩雙手同時顯示」的殘影。
  const auto poseFiles = assets.list(".pose3.json", kSearchDepth);
  yyjson_mut_val* poseVal = yyjson_mut_obj_get(refs, "Pose");
  const char* poseStr = poseVal ? yyjson_mut_get_str(poseVal) : nullptr;
  if (poseStr && poseStr[0] != '\0') {
    const std::string fixed = repairPath(assets, poseStr, poseFiles);
    if (!fixed.empty()) {
      yyjson_mut_obj_put(refs, key(doc, "Pose"), str(doc, fixed));
      report.repairedPaths++;
    }
  } else if (!poseFiles.empty()) {
    yyjson_mut_obj_put(refs, key(doc, "Pose"), str(doc, poseFiles.front()));
    report.addedPose = true;
  }

  // 附註：Cubism 2 時代的包（haru 就是）常帶小寫 "layout"（width:5 之類），
  // 這裡刻意**不**把它搬成大寫 "Layout" —— 那些數值是寫給 Cubism 2 的
  // 2 單位邏輯座標系，直接餵給 CubismModelMatrix（haru 的 canvas 是
  // 33.3×62.5 單位）會把構圖推到視野外，實測整個畫面全空。

  return result;
}

bool isCubism4Json(yyjson_val* json) {
  // FileReferences 是物件或陣列都算（只看有沒有這個容器，內容留給補全處理）
  yyjson_val* refs = jsonu::get(json, "FileReferences");
  return refs != nullptr && (yyjson_is_obj(refs) || yyjson_is_arr(refs));
}

EnrichedSettings loadCubism4Settings(const ModelAssets& assets) {
  const auto text = assets.read(assets.entryName());
  const auto doc = text ? jsonu::Doc::parse(*text) : std::nullopt;
  if (!doc) {
    throw std::runtime_error("無法解析 model3.json：" + assets.describe());
  }
  return enrichCubism4Settings(doc->root(), assets);
}

EnrichedSettings loadCubism4Settings(const fs::path& entryPath) {
  // entryPath 可以是 *.zip；openModelAssets 會自己找出 zip 裡的入口檔
  const auto assets = openModelAssets(entryPath);
  if (!assets) {
    throw std::runtime_error("無法開啟模型：" + entryPath.u8string());
  }
  return loadCubism4Settings(*assets);
}

}  // namespace l2m
