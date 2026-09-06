#pragma once

// 初次啟動種入的預設角色（私人秘書），兼作使用者寫自己角色的 template。
//
// 為什麼要種：第一次啟動時 personas/ 是空的，使用者面對一個空清單，
// 看不出角色描述該長什麼樣，更看不出 # 分區各要寫什麼。種一份可用的
// 預設角色同時解決這兩件事。
//
// 內容是 resources/personas/Default.<locale>.md 五份真的 markdown 檔
//（可直接編輯、直接 diff，翻譯的人不必碰 C++），經 resources/personas.qrc
// 編進 **l2m_core**（不能塞進掛在執行檔上的 resources.qrc —— 測試只連
// l2m_core，看不到它）。編進二進位檔，零部署面。
//
// 刻意不給角色取專有名字：檔名 Default.md 就是顯示名稱，描述裡再冒出
// 另一個名字只會讓使用者搞不清楚「它到底叫什麼」。想命名就把檔案改名 ——
// 這正好是 template 該教會使用者的事。

#include <string>

namespace l2m {

inline constexpr const char* kDefaultPersonaName = "Default";

// 要寫進 personas/Default.md 的完整內容（含三個 # 區塊）。
// locale 先過 i18n::matchLocale，對不到就退回 en。
// 讀不到時回空字串 —— 呼叫端要當成「這次不種」而不是寫一個空檔案。
std::string defaultPersonaMarkdown(const std::string& locale);

}  // namespace l2m
