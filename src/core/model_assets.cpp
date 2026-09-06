#include "model_assets.h"

#include <algorithm>
#include <fstream>
#include <set>

#include "json_doc.h"
#include "model_scanner.h"
#include "model_settings.h"
#include "string_util.h"

namespace l2m {

namespace fs = std::filesystem;
using strutil::endsWithInsensitive;

namespace {

// ── 資料夾模型 ───────────────────────────────────────────────────────────
class DirModelAssets : public ModelAssets {
public:
  DirModelAssets(fs::path root, std::string entry) : root_(std::move(root)), entry_(std::move(entry)) {}

  const std::string& entryName() const override { return entry_; }

  std::string describe() const override { return (root_ / fs::u8path(entry_)).u8string(); }

  std::optional<std::string> read(const std::string& rel) const override { return jsonu::readFileUtf8(root_ / fs::u8path(rel)); }

  std::optional<std::string> readPrefix(const std::string& rel, std::size_t maxBytes) const override {
    std::ifstream file(root_ / fs::u8path(rel), std::ios::binary);
    if (!file) return std::nullopt;
    if (maxBytes == 0) return std::string();
    std::string out(maxBytes, '\0');
    file.read(out.data(), static_cast<std::streamsize>(maxBytes));
    const std::streamsize got = file.gcount();
    out.resize(got > 0 ? static_cast<std::size_t>(got) : 0);
    return out;
  }

  bool exists(const std::string& rel) const override {
    std::error_code ec;
    return fs::exists(root_ / fs::u8path(rel), ec);
  }

  std::vector<std::string> list(const std::string& suffix, int maxDepth) const override {
    std::vector<std::string> found;
    listInto(root_, suffix, 1, maxDepth, "", found);
    std::sort(found.begin(), found.end());
    return found;
  }

private:
  // depth 是「這一層的檔案會有幾個路徑段」，所以從 1 開始
  static void listInto(const fs::path& dir, const std::string& suffix, int depth, int maxDepth, const std::string& prefix, std::vector<std::string>& found) {
    std::error_code ec;
    fs::directory_iterator it(dir, ec);
    if (ec) return;
    for (const auto& entry : it) {
      const std::string name = entry.path().filename().u8string();
      // 隱藏檔不列出（但直接指名照樣讀得到）；stem 空的資產檔名不算隱藏檔，
      // 見 model_scanner.h 的 isEmptyStemAssetName
      if (!name.empty() && name[0] == '.' && !isEmptyStemAssetName(name)) continue;
      std::error_code statEc;
      if (entry.is_directory(statEc)) {
        if (depth < maxDepth) listInto(entry.path(), suffix, depth + 1, maxDepth, prefix + name + "/", found);
      } else if (endsWithInsensitive(name, suffix)) {
        found.push_back(prefix + name);
      }
    }
  }

  fs::path root_;
  std::string entry_;
};

// ── zip 模型 ─────────────────────────────────────────────────────────────
class ZipModelAssets : public ModelAssets {
public:
  ZipModelAssets(fs::path zipPath, std::unique_ptr<ZipArchive> zip, std::string root, std::string entry)
    : zipPath_(std::move(zipPath)), zip_(std::move(zip)), root_(std::move(root)), entry_(std::move(entry)) {}

  const std::string& entryName() const override { return entry_; }

  std::string describe() const override { return zipPath_.u8string() + "!" + root_ + entry_; }

  std::optional<std::string> read(const std::string& rel) const override { return zip_->read(root_ + rel); }

  std::optional<std::string> readPrefix(const std::string& rel, std::size_t maxBytes) const override { return zip_->readPrefix(root_ + rel, maxBytes); }

  bool exists(const std::string& rel) const override { return zip_->contains(root_ + rel); }

  std::vector<std::string> list(const std::string& suffix, int maxDepth) const override {
    std::vector<std::string> found;
    // entries() 已經正規化並排序過，逐一過濾即可（不必再排一次）
    for (const auto& full : zip_->entries()) {
      if (full.size() <= root_.size()) continue;
      if (full.compare(0, root_.size(), root_) != 0) continue;
      const std::string rel = full.substr(root_.size());
      if (segmentCount(rel) > maxDepth) continue;
      if (!endsWithInsensitive(rel, suffix)) continue;
      found.push_back(rel);
    }
    return found;
  }

private:
  static int segmentCount(const std::string& path) { return 1 + static_cast<int>(std::count(path.begin(), path.end(), '/')); }

  fs::path zipPath_;
  std::unique_ptr<ZipArchive> zip_;
  // zip 內的模型根目錄前綴：直接放在 zip 根目錄時是空字串，多包一層時是 "Hiyori/"
  std::string root_;
  std::string entry_;
};

// 在 zip 的某一層（不進子目錄）找入口檔
std::optional<std::string> findEntryUnder(const ZipArchive& zip, const std::string& root) {
  std::vector<std::string> generic;
  for (const auto& full : zip.entries()) {
    if (full.size() <= root.size()) continue;
    if (full.compare(0, root.size(), root) != 0) continue;
    const std::string rel = full.substr(root.size());
    if (rel.find('/') != std::string::npos) continue;  // 只看這一層
    // entries() 已排序，所以第一個命中的就是「排序後的第一個」
    if (endsWithInsensitive(rel, ".model3.json")) return full;
    if (isGenericEntryName(rel)) generic.push_back(full);
  }

  // 裸命名入口分不出 Cubism 版本，得讀內容才知道（同 model_scanner 的 sniff）
  for (const auto& candidate : generic) {
    const auto text = zip.read(candidate);
    if (!text) continue;
    const auto doc = jsonu::Doc::parse(*text);
    if (doc && isCubism4Json(doc->root())) return candidate;
  }
  return std::nullopt;
}

}  // namespace

std::optional<std::string> findZipEntry(const ZipArchive& zip) {
  if (const auto atRoot = findEntryUnder(zip, "")) return atRoot;

  // 根目錄沒有入口檔：找唯一的頂層資料夾再試一次。
  // 兩個以上就代表這不是單一模型包，無從決定要哪個，直接放棄。
  std::set<std::string> topDirs;
  for (const auto& full : zip.entries()) {
    const size_t slash = full.find('/');
    if (slash == std::string::npos) continue;
    topDirs.insert(full.substr(0, slash + 1));
    if (topDirs.size() > 1) return std::nullopt;
  }
  if (topDirs.size() != 1) return std::nullopt;
  return findEntryUnder(zip, *topDirs.begin());
}

std::unique_ptr<ModelAssets> openDirectoryAssets(const fs::path& modelDir, const std::string& entryName) { return std::make_unique<DirModelAssets>(modelDir, entryName); }

std::unique_ptr<ModelAssets> openModelAssets(const fs::path& entryPath) {
  const std::string filename = entryPath.filename().u8string();

  if (!endsWithInsensitive(filename, ".zip")) {
    return std::make_unique<DirModelAssets>(entryPath.parent_path(), filename);
  }

  auto zip = ZipArchive::open(openFileByteSource(entryPath));
  if (!zip) return nullptr;
  const auto entry = findZipEntry(*zip);
  if (!entry) return nullptr;

  // 入口檔一定在所選根目錄的那一層，所以「最後一個斜線之前」就是前綴
  const size_t slash = entry->find_last_of('/');
  const std::string root = slash == std::string::npos ? std::string() : entry->substr(0, slash + 1);
  const std::string name = slash == std::string::npos ? *entry : entry->substr(slash + 1);
  return std::make_unique<ZipModelAssets>(entryPath, std::move(zip), root, name);
}

}  // namespace l2m
