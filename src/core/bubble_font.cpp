#include "bubble_font.h"

#include "string_util.h"

namespace l2m {

namespace {

// locale.rfind(prefix, 0) == 0 就是「以 prefix 開頭」，
// 專案裡其他地方（bubble_window.cpp 原本的寫法）也是這個慣用法
bool startsWith(const std::string& text, const char* prefix) { return text.rfind(prefix, 0) == 0; }

}  // namespace

std::vector<std::string> bubbleFontCandidates(const std::string& locale) {
  // 日韓要有自己的家族，否則同一組漢字會被套成中文字形
  if (startsWith(locale, "ja")) return {"Yu Gothic UI", "Meiryo UI"};
  if (startsWith(locale, "ko")) return {"Malgun Gothic"};
  // 簡體要在 zh 的一般規則之前判斷，不然 zh-CN 會被正黑體收走
  if (locale == "zh-CN") return {"Microsoft YaHei UI", "Microsoft YaHei"};
  if (startsWith(locale, "zh")) return {"Microsoft JhengHei UI", "Microsoft JhengHei"};
  return {"Segoe UI"};
}

std::string pickInstalledFamily(const std::vector<std::string>& candidates, const std::vector<std::string>& installed) {
  for (const std::string& candidate : candidates) {
    for (const std::string& family : installed) {
      // 回傳資料庫裡的那份寫法，而不是我們寫死的那份 ——
      // 交給 Qt 的名字必須跟它自己列舉出來的一字不差，才不會又觸發後援解析
      if (strutil::equalsInsensitive(candidate, family)) return family;
    }
  }
  return {};
}

}  // namespace l2m
