#include "config_store.h"

#include <QDebug>

#include <fstream>
#include <stdexcept>

#include "string_util.h"

namespace l2m {

namespace fs = std::filesystem;

ConfigStore::ConfigStore(fs::path filePath, QObject* parent) : QObject(parent), filePath_(std::move(filePath)) {
  data_ = load();

  // 300ms 防抖寫檔：連續變更只落盤一次
  writeTimer_.setSingleShot(true);
  writeTimer_.setInterval(300);
  connect(&writeTimer_, &QTimer::timeout, this, &ConfigStore::flush);
}

AppConfig ConfigStore::load() {
  std::error_code ec;
  if (!fs::exists(filePath_, ec)) return defaultConfig();

  const auto text = jsonu::readFileUtf8(filePath_);
  const auto doc = text ? jsonu::Doc::parse(*text) : std::nullopt;
  if (!doc) {
    backup("JSON 解析失敗");
    return defaultConfig();
  }

  std::vector<std::string> issues;
  auto parsed = parseConfig(doc->root(), &issues);
  if (!parsed) {
    std::string detail;
    for (const auto& issue : issues) detail += (detail.empty() ? "" : ", ") + issue;
    backup("欄位驗證失敗：" + detail);
    return defaultConfig();
  }
  return *parsed;
}

void ConfigStore::backup(const std::string& reason) {
  std::string name = filePath_.filename().u8string();
  if (strutil::endsWithInsensitive(name, ".json")) name = name.substr(0, name.size() - 5);
  const fs::path bak = filePath_.parent_path() / fs::u8path(name + ".bak.json");
  std::error_code ec;
  fs::rename(filePath_, bak, ec);
  if (!ec) {
    qWarning() << "[config]" << reason.c_str() << "；已備份並以預設值重建";
  } else {
    qWarning() << "[config]" << reason.c_str() << "；備份失敗，直接以預設值覆蓋";
  }
}

const AppConfig& ConfigStore::patch(const std::string& patchJson) {
  const auto patchDoc = jsonu::Doc::parse(patchJson);
  if (!patchDoc || !yyjson_is_obj(patchDoc->root())) {
    throw std::runtime_error("Invalid config value - patch must be a JSON object");
  }

  // 兩層合併：先把現況序列化回 JSON，再逐區塊覆蓋 patch 給的欄位
  const auto currentDoc = *jsonu::Doc::parse(serializeConfig(data_));
  jsonu::MutDoc merged;
  merged.setRoot(merged.copyOf(currentDoc.root()));

  size_t idx, max;
  yyjson_val* sectionKey;
  yyjson_val* sectionVal;
  yyjson_obj_foreach(patchDoc->root(), idx, max, sectionKey, sectionVal) {
    if (!yyjson_is_obj(sectionVal)) continue;
    const char* name = yyjson_get_str(sectionKey);
    yyjson_mut_val* target = yyjson_mut_obj_get(merged.root(), name);
    if (!target || !yyjson_mut_is_obj(target)) {
      target = yyjson_mut_obj(merged.get());
      yyjson_mut_obj_put(merged.root(), yyjson_mut_strcpy(merged.get(), name), target);
    }
    size_t i, m;
    yyjson_val* k;
    yyjson_val* v;
    yyjson_obj_foreach(sectionVal, i, m, k, v) { yyjson_mut_obj_put(target, yyjson_mut_strcpy(merged.get(), yyjson_get_str(k)), yyjson_val_mut_copy(merged.get(), v)); }
  }

  const auto mergedDoc = *jsonu::Doc::parse(merged.write());
  std::vector<std::string> issues;
  auto parsed = parseConfig(mergedDoc.root(), &issues);
  if (!parsed) {
    std::string detail;
    for (const auto& issue : issues) detail += (detail.empty() ? "" : "; ") + issue;
    throw std::runtime_error("Invalid config value - " + detail);
  }

  // 用序列化結果比對是否真的有變化
  if (serializeConfig(*parsed) == serializeConfig(data_)) return data_;

  data_ = *parsed;
  scheduleWrite();
  emit changed(data_);
  return data_;
}

void ConfigStore::scheduleWrite() { writeTimer_.start(); }

void ConfigStore::flush() {
  writeTimer_.stop();
  std::error_code ec;
  fs::create_directories(filePath_.parent_path(), ec);
  std::ofstream file(filePath_, std::ios::binary);
  if (!file) {
    qWarning() << "[config] 寫入失敗：無法開啟檔案";
    return;
  }
  file << serializeConfig(data_, true) << "\n";
}

}  // namespace l2m
