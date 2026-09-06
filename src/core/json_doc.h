#pragma once

// yyjson 的薄 RAII 包裝。
//
// 只做三件事：管理 doc 生命週期、以 UTF-8 讀檔解析、常用取值輔助。
// 各模組拿到的是原生 yyjson_val* / yyjson_mut_val*，不另外抽象一層。

#include <yyjson.h>

#include <filesystem>
#include <optional>
#include <string>

namespace l2m::jsonu {

// 讀取整個檔案（UTF-8 位元組）；失敗回 nullopt。
// 不用 yyjson_read_file 是因為它吃 char* 路徑，Windows 上非 ASCII 路徑會壞。
std::optional<std::string> readFileUtf8(const std::filesystem::path& path);

// 不可變文件（解析結果）
class Doc {
public:
  Doc() = default;
  explicit Doc(yyjson_doc* doc) : doc_(doc) {}
  Doc(Doc&& other) noexcept : doc_(other.doc_) { other.doc_ = nullptr; }
  Doc& operator=(Doc&& other) noexcept {
    if (this != &other) {
      if (doc_) yyjson_doc_free(doc_);
      doc_ = other.doc_;
      other.doc_ = nullptr;
    }
    return *this;
  }
  Doc(const Doc&) = delete;
  Doc& operator=(const Doc&) = delete;
  ~Doc() {
    if (doc_) yyjson_doc_free(doc_);
  }

  static std::optional<Doc> parse(const std::string& text);
  static std::optional<Doc> parseFile(const std::filesystem::path& path);

  yyjson_val* root() const { return doc_ ? yyjson_doc_get_root(doc_) : nullptr; }
  explicit operator bool() const { return doc_ != nullptr; }

private:
  yyjson_doc* doc_ = nullptr;
};

// 可變文件（建構或改寫用）
class MutDoc {
public:
  MutDoc() : doc_(yyjson_mut_doc_new(nullptr)) {}
  MutDoc(MutDoc&& other) noexcept : doc_(other.doc_) { other.doc_ = nullptr; }
  MutDoc& operator=(MutDoc&& other) noexcept {
    if (this != &other) {
      if (doc_) yyjson_mut_doc_free(doc_);
      doc_ = other.doc_;
      other.doc_ = nullptr;
    }
    return *this;
  }
  MutDoc(const MutDoc&) = delete;
  MutDoc& operator=(const MutDoc&) = delete;
  ~MutDoc() {
    if (doc_) yyjson_mut_doc_free(doc_);
  }

  yyjson_mut_doc* get() const { return doc_; }
  yyjson_mut_val* root() const { return doc_ ? yyjson_mut_doc_get_root(doc_) : nullptr; }
  void setRoot(yyjson_mut_val* val) { yyjson_mut_doc_set_root(doc_, val); }

  // 從不可變值深拷貝進本文件（用於「讀入 → 改寫」流程）
  yyjson_mut_val* copyOf(yyjson_val* val) { return yyjson_val_mut_copy(doc_, val); }

  // 序列化；pretty 用 2 空白縮排
  std::string write(bool pretty = false) const;

private:
  yyjson_mut_doc* doc_ = nullptr;
};

// --- 取值輔助（key 不存在或型別不符時回退） ---

inline yyjson_val* get(yyjson_val* obj, const char* key) { return obj ? yyjson_obj_get(obj, key) : nullptr; }

inline std::string getString(yyjson_val* obj, const char* key, const std::string& fallback = "") {
  yyjson_val* v = get(obj, key);
  const char* s = v ? yyjson_get_str(v) : nullptr;
  return s ? std::string(s) : fallback;
}

inline std::string asString(yyjson_val* val, const std::string& fallback = "") {
  const char* s = val ? yyjson_get_str(val) : nullptr;
  return s ? std::string(s) : fallback;
}

}  // namespace l2m::jsonu
