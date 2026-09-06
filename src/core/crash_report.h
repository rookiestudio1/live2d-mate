#pragma once

// 當機報告的純文字組裝與 crash 檔的命名／保留規則 —— 字串與整數算術，
// 不碰檔案系統。
//
// 放在 l2m_core 而不是 platform/crash_handler_win.cpp：那支只在當機路徑上執行，
// 沒有任何辦法用 QTest 驗證。而這裡每一支錯了的後果都是**靜默**的 ——
// writeHex 少補一位、formatFrame 減錯基底，產出的報告格式完全正確，
// 只是每一行都指向別的函式；保留規則多刪一份，就是把使用者唯一那次當機現場
// 砍掉了（與 core/log_rotation.h 同一個理由）。
//
// **兩種介面刻意並存**：`write*` 系列寫進呼叫端給的固定緩衝，一個位元組都不配置，
// 給 crash handler 的第一階段用（那裡連 malloc 都不能碰，禁用清單見
// platform/crash_handler.h）；回傳 std::string／std::vector 的那幾支只給
// 「下次正常啟動時的掃描與清理」用，那條路徑在 GUI 執行緒上，配置記憶體沒問題。

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace l2m {

// 堆疊的層數上限。這個數字不是隨便挑的：CaptureStackBackTrace 在 Windows 8
// 以前的 x64 上就是卡在 62（FramesToSkip + FramesToCapture < 63），兩個擷取路徑
// 用同一個上限，crash 檔的格式與這裡的緩衝大小才只有一套。
// 實務上超過 62 層的只有無窮遞迴，那種現場看最上面十層就夠了。
inline constexpr int kMaxFrames = 62;

// 大寫十六進位寫進 out，**不寫結尾的 '\0'**，回傳寫了幾個字元。
// digits 是最小寬度（不足補零），0 表示用最短表示。
// cap 放不下就一個位元組都不寫並回 0 —— handler 那邊沒有第二次機會，
// 寧可整行跳過也不要寫出半行。
//
// **寬度不足時往上放寬而不是截斷**：0x1234 印成 "34" 會把讀報告的人帶去
// 完全錯誤的函式，而且從輸出上完全看不出來出過事。
size_t writeHex(char* out, size_t cap, unsigned long long value, int digits);

// 一個 frame 的文字化，寫進 out 並補上結尾 '\0'，回傳字元數（不含 '\0'）。
// 位址落在 [imageBase, imageBase + imageSize) 就寫成 "live2d_mate.exe+0x001A2B3C"，
// 否則寫絕對位址 "0x00007FFC12345678"。
//
// **RVA 是唯一能離線還原的形式**：ASLR 讓每次載入的基底都不同，
// 絕對位址事後對不回任何一份 pdb。
//
// imageName 為 nullptr 或 imageSize 為 0（＝基底沒抓到）時一律走絕對位址，
// 不能算出一個看起來很像真的 RVA。
size_t formatFrame(char* out, size_t cap, unsigned long long address, unsigned long long imageBase, unsigned long long imageSize, const char* imageName);

// crash 檔名裡的時間戳。刻意不用 QDateTime／std::chrono：前者把 l2m_core 綁到 Qt
// 的日期型別，後者要 C++20，而這裡只需要七個整數 —— 同 core/log_rotation.h 的 LogDate。
struct CrashStamp {
  int year = 0;
  int month = 0;
  int day = 0;
  int hour = 0;
  int minute = 0;
  int second = 0;
  unsigned long pid = 0;

  friend bool operator==(const CrashStamp& a, const CrashStamp& b) {
    return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour && a.minute == b.minute && a.second == b.second && a.pid == b.pid;
  }
  friend bool operator!=(const CrashStamp& a, const CrashStamp& b) { return !(a == b); }
};

// 檔名前綴。crash 檔跟一般 log 放同一個 logs/ 資料夾（使用者回報時整個壓起來就好），
// 所以前綴必須跟 kLogFileStem 分得開
inline constexpr const char* kCrashFileStem = "crash";

// 檔名格式：crash-20260831-142345-12345.log
//           （日期）  （時間）  （pid）
//
// **日期時間欄位全部零填補、固定寬度，所以檔名的字典序就是時間序。**
// 「最新一份」與「該刪哪些」一律照字典序判定，不看 mtime —— mtime 會被
// 複製、還原、同步工具改掉，而那正是使用者把 logs/ 壓縮寄過來時會發生的事。
//
// 帶 pid 是因為主行程／--mcp-stdio 橋接／--splash 子行程可能同秒一起死。
// pid 是變動寬度，所以同秒多份時 pid 決定先後 —— 任意但穩定，
// 兩個不同行程的當機本來就沒有「誰比較新」可言。
size_t writeCrashFileName(char* out, size_t cap, const CrashStamp& stamp);

// 上者包一層。只給啟動端的掃描與清理用（那條路徑可以配置記憶體）
std::string crashFileName(const CrashStamp& stamp);

// 解析檔名。認不得就回 nullopt —— logs/ 裡的外來檔案（使用者自己丟的、
// 一般的 live2d_mate-*.log）一律不碰
std::optional<CrashStamp> parseCrashFileName(std::string_view name);

// crash 檔的保留份數。**刻意跟一般 log 的「留今天與前兩天」不同**：
// 使用者可能一個月才崩一次，按天數清會把唯一那次現場清掉。
// crash 檔又小又稀有，20 份放著幾乎不佔空間。
inline constexpr int kCrashKeepCount = 20;

// names 之中最新的一份（字典序最大）。一份都沒有回 nullopt。
// 認不得的檔名不參與比較
std::optional<std::string> latestCrashFile(const std::vector<std::string>& names);

// 該刪掉的檔名，**保持 names 的輸入順序**（同 core/log_rotation.h 的 expiredLogFiles）。
// 認不得的檔名永遠不進結果 —— logs/ 裡還有一般的 live2d_mate-*.log 與
// 使用者自己丟的東西。
//
// keepCount <= 0 是設定錯誤，寧可一個都不刪也不要把目錄清空
std::vector<std::string> expiredCrashFiles(const std::vector<std::string>& names, int keepCount);

// SEH／CRT 錯誤碼的可讀文字，例如 0xC0000005 -> "ACCESS_VIOLATION"。
// 回傳的是靜態字面值（handler 第一階段不能配置記憶體）。認不得回 "UNKNOWN"
const char* crashReasonText(unsigned long code);

}  // namespace l2m
