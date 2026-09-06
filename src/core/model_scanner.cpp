#include "model_scanner.h"

#include <QDebug>
#include <QString>
#include <QUrl>

#include <algorithm>
#include <memory>
#include <set>

#include "annotations.h"
#include "builtin_actions.h"
#include "display_info.h"
#include "json_doc.h"
#include "model_assets.h"
#include "model_settings.h"
#include "string_util.h"

namespace l2m {

namespace fs = std::filesystem;
using strutil::endsWithInsensitive;
using strutil::equalsInsensitive;

namespace {

constexpr int kMaxDepth = 3;

const std::set<std::string>& ignoredDirs() {
  static const std::set<std::string> dirs{"node_modules", ".git", "__MACOSX"};
  return dirs;
}

// 先做大小寫不敏感的比較，相同時再用原字串分勝負。
// （不用 ICU 的地區排序：這裡只需要「穩定且兩處一致」。）
bool localeLess(const std::string& a, const std::string& b) {
  const std::string la = strutil::toLowerAscii(a);
  const std::string lb = strutil::toLowerAscii(b);
  if (la != lb) return la < lb;
  return a < b;
}

// URI 片段的百分比編碼，保留字元 A-Za-z0-9 - _ . ! ~ * ' ( )（即 encodeURIComponent 的規則）
std::string encodeUriComponent(const std::string& segment) {
  const QByteArray encoded = QUrl::toPercentEncoding(QString::fromStdString(segment), QByteArrayLiteral("!*'()"));
  return encoded.toStdString();
}

struct Candidate {
  // 磁碟上的入口。資料夾模型是那個 model3.json；zip 模型是 .zip 檔本身
  // （zip 內的入口檔名由 ModelAssets 記著，這裡不重複）。
  fs::path absPath;
  // 在 collectCandidates 就開好，主迴圈直接用 —— 否則每個 zip 會被開兩次
  std::shared_ptr<ModelAssets> assets;
  bool zip = false;
};

// 裸命名入口檔讀內容判斷版本；Cubism 4 以外（含 Cubism 2 與壞檔）一律不收
bool sniffGenericIsCubism4(const fs::path& absPath) {
  const auto doc = jsonu::Doc::parseFile(absPath);
  return doc && isCubism4Json(doc->root());
}

void collectCandidates(const fs::path& dir, int depth, std::vector<Candidate>& out) {
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) return;

  // 依名稱排序，行為才不受檔案系統列舉順序影響
  std::vector<fs::path> entries;
  for (const auto& entry : it) entries.push_back(entry.path());
  std::sort(entries.begin(), entries.end());

  std::vector<fs::path> dirs;
  for (const auto& abs : entries) {
    const std::string name = abs.filename().u8string();
    // 開頭是點的一律略過（隱藏目錄、AppleDouble 的 ._Foo.model3.json），
    // 但 stem 空的入口檔（整個檔名就是 ".model3.json"）是真模型 —— 見 isEmptyStemAssetName
    if (!name.empty() && name[0] == '.' && !isEmptyStemAssetName(name)) continue;
    if (ignoredDirs().count(name)) continue;

    std::error_code statEc;
    const bool isDir = fs::is_directory(abs, statEc);
    if (statEc) continue;

    if (isDir) {
      dirs.push_back(abs);
    } else if (endsWithInsensitive(name, ".model3.json")) {
      out.push_back({abs, openModelAssets(abs), false});
    } else if (isGenericEntryName(name)) {
      if (sniffGenericIsCubism4(abs)) out.push_back({abs, openModelAssets(abs), false});
    } else if (endsWithInsensitive(name, ".zip")) {
      // zip 要真的打開、讀中央目錄才知道裡面是不是模型包。不是的話靜靜略過 ——
      // models 目錄裡本來就可能躺著不相干的壓縮檔，那不是使用者的錯誤。
      auto assets = openModelAssets(abs);
      if (assets) {
        out.push_back({abs, std::move(assets), true});
      } else {
        qDebug() << "[models] 略過不是模型包的 zip" << QString::fromStdString(abs.u8string());
      }
    }
    // *.model.json（Cubism 2.1）：本專案不支援，直接略過
  }

  if (depth < kMaxDepth) {
    for (const auto& sub : dirs) collectCandidates(sub, depth + 1, out);
  }
}

// 取出每個動作的檔名（去掉路徑）；缺 File 欄位時補空字串，索引才不會錯位
std::string motionFileName(yyjson_val* item) {
  const std::string path = jsonu::getString(item, "File");
  const size_t pos = path.find_last_of('/');
  return pos == std::string::npos ? path : path.substr(pos + 1);
}

struct ParsedCubism4 {
  std::vector<MotionGroupInfo> motions;
  std::vector<std::string> expressions;
};

ParsedCubism4 parseCubism4(yyjson_val* root) {
  ParsedCubism4 result;
  yyjson_val* refs = jsonu::get(root, "FileReferences");

  yyjson_val* motions = jsonu::get(refs, "Motions");
  if (yyjson_is_obj(motions)) {
    size_t idx, max;
    yyjson_val* k;
    yyjson_val* v;
    yyjson_obj_foreach(motions, idx, max, k, v) {
      if (!yyjson_is_arr(v)) continue;
      MotionGroupInfo group;
      group.name = yyjson_get_str(k);
      group.count = static_cast<int>(yyjson_arr_size(v));
      size_t i, m;
      yyjson_val* item;
      yyjson_arr_foreach(v, i, m, item) group.files.push_back(motionFileName(item));
      result.motions.push_back(std::move(group));
    }
  }

  yyjson_val* expressions = jsonu::get(refs, "Expressions");
  if (yyjson_is_arr(expressions)) {
    size_t idx, max;
    yyjson_val* e;
    yyjson_arr_foreach(expressions, idx, max, e) {
      const std::string name = jsonu::getString(e, "Name");
      if (!name.empty()) result.expressions.push_back(name);
    }
  }

  return result;
}

// 同一個容器有多個入口時只留（排序後的）第一個。
// 資料夾模型的容器是所在資料夾；zip 的容器就是 zip 檔本身 ——
// 拿 parent_path() 當鍵的話 models/Foo.zip 會跟同層的 models/index.json 撞在一起。
std::vector<Candidate> dedupeByFolder(const std::vector<Candidate>& candidates) {
  std::vector<Candidate> result;
  std::set<std::string> seen;
  for (const auto& candidate : candidates) {
    const std::string container = candidate.zip ? candidate.absPath.u8string() : candidate.absPath.parent_path().u8string();
    if (seen.count(container)) continue;
    seen.insert(container);
    result.push_back(candidate);
  }
  return result;
}

// 相對路徑（POSIX 斜線）
std::string relativePosix(const fs::path& base, const fs::path& target) {
  std::error_code ec;
  const fs::path rel = fs::relative(target, base, ec);
  std::string s = (ec ? target : rel).generic_u8string();
  return s;
}

// 一個候選的「內容」部分：動作、表情、參數、內建項目、命名檔。
//
// 刻意不填 id／name／url —— 那三個的規則在「掃描整個 models 目錄」與
// 「開啟單一入口」（describeModel，Viewer 用）底下不一樣，由呼叫端各自填。
// 內容部分則必須一模一樣，所以只有這一份。
//
// 解析不出來回 nullopt；例外**不接**，留給呼叫端 —— scanModels 靠那個 try
// 保住整輪掃描（一個壞掉的模型不准把其他模型帶走）。
std::optional<ModelInfo> describeContents(const ModelAssets& assets, const fs::path& absPath) {
  // VTube Studio 模型與搬過檔的模型包，動作／表情要靠補全才找得到
  const EnrichedSettings enriched = loadCubism4Settings(assets);
  const auto& report = enriched.report;
  if (report.addedMotions || report.addedExpressions || report.repairedPaths || report.addedPose) {
    qDebug() << "[models]" << QString::fromStdString(absPath.u8string()) << "補全設定：動作 +" << report.addedMotions << "、表情 +" << report.addedExpressions << "、修正路徑" << report.repairedPaths
             << (report.addedPose ? "、補上 pose" : "");
  }
  std::optional<jsonu::Doc> enrichedDoc = jsonu::Doc::parse(enriched.json());
  if (!enrichedDoc) return std::nullopt;
  ParsedCubism4 parsed = parseCubism4(enrichedDoc->root());

  ModelInfo model;
  model.motions = std::move(parsed.motions);
  model.expressions = std::move(parsed.expressions);

  // 只有臉部追蹤用的模型會把表情做成參數
  model.parameters = describeParameters(assets, enrichedDoc->root());
  // 作者做了 .exp3.json 就以它為準，不要再從參數猜一份出來混淆
  if (model.expressions.empty()) {
    model.paramExpressions = expressionsFromDisplayInfo(model.parameters);
    for (const auto& e : model.paramExpressions) model.expressions.push_back(e.name);
  }

  // 內建動作／表情接在模型自己的項目後面 —— 排前面會讓 resolveMotion 的
  // 前綴比對先命中合成的那些，模型作者做的東西反而叫不動。
  // 撞名一律跳過：留著也只是叫不到的幽靈項目（resolve 一定先命中真的那個）。
  for (const auto& action : availableBuiltinActions(model.parameters)) {
    if (action.motion) {
      const bool taken = std::any_of(model.motions.begin(), model.motions.end(), [&action](const MotionGroupInfo& g) { return strutil::equalsInsensitive(g.name, action.name); });
      if (taken) continue;
      model.motions.push_back({action.name, 1, {}});
      model.builtinMotions.push_back(action.name);
    } else {
      const bool taken = std::any_of(model.expressions.begin(), model.expressions.end(), [&action](const std::string& e) { return strutil::equalsInsensitive(e, action.name); });
      if (taken) continue;
      model.expressions.push_back(action.name);
      model.builtinExpressions.push_back(action.name);
    }
  }

  // 命名檔就在模型旁邊，掃描時一起讀進來（zip 模型會退回讀 zip 內建的那一份）
  model.annotations = readAnnotations(absPath, assets);

  std::sort(model.motions.begin(), model.motions.end(), [](const MotionGroupInfo& a, const MotionGroupInfo& b) { return localeLess(a.name, b.name); });
  return model;
}

// 單一入口的顯示名稱。
// zip 是容器本身，取檔名去掉 .zip；資料夾模型取所在資料夾名（那通常就是角色名，
// 與 scanModels 對巢狀模型的規則一致）。資料夾名取不到（入口就放在磁碟根目錄）
// 才退回檔名去掉副檔名 —— 而副檔名剝完是空的（".model3.json"）就退回檔名本身，
// 名稱一空掉，清單上是一列空白、resolveModel 還會被空字串命中。
std::string singleEntryName(const fs::path& entryPath) {
  const std::string file = entryPath.filename().u8string();
  if (endsWithInsensitive(file, ".zip")) return file.substr(0, file.size() - 4);

  const std::string folder = entryPath.parent_path().filename().u8string();
  if (!folder.empty()) return folder;

  for (const char* suffix : {".model3.json", ".json"}) {
    if (!endsWithInsensitive(file, suffix)) continue;
    const std::string stem = file.substr(0, file.size() - std::string(suffix).size());
    // stem 空的入口檔剝完什麼都不剩，寧可退回檔名本身（理由同 scanModels 那邊）
    if (!stem.empty()) return stem;
    break;
  }
  return file;
}

}  // namespace

bool isGenericEntryName(const std::string& filename) { return equalsInsensitive(filename, "model.json") || equalsInsensitive(filename, "index.json"); }

bool isEmptyStemAssetName(const std::string& filename) {
  for (const char* name : {".model3.json", ".motion3.json", ".exp3.json", ".pose3.json", ".vtube.json"}) {
    if (equalsInsensitive(filename, name)) return true;
  }
  return false;
}

std::vector<ModelInfo> scanModels(const fs::path& modelsDir, const std::string& urlBase) {
  std::vector<Candidate> candidates;
  collectCandidates(modelsDir, 0, candidates);

  std::vector<ModelInfo> models;
  for (const auto& candidate : dedupeByFolder(candidates)) {
    // 這個 try 刻意包住**整個**候選的處理，不是只包 loadCubism4Settings：
    // 一個壞掉的模型不准把整輪掃描帶走。實際踩過的是 zip 條目名稱不是合法 UTF-8
    // 時 MSVC 的 fs::path 在 readAnnotations 裡丟例外 → 沒人接 → 整個行程
    // abort 成 0xC0000409，使用者看到的是「桌寵按重新掃描就消失」。
    try {
      std::optional<ModelInfo> described = describeContents(*candidate.assets, candidate.absPath);
      if (!described) continue;
      ModelInfo model = std::move(*described);

      const std::string relPath = relativePosix(modelsDir, candidate.absPath);
      const size_t slash = relPath.find_last_of('/');
      const std::string folder = slash == std::string::npos ? "." : relPath.substr(0, slash);
      std::string name;
      if (candidate.zip) {
        // zip 是容器本身，名稱一律取檔名去掉 .zip（不像資料夾模型是用所在資料夾名）——
        // 放在 models/sub/Foo.zip 時取「sub」是錯的
        const std::string file = candidate.absPath.filename().u8string();
        name = file.substr(0, file.size() - 4);
      } else if (folder == ".") {
        // 直接放在 models 根目錄的入口檔：用檔名去掉副檔名當名稱
        name = relPath;
        for (const char* suffix : {".model3.json", ".json"}) {
          if (endsWithInsensitive(name, suffix)) {
            name = name.substr(0, name.size() - std::string(suffix).size());
            break;
          }
        }
        // stem 空的入口檔（models 根目錄直接放一個 ".model3.json"）剝完什麼都不剩。
        // 空名稱有三個症狀：清單上是一列空白、modelListHint 產出
        // "Available models: , Hiyori"（AI 拿它自我修正的那句話就壞了）、
        // 而且 resolveModel 第二輪拿 name 比對時會被空字串靜默命中。
        if (name.empty()) name = relPath;
      } else {
        const size_t folderSlash = folder.find_last_of('/');
        name = folderSlash == std::string::npos ? folder : folder.substr(folderSlash + 1);
      }

      model.id = relPath;
      model.name = name;

      // URL：每個路徑段落各自做 percent-encoding
      std::string url = urlBase;
      size_t start = 0;
      while (start <= relPath.size()) {
        const size_t end = relPath.find('/', start);
        const std::string segment = relPath.substr(start, end == std::string::npos ? std::string::npos : end - start);
        url += "/" + encodeUriComponent(segment);
        if (end == std::string::npos) break;
        start = end + 1;
      }
      model.url = url;

      models.push_back(std::move(model));
    } catch (const std::exception& err) {
      qWarning() << "[models] 略過無法解析的模型" << QString::fromStdString(candidate.absPath.u8string()) << "：" << err.what();
      continue;
    }
  }

  std::sort(models.begin(), models.end(), [](const ModelInfo& a, const ModelInfo& b) { return localeLess(a.name, b.name); });
  return models;
}

std::optional<fs::path> findDirectoryEntry(const fs::path& dir) {
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) return std::nullopt;

  fs::directory_iterator it(dir, ec);
  if (ec) return std::nullopt;

  // 依名稱排序，挑中的入口才不受檔案系統列舉順序影響（同 collectCandidates）
  std::vector<fs::path> files;
  for (const auto& entry : it) {
    const fs::path& abs = entry.path();
    const std::string name = abs.filename().u8string();
    // 開頭是點的一律略過：macOS 打包留下的 ._Foo.model3.json 是 AppleDouble
    // 中繼資料而不是模型，撿到它就是「拖進來卻說解析失敗」。
    // 例外是 stem 空的入口檔（整個檔名就是 ".model3.json"）—— 見 isEmptyStemAssetName。
    if (name.empty() || (name[0] == '.' && !isEmptyStemAssetName(name))) continue;
    // entry.is_directory() 而不是 fs::is_directory(abs)：directory_entry 在 Windows 上
    // 沿用列舉時就拿到的屬性，省掉每個檔案一次獨立的 stat。這一支現在掛在拖放的
    // dragEnterEvent 上（ViewerWindow::looksLikeModelPath），從慢速或斷線的網路磁碟機
    // 拖一個資料夾進來時，那些 stat 是卡在 GUI 執行緒上的。
    std::error_code statEc;
    if (entry.is_directory(statEc) || statEc) continue;
    files.push_back(abs);
  }
  std::sort(files.begin(), files.end());

  // 兩輪而不是一輪：同一層同時有 Foo.model3.json 與 model.json 時該收的一定是
  // 前者，那個優先順序不該由檔名排序決定。
  for (const auto& abs : files) {
    if (endsWithInsensitive(abs.filename().u8string(), ".model3.json")) return abs;
  }
  for (const auto& abs : files) {
    if (isGenericEntryName(abs.filename().u8string()) && sniffGenericIsCubism4(abs)) return abs;
  }
  return std::nullopt;
}

std::optional<ModelInfo> describeModel(const fs::path& entryPath) {
  // 不是模型包的 zip、壞掉的入口檔，openModelAssets 一律回 nullptr（不丟例外）
  const std::unique_ptr<ModelAssets> assets = openModelAssets(entryPath);
  if (!assets) return std::nullopt;

  // try 的理由與 scanModels 那邊完全相同（zip 條目名不是合法 UTF-8 時
  // MSVC 的 fs::path 會丟例外）。差別只在這裡沒有「其他模型」要保住，
  // 純粹是不准讓一個壞檔案把整個 Viewer 帶走。
  try {
    std::optional<ModelInfo> model = describeContents(*assets, entryPath);
    if (!model) return std::nullopt;
    model->id = entryPath.generic_u8string();
    model->name = singleEntryName(entryPath);
    // url 刻意留空，理由見標頭
    return model;
  } catch (const std::exception& err) {
    qWarning() << "[models] 無法解析模型" << QString::fromStdString(entryPath.u8string()) << "：" << err.what();
    return std::nullopt;
  }
}

}  // namespace l2m
