#include "crash_report.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <string>
#include <string_view>

namespace l2m {
namespace {

constexpr char kHexDigits[] = "0123456789ABCDEF";

int hexWidthOf(unsigned long long value) {
  int width = 1;
  while (value >= 16) {
    value /= 16;
    ++width;
  }
  return width;
}

// 把字面值接到游標上，放不下回 false（**一個位元組都不寫**，不會寫進半個字面值）。
// 所有 append 都走這一支，容量檢查才只有一處。
//
// **這不代表緩衝裡不會留下半行。** formatFrame／writeCrashFileName 是一段一段接的，
// 中途某一段放不下就直接 return 0，前面已經寫進去的位元組會原樣留在緩衝裡，
// 而且**沒有結尾的 '\0'**。這是刻意的取捨：回傳 0 的語意是「這個緩衝的內容作廢」，
// 呼叫端一律照回傳長度使用（crash_handler_win.cpp 的 writeStack／writeSymbols
// 拿到 0 就整行跳過、連 g_used 都不推進），所以那些殘留位元組永遠不會被讀到。
// 在當機路徑上「回滾」沒有意義 —— 少一次寫入，就少一次踩到已經不可信的記憶體
bool appendLiteral(char*& out, size_t& left, const char* text) {
  const size_t n = std::strlen(text);
  if (n > left) return false;
  std::memcpy(out, text, n);
  out += n;
  left -= n;
  return true;
}

// 十進位接到游標上，width 不足補零（width 0 ＝ 不補）。
// 不用 snprintf：第一階段禁用 CRT stdio（會配置、會上鎖）
bool appendDec(char*& out, size_t& left, unsigned long long value, int width) {
  char digits[24];
  int n = 0;
  do {
    digits[n++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  const int pad = width > n ? width - n : 0;
  if (static_cast<size_t>(pad + n) > left) return false;
  for (int i = 0; i < pad; ++i) *out++ = '0';
  for (int i = n - 1; i >= 0; --i) *out++ = digits[i];
  left -= static_cast<size_t>(pad + n);
  return true;
}

bool allDigits(std::string_view text) {
  return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// 不檢查溢位，呼叫端負責先限制位數
unsigned long long toULL(std::string_view text) {
  unsigned long long value = 0;
  for (const char c : text) value = value * 10 + static_cast<unsigned long long>(c - '0');
  return value;
}

}  // namespace

size_t writeHex(char* out, size_t cap, unsigned long long value, int digits) {
  if (!out) return 0;
  // 寬度不足時往上放寬而不是截斷（理由見標頭）
  const int width = std::max(digits, hexWidthOf(value));
  if (static_cast<size_t>(width) > cap) return 0;
  for (int i = width - 1; i >= 0; --i) {
    out[i] = kHexDigits[value & 0xF];
    value >>= 4;
  }
  return static_cast<size_t>(width);
}

size_t formatFrame(char* out, size_t cap, unsigned long long address, unsigned long long imageBase, unsigned long long imageSize, const char* imageName) {
  if (!out || cap == 0) return 0;
  char* cursor = out;
  size_t left = cap - 1;  // 留一格給結尾的 '\0'

  // imageSize 0 ＝ 基底沒抓到，一律當界外
  const bool inside = imageName != nullptr && imageSize > 0 && address >= imageBase && address - imageBase < imageSize;
  if (inside) {
    if (!appendLiteral(cursor, left, imageName)) return 0;
    if (!appendLiteral(cursor, left, "+0x")) return 0;
    const size_t n = writeHex(cursor, left, address - imageBase, 8);
    if (n == 0) return 0;
    cursor += n;
    left -= n;
  } else {
    if (!appendLiteral(cursor, left, "0x")) return 0;
    const size_t n = writeHex(cursor, left, address, 16);
    if (n == 0) return 0;
    cursor += n;
    left -= n;
  }
  *cursor = '\0';
  return static_cast<size_t>(cursor - out);
}

size_t writeCrashFileName(char* out, size_t cap, const CrashStamp& stamp) {
  if (!out || cap == 0) return 0;
  char* cursor = out;
  size_t left = cap - 1;  // 留一格給結尾的 '\0'
  const auto u = [](int v) { return static_cast<unsigned long long>(v); };
  if (!appendLiteral(cursor, left, kCrashFileStem)) return 0;
  if (!appendLiteral(cursor, left, "-")) return 0;
  if (!appendDec(cursor, left, u(stamp.year), 4)) return 0;
  if (!appendDec(cursor, left, u(stamp.month), 2)) return 0;
  if (!appendDec(cursor, left, u(stamp.day), 2)) return 0;
  if (!appendLiteral(cursor, left, "-")) return 0;
  if (!appendDec(cursor, left, u(stamp.hour), 2)) return 0;
  if (!appendDec(cursor, left, u(stamp.minute), 2)) return 0;
  if (!appendDec(cursor, left, u(stamp.second), 2)) return 0;
  if (!appendLiteral(cursor, left, "-")) return 0;
  if (!appendDec(cursor, left, stamp.pid, 0)) return 0;
  if (!appendLiteral(cursor, left, ".log")) return 0;
  *cursor = '\0';
  return static_cast<size_t>(cursor - out);
}

std::string crashFileName(const CrashStamp& stamp) {
  char buf[64];
  const size_t n = writeCrashFileName(buf, sizeof(buf), stamp);
  return std::string(buf, n);
}

std::optional<CrashStamp> parseCrashFileName(std::string_view name) {
  const std::string prefix = std::string(kCrashFileStem) + '-';
  constexpr std::string_view kSuffix = ".log";
  if (name.size() <= prefix.size() + kSuffix.size()) return std::nullopt;
  if (name.substr(0, prefix.size()) != prefix) return std::nullopt;
  if (name.substr(name.size() - kSuffix.size()) != kSuffix) return std::nullopt;

  std::string_view middle = name.substr(prefix.size(), name.size() - prefix.size() - kSuffix.size());

  // 版面固定：8 碼日期 '-' 6 碼時間 '-' pid
  if (middle.size() < 8 + 1 + 6 + 1 + 1) return std::nullopt;
  if (middle[8] != '-' || middle[15] != '-') return std::nullopt;

  const std::string_view date = middle.substr(0, 8);
  const std::string_view time = middle.substr(9, 6);
  const std::string_view pid = middle.substr(16);
  if (!allDigits(date) || !allDigits(time) || !allDigits(pid)) return std::nullopt;
  // 位數上限 10：十進位 10 位可達 9,999,999,999，但 DWORD 上限 4,294,967,295 只有 10 位
  // （例如 9,999,999,999 > 4,294,967,295）。所以位數擋不住超大值，後面還要檢查數值
  if (pid.size() > 10) return std::nullopt;
  // DWORD 上限 4,294,967,295。超出就不是我們寫的檔案
  if (toULL(pid) > 4294967295ULL) return std::nullopt;

  CrashStamp stamp;
  stamp.year = static_cast<int>(toULL(date.substr(0, 4)));
  stamp.month = static_cast<int>(toULL(date.substr(4, 2)));
  stamp.day = static_cast<int>(toULL(date.substr(6, 2)));
  stamp.hour = static_cast<int>(toULL(time.substr(0, 2)));
  stamp.minute = static_cast<int>(toULL(time.substr(2, 2)));
  stamp.second = static_cast<int>(toULL(time.substr(4, 2)));
  stamp.pid = static_cast<unsigned long>(toULL(pid));

  if (stamp.month < 1 || stamp.month > 12) return std::nullopt;
  if (stamp.day < 1 || stamp.day > 31) return std::nullopt;
  if (stamp.hour > 23 || stamp.minute > 59 || stamp.second > 59) return std::nullopt;
  return stamp;
}

std::optional<std::string> latestCrashFile(const std::vector<std::string>& names) {
  const std::string* best = nullptr;
  for (const std::string& name : names) {
    if (!parseCrashFileName(name)) continue;
    // 檔名的字典序就是時間序（欄位全部零填補），不看 mtime
    if (!best || name > *best) best = &name;
  }
  if (!best) return std::nullopt;
  return *best;
}

std::vector<std::string> expiredCrashFiles(const std::vector<std::string>& names, int keepCount) {
  if (keepCount <= 0) return {};

  std::vector<std::string> known;
  for (const std::string& name : names) {
    if (parseCrashFileName(name)) known.push_back(name);
  }
  if (static_cast<int>(known.size()) <= keepCount) return {};

  std::sort(known.begin(), known.end(), std::greater<std::string>());
  const std::set<std::string> keep(known.begin(), known.begin() + keepCount);

  // 回傳保持輸入順序（同 expiredLogFiles）
  std::vector<std::string> expired;
  for (const std::string& name : names) {
    if (parseCrashFileName(name) && keep.count(name) == 0) expired.push_back(name);
  }
  return expired;
}

const char* crashReasonText(unsigned long code) {
  switch (code) {
    case 0xC0000005:
      return "ACCESS_VIOLATION";
    case 0xC0000006:
      return "IN_PAGE_ERROR";
    case 0xC000001D:
      return "ILLEGAL_INSTRUCTION";
    case 0xC0000025:
      return "NONCONTINUABLE_EXCEPTION";
    case 0xC0000026:
      return "INVALID_DISPOSITION";
    case 0xC000008C:
      return "ARRAY_BOUNDS_EXCEEDED";
    case 0xC000008E:
      return "FLT_DIVIDE_BY_ZERO";
    case 0xC0000094:
      return "INT_DIVIDE_BY_ZERO";
    case 0xC0000096:
      return "PRIV_INSTRUCTION";
    case 0xC00000FD:
      return "STACK_OVERFLOW";
    // MSVC 的 __fastfail 用這個碼（terminate -> abort 走的路）。
    // 三個歷史現場有兩個死在這裡，見設計文件 §1
    case 0xC0000409:
      return "STACK_BUFFER_OVERRUN";
    case 0xC0000374:
      return "HEAP_CORRUPTION";
    // MSVC C++ 例外的 SEH 編碼（'msc' | 0xE0000000）
    case 0xE06D7363:
      return "CPP_EXCEPTION";
    case 0x80000003:
      return "BREAKPOINT";
    default:
      return "UNKNOWN";
  }
}

}  // namespace l2m
