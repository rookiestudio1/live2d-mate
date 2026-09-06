#pragma once

// UI 訊息的多語系支援。
//
// 訊息表是執行期讀取的 JSON（i18n/<locale>.json），不用 Qt Linguist ——
// 整份 key 表以資料檔維護，改翻譯不必重新編譯。

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace l2m::i18n {

// 查不到翻譯時退回這個語系
inline constexpr const char* kFallbackLocale = "en";

// 各語系的自稱名。依慣例不翻譯，使用者才找得到自己的語言。
const std::map<std::string, std::string>& localeAutonyms();

// 把系統語系字串（en-US、ja、ko-KR、zh-Hant-TW、zh-CN…）對應到支援的語系。
// 對不到就退回英文，寧可顯示看得懂的外語也不要顯示空白。
std::string matchLocale(const std::string& raw);

// 訊息表所在目錄（含 en.json 等）。app 啟動時設定；測試指向 repo 的 i18n/。
void setMessagesDir(const std::filesystem::path& dir);

// 插值參數：{name} 佔位符的對應值；count 另外供單複數挑選
class TParams {
public:
  TParams& arg(const std::string& name, const std::string& value) {
    values_[name] = value;
    return *this;
  }
  TParams& count(int n) {
    count_ = n;
    values_["count"] = std::to_string(n);
    return *this;
  }

  const std::map<std::string, std::string>& values() const { return values_; }
  const std::optional<int>& countValue() const { return count_; }

private:
  std::map<std::string, std::string> values_;
  std::optional<int> count_;
};

// 翻譯一個 key。找不到翻譯先退回英文表；連英文都沒有就回傳 key 本身
// （key 打錯時總比顯示空白好）。
std::string translate(const std::string& locale, const std::string& key, const TParams& params = {});

// 診斷／測試用：列出某語系的全部 key 與是否有空白翻譯
std::vector<std::string> messageKeys(const std::string& locale);
std::vector<std::string> blankMessages(const std::string& locale);

}  // namespace l2m::i18n
