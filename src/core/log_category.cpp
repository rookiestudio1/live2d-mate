#include "log_category.h"

namespace l2m {

LogCategory splitLogCategory(std::string_view message) {
  LogCategory result;
  if (message.empty() || message.front() != '[') {
    result.body = std::string(message);
    return result;
  }

  const size_t limit = message.size() < kMaxPrefixLen ? message.size() : kMaxPrefixLen;
  const size_t close = message.find(']', 1);
  if (close == std::string_view::npos || close >= limit || close == 1) {
    result.body = std::string(message);
    return result;
  }

  bool hasLetter = false;
  for (size_t i = 1; i < close; ++i) {
    const char c = message[i];
    const bool letter = c >= 'a' && c <= 'z';
    const bool ok = letter || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
    if (!ok) {
      result.body = std::string(message);
      return result;
    }
    hasLetter = hasLetter || letter;
  }
  // 純數字的方括號（"[404] Not Found"）是本文，不是分類名
  if (!hasLetter) {
    result.body = std::string(message);
    return result;
  }

  // ']' 之後要嘛是字串結尾，要嘛是一個半形空格。其餘（例如 "[abc]x"）不算前綴
  if (close + 1 < message.size() && message[close + 1] != ' ') {
    result.body = std::string(message);
    return result;
  }

  result.name = std::string(message.substr(1, close - 1));
  // 只吃掉一個空格，後面的留著（[perf] 的對齊欄位靠它）
  const size_t bodyStart = close + 1 < message.size() ? close + 2 : message.size();
  result.body = std::string(message.substr(bodyStart));
  return result;
}

}  // namespace l2m
