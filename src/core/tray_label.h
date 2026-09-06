#pragma once

// 系統匣與命名視窗共用的標籤格式化。
//
// 放在 l2m_core 而不是 tray.cpp：tray.cpp 編進執行檔，只連 l2m_core 的
// QTest 連不到它。

#include <string>

namespace l2m {

// 有命名就用意義當主標籤，原始名稱退到括號裡；
// 沒命名就維持原樣，使用者才知道模型本來就叫這個。
std::string formatNamedLabel(const std::string& locale, const std::string& meaning, const std::string& raw, const std::string& suffix = "");

}  // namespace l2m
