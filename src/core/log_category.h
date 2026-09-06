#pragma once

// 把訊息開頭的子系統前綴切出來，例如 "[live2d] 開不了模型" -> {"live2d", "開不了模型"}。
//
// 本專案的 qDebug／qWarning 一律寫成「[子系統] 中文訊息」（CLAUDE.md 的慣例），
// 全專案共 15 個前綴。日誌後端換成 spdlog 之後，這些前綴直接當成 named logger 的
// 名字用 —— 輸出長得一樣，但多了「可以按分類調等級」這件事。
//
// 放在 l2m_core：判定規則寬一格就會誤傷正常訊息（例如 SSE 的 [DONE] 哨兵、
// 或使用者角色描述裡的方括號），而誤判的後果是訊息本文被砍掉一截又沒人發現。
// 這種「錯了也不會當掉、只會靜靜少字」的規則一定要有測試釘住。

#include <string>
#include <string_view>

namespace l2m {

struct LogCategory {
  std::string name;  // 前綴內容，不含方括號。沒有前綴時是空字串
  std::string body;  // 去掉「前綴 + 其後一個半形空格」之後的訊息
};

// 判定條件（四個都要成立才算前綴）：
//   1. 第一個字元是 '['
//   2. 在 kMaxPrefixLen 個字元內找得到 ']'
//   3. 中間全部是小寫 ASCII 字母、數字、'-'、'.' 或 '_'，且**至少有一個字母**
//   4. ']' 後面接一個半形空格，或 ']' 就是字串結尾
//
// 第 3 點的兩半各有理由：只收小寫，所以 SSE 的 [DONE] 這種大寫哨兵不會被誤認成分類；
// 但一定要收數字，因為真前綴裡就有 [live2d] 與 [i18n]。而「至少有一個字母」是為了
// 擋掉 "[404] Not Found" 那種以純數字開頭的訊息 —— 那是本文，不是分類名。
// 第 4 點只吃掉**一個**空格 —— frame_profiler 的 "[perf]   update  n=..."
// 靠後面那幾個空格對齊欄位，多吃一個表格就歪了。
inline constexpr size_t kMaxPrefixLen = 16;

LogCategory splitLogCategory(std::string_view message);

}  // namespace l2m
