#include "i18n.h"

#include <QDebug>

#include <cctype>
#include <mutex>

#include "config_schema.h"
#include "json_doc.h"
#include "string_util.h"

namespace l2m::i18n {

namespace {

// 訊息值：字串或 {one, other} 單複數組
struct MessageValue {
  bool plural = false;
  std::string one;
  std::string other;  // 非複數時值放這裡
};

using Table = std::map<std::string, MessageValue>;

std::filesystem::path messagesDir;
std::map<std::string, Table> tables;
std::mutex mutex;

// 載入單一語系的 JSON 訊息表（載過就直接用快取）
const Table* tableFor(const std::string& locale) {
  std::lock_guard<std::mutex> lock(mutex);
  auto it = tables.find(locale);
  if (it != tables.end()) return &it->second;

  const auto doc = jsonu::Doc::parseFile(messagesDir / std::filesystem::u8path(locale + ".json"));
  if (!doc || !yyjson_is_obj(doc->root())) {
    qWarning() << "[i18n] 訊息表載入失敗:" << QString::fromStdString(locale);
    tables[locale] = {};
    return &tables[locale];
  }

  Table table;
  size_t idx, max;
  yyjson_val* k;
  yyjson_val* v;
  yyjson_obj_foreach(doc->root(), idx, max, k, v) {
    MessageValue value;
    if (yyjson_is_str(v)) {
      value.other = yyjson_get_str(v);
    } else if (yyjson_is_obj(v)) {
      value.plural = true;
      value.one = jsonu::getString(v, "one");
      value.other = jsonu::getString(v, "other");
    } else {
      continue;
    }
    table[yyjson_get_str(k)] = std::move(value);
  }
  tables[locale] = std::move(table);
  return &tables[locale];
}

// 單複數：只有 count 剛好是 1 時才用 one，其餘（含 0 與未提供）都用 other
const std::string& pick(const MessageValue& value, const TParams& params) {
  if (!value.plural) return value.other;
  return params.countValue().has_value() && *params.countValue() == 1 ? value.one : value.other;
}

// 取代 {name} 佔位符。找不到對應參數就原樣留著，翻譯出錯時比較好看出來。
// 線性掃描一次組出結果 —— 代入的值不會被再掃一次，
// 使用者寫的意義裡有大括號也不會被吃掉。
std::string interpolate(const std::string& text, const TParams& params) {
  if (params.values().empty()) return text;

  std::string result;
  result.reserve(text.size());
  size_t i = 0;
  while (i < text.size()) {
    if (text[i] != '{') {
      result += text[i++];
      continue;
    }
    // 找對應的 }，中間必須是 \w+
    size_t j = i + 1;
    while (j < text.size() && (std::isalnum(static_cast<unsigned char>(text[j])) || text[j] == '_')) ++j;
    if (j < text.size() && text[j] == '}' && j > i + 1) {
      const std::string name = text.substr(i + 1, j - i - 1);
      const auto it = params.values().find(name);
      if (it != params.values().end()) {
        result += it->second;
      } else {
        result += text.substr(i, j - i + 1);
      }
      i = j + 1;
    } else {
      result += text[i++];
    }
  }
  return result;
}

}  // namespace

const std::map<std::string, std::string>& localeAutonyms() {
  static const std::map<std::string, std::string> autonyms{
    {"en", "English"}, {"ja", "日本語"}, {"ko", "한국어"}, {"zh-CN", "简体中文"}, {"zh-TW", "繁體中文"},
  };
  return autonyms;
}

std::string matchLocale(const std::string& raw) {
  const std::string tag = strutil::toLowerAscii(strutil::trim(raw));
  if (tag.empty()) return kFallbackLocale;
  if (tag.rfind("ja", 0) == 0) return "ja";
  if (tag.rfind("ko", 0) == 0) return "ko";
  if (tag.rfind("zh", 0) == 0) {
    // zh-Hant / zh-TW / zh-HK / zh-MO 都是正體中文圈
    const bool traditional = tag.find("hant") != std::string::npos || tag.find("tw") != std::string::npos || tag.find("hk") != std::string::npos || tag.find("mo") != std::string::npos;
    return traditional ? "zh-TW" : "zh-CN";
  }
  return kFallbackLocale;
}

void setMessagesDir(const std::filesystem::path& dir) {
  std::lock_guard<std::mutex> lock(mutex);
  messagesDir = dir;
  tables.clear();
}

std::string translate(const std::string& locale, const std::string& key, const TParams& params) {
  const Table* table = tableFor(locale);
  auto it = table->find(key);
  if (it == table->end()) {
    const Table* fallback = tableFor(kFallbackLocale);
    it = fallback->find(key);
    if (it == fallback->end()) return key;
  }
  return interpolate(pick(it->second, params), params);
}

std::vector<std::string> messageKeys(const std::string& locale) {
  std::vector<std::string> keys;
  for (const auto& [key, value] : *tableFor(locale)) keys.push_back(key);
  return keys;
}

std::vector<std::string> blankMessages(const std::string& locale) {
  std::vector<std::string> blank;
  for (const auto& [key, value] : *tableFor(locale)) {
    if (value.plural) {
      if (strutil::trim(value.one).empty() || strutil::trim(value.other).empty()) blank.push_back(key);
    } else if (strutil::trim(value.other).empty()) {
      blank.push_back(key);
    }
  }
  return blank;
}

}  // namespace l2m::i18n
