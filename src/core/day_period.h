#pragma once

// 小時 → 時段語意，與角色 .md 第四區「# Greeting by Time」的時段前綴解析。
//
// 第四區的格式：一行一句，行首可帶英文時段前綴（morning: / afternoon: /
// evening: / night:，大小寫不敏感，半形全形冒號都認）；沒有前綴的行
// 全時段有效。前綴用英文與區塊標題同一個理由 —— 它是解析用的標記，
// 設定視窗的標籤會教使用者怎麼寫。

#include <string>
#include <vector>

namespace l2m {

enum class DayPeriod { Morning, Afternoon, Evening, Night };

// 5~10 時早上、11~17 時下午、18~22 時晚上、23~4 時深夜。
// 界線沒有人調得出來，也不值得開設定 —— 錯半小時的代價只是招呼語早了一點。
DayPeriod dayPeriodFor(int hour);

// 時段的英文前綴（"morning"…），序列化與 UI 提示共用
const char* dayPeriodPrefix(DayPeriod period);

// 從第四區的行挑出「此刻適用」的候選：帶對應前綴的（去掉前綴）＋ 無前綴的。
// 前綴打錯字的行視為無前綴（全時段有效）—— 靜默丟掉整行更難查。
std::vector<std::string> greetingsForPeriod(const std::vector<std::string>& lines, DayPeriod period);

}  // namespace l2m
