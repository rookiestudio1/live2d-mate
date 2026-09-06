# 當機堆疊寫進 log 檔 — 實作計畫

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 行程異常結束時，把呼叫堆疊寫進 `%APPDATA%/live2d_mate/logs/crash-*.log`，而且拿到那份檔案的人看得懂。

**Architecture:** 四個進場點（`SetUnhandledExceptionFilter` / `std::set_terminate` / `_set_purecall_handler` / `_set_invalid_parameter_handler`）共用一個兩階段寫檔路徑。第一階段只用「不配置記憶體、不上鎖」拿得到的東西（`CaptureStackBackTrace`、`RtlVirtualUnwind`、安裝時快照的 exe 基底、`WriteFile`），寫完立刻 `FlushFileBuffers`；第二階段才用 DbgHelp 符號化並 append。純字串與整數算術全部放 `l2m_core` 配 QTest，OS 互動放 `src/platform/`。

**Tech Stack:** C++17、Qt 6.8.3（`build/rel`）、MSVC2022 x64、Ninja、kernel32/ntdll/DbgHelp（系統元件，不新增第三方相依）、QTest。

**Spec:** `docs/superpowers/specs/2026-08-31-crash-stack-logging-design.md`

## Global Constraints

- **縮排 2 空白**，無 tab；建構子初始化列表縮 2；行寬約 100。沒有 `.clang-format`。
- **註解一律繁體中文**；標頭開頭的區塊註解要寫「這是什麼／為什麼這樣設計」，帶實測數字或具體 bug。**這就是本專案的文件，請維持同樣密度。**
- **面向使用者／AI 的字串一律英文**；UI 字串走 `i18n::translate`，鍵值在 `i18n/{en,ja,ko,zh-CN,zh-TW}.json`。`qDebug`/`qWarning` 是中文並帶子系統前綴，本功能一律用 `[crash]`。
- 檔名 `snake_case.{h,cpp}`、`#pragma once`、平台實作用 `_win.cpp` / `_mac.mm` 後綴。
- 類別 `PascalCase`、成員尾底線 `foo_`、常數 `kCamelCase`、新列舉用 `enum class`。命名空間 `l2m` / `l2m::platform`，結尾大括號註記 `}  // namespace l2m`。
- include 順序：順序敏感的平台標頭最前面（`<windows.h>`、`<dbghelp.h>`），然後自己的標頭、Qt、std、專案標頭。
- **`l2m_core` 不得依賴 GUI/GL/OS API。** 所有測試只連 `l2m_core`。
- **`serializeConfig` 的鍵順序是既有 config.json 的合約**：新欄位一律接在既有鍵後面，新 section 接在最後面。
- **crash handler 第一階段的禁用清單**：不呼叫 Qt、不 `new`/`malloc`、不上任何鎖、不用 CRT stdio、不碰 `std::string`、不碰 `GetModuleHandleEx`（loader lock）。
- **保留份數 `kCrashKeepCount = 20`**，堆疊上限 `kMaxFrames = 62`，`%TEMP%` 是唯一退路。
- **commit 訊息繁體中文**，`類別：一句摘要` 形式（`新增：`／`修正：`／`效能：`），內文分項說明並帶 `file.cpp:line` 參照。

### 本機建置環境（CLAUDE.md 沒寫，缺了會誤判成程式碼壞掉）

`build/rel` 這棵樹配的是 **Qt 6.8.3**（`build/rel/CMakeCache.txt` 的 `Qt6_DIR`），不是 CLAUDE.md 範例裡的 6.11.2。

**建置**（要先載 vcvars，否則會出現 `fatal error C1083: Cannot open include file: 'string'`）。
**用 PowerShell 工具跑，不要用 Bash 工具的 `cmd //c`** —— Git Bash 會改寫 `//c` 與 Windows 路徑：

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel --target test_crash_report'
```

**跑測試**（ctest 不會自己加 Qt 的 DLL 路徑，缺了會全部以 `Exit code 0xc0000135` 失敗，看起來像測試壞掉）：

```powershell
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"; ctest --test-dir build/rel -R test_crash_report --output-on-failure
```

連結 `live2d_mate.exe` 時若桌寵正在執行會 `LNK1168`。使用者平常跑的是 Qt Creator 那棵 `build/Desktop_Qt_6_11_2_MSVC2022_64bit_RelWithDebInfo/`，不是 `build/rel`。**連結失敗那次仍會更新 exe 的 mtime**，ninja 下一次會說 "no work to do" —— 別相信，把 exe 刪掉再建。

改檔案時：Bash heredoc 會吃掉反斜線（即使 `<<'EOF'`），要寫反斜線請用 Write/Edit 工具。repo 行尾 CRLF/LF 混用，用腳本改檔時 anchor 一律不含換行字元。

---

## File Structure

| 檔案 | 責任 |
|---|---|
| `src/core/crash_report.h` / `.cpp` **（新）** | 位址與檔名的純文字組裝、保留規則、錯誤碼文字。零 OS 相依，全部可測 |
| `tests/test_crash_report.cpp` **（新）** | 上者的測試，自動進 CI（`tests.yml` 走 `L2M_BUILD_APP=OFF`） |
| `src/platform/crash_handler.h` **（新）** | 介面與「為什麼這樣設計」的文件 |
| `src/platform/crash_handler_win.cpp` **（新）** | 四個進場點、堆疊擷取、兩階段寫檔、`L2M_CRASH_TEST` |
| `src/platform/crash_handler_mac.mm` **（新）** | 空殼（macOS 的 install 版面還沒接） |
| `src/app/main.cpp` **（改）** | 安裝點（`--mcp-stdio` 分流之前）、啟動掃描、`L2M_CRASH_TEST` 計時器 |
| `src/mcp/mcp_http_server.cpp` **（改）** | `set_exception_handler` 補上 httplib worker 的缺口 |
| `src/core/config_schema.h` / `.cpp` **（改）** | `crash.lastNotified` |
| `src/windows/tray.h` / `.cpp` **（改）** | `notify()`：系統匣氣球通知 |
| `src/windows/settings/about_page.*` **（改）** | 「開啟日誌資料夾」按鈕 |
| `i18n/{en,ja,ko,zh-CN,zh-TW}.json` **（改）** | 三個新鍵 |
| `CMakeLists.txt` **（改）** | 來源清單、`l2m_add_test`、`Dbghelp`、`/PDBSTRIPPED`、install |
| `.github/workflows/release.yml` **（改）** | 完整 pdb 掛成獨立資產 |

---

## Task 1: 位址格式化（`writeHex` / `formatFrame`）

**Files:**
- Create: `src/core/crash_report.h`, `src/core/crash_report.cpp`
- Create: `tests/test_crash_report.cpp`
- Modify: `CMakeLists.txt`（`l2m_core` 來源清單尾端、`l2m_add_test` 清單尾端）

**Interfaces:**
- Consumes: 無
- Produces:
  - `size_t l2m::writeHex(char* out, size_t cap, unsigned long long value, int digits)`
  - `size_t l2m::formatFrame(char* out, size_t cap, unsigned long long address, unsigned long long imageBase, unsigned long long imageSize, const char* imageName)`
  - `inline constexpr int l2m::kMaxFrames = 62;`

- [ ] **Step 1: 建立標頭（先只放這一節的兩支）**

`src/core/crash_report.h`：

```cpp
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
size_t formatFrame(char* out, size_t cap, unsigned long long address,
                   unsigned long long imageBase, unsigned long long imageSize,
                   const char* imageName);

}  // namespace l2m
```

- [ ] **Step 2: 寫失敗的測試**

`tests/test_crash_report.cpp`：

```cpp
// 當機報告的位址格式化、檔名規則與保留策略。
//
// 這一支釘住的是「報告會不會靜靜指向錯的地方」：writeHex 少補一位零、
// formatFrame 減錯基底，產出的報告格式完全正確，只是每一行都指到別的函式 ——
// 那種錯誤在真的當機、而且有人拿去對 pdb 之前不會有任何人發現。
#include <QtTest>

#include <cstring>
#include <string>
#include <vector>

#include "core/crash_report.h"

using namespace l2m;

namespace {

// 把固定緩衝介面包成好讀的字串，測試裡才不必每次擺一個 char[]
std::string hex(unsigned long long value, int digits, size_t cap = 64) {
  std::vector<char> buf(cap, '\0');
  const size_t n = writeHex(buf.data(), cap, value, digits);
  return std::string(buf.data(), n);
}

std::string frame(unsigned long long addr, unsigned long long base, unsigned long long size,
                  const char* name = "live2d_mate.exe") {
  char buf[128];
  const size_t n = formatFrame(buf, sizeof(buf), addr, base, size, name);
  return std::string(buf, n);
}

}  // namespace

class TestCrashReport : public QObject {
  Q_OBJECT

private slots:
  // 固定寬度補零，一律大寫（WinDbg 與 llvm-symbolizer 兩邊都吃）
  void hexPadsToWidth() {
    QCOMPARE(hex(0, 8), std::string("00000000"));
    QCOMPARE(hex(0x1a2b3c, 8), std::string("001A2B3C"));
    QCOMPARE(hex(0xffffffffffffffffull, 16), std::string("FFFFFFFFFFFFFFFF"));
  }

  // digits = 0 是最短表示
  void hexMinimalWhenNoWidth() {
    QCOMPARE(hex(0, 0), std::string("0"));
    QCOMPARE(hex(0xabc, 0), std::string("ABC"));
  }

  // **寬度不足時絕對不能截掉高位**：把 0x1234 印成 "34" 是會把人帶去
  // 完全錯誤的函式的那種錯誤，寧可讓欄位變寬
  void hexNeverTruncatesHighDigits() { QCOMPARE(hex(0x1234, 2), std::string("1234")); }

  // 緩衝放不下就一個位元組都不寫（handler 那邊沒有第二次機會）
  void hexRefusesShortBuffer() {
    char buf[4];
    std::memset(buf, '#', sizeof(buf));
    QCOMPARE(writeHex(buf, 4, 0x123456, 8), size_t(0));
    QCOMPARE(buf[0], '#');
  }

  // 落在模組區間內 -> module+RVA，RVA 補到 8 位
  void frameInsideImageUsesRva() {
    QCOMPARE(frame(0x7ff6a01a2b3cull, 0x7ff6a0000000ull, 0xabc000ull),
             std::string("live2d_mate.exe+0x001A2B3C"));
  }

  // 邊界：基底本身是 RVA 0（界內），base + size 已經是界外
  void frameBoundaries() {
    QCOMPARE(frame(0x7ff6a0000000ull, 0x7ff6a0000000ull, 0xabc000ull),
             std::string("live2d_mate.exe+0x00000000"));
    QCOMPARE(frame(0x7ff6a0abc000ull, 0x7ff6a0000000ull, 0xabc000ull),
             std::string("0x00007FF6A0ABC000"));
    QCOMPARE(frame(0x7ff69fffffffull, 0x7ff6a0000000ull, 0xabc000ull),
             std::string("0x00007FF69FFFFFFF"));
  }

  // size 0 代表「基底沒抓到」，一律當界外 —— 不能算出一個看起來很像真的 RVA
  void frameWithoutImageSizeIsAbsolute() {
    QCOMPARE(frame(0x7ff6a01a2b3cull, 0x7ff6a0000000ull, 0),
             std::string("0x00007FF6A01A2B3C"));
  }

  // 模組名沒有就只能印絕對位址
  void frameWithoutNameIsAbsolute() {
    QCOMPARE(frame(0x7ff6a01a2b3cull, 0x7ff6a0000000ull, 0xabc000ull, nullptr),
             std::string("0x00007FF6A01A2B3C"));
  }

  // 緩衝不夠就回 0；呼叫端據此整行跳過，而不是寫出半行
  void frameRefusesShortBuffer() {
    char buf[8];
    QCOMPARE(formatFrame(buf, sizeof(buf), 0x7ff6a01a2b3cull, 0x7ff6a0000000ull, 0xabc000ull,
                         "live2d_mate.exe"),
             size_t(0));
  }
};

QTEST_APPLESS_MAIN(TestCrashReport)
#include "test_crash_report.moc"
```

- [ ] **Step 3: 接上 CMake**

在 `CMakeLists.txt` 的 `add_library(l2m_core STATIC ...)` 清單**最後一組**（`src/core/gaze_director.cpp` 那類最新加入的項目之後）加入：

```cmake
  src/core/crash_report.h
  src/core/crash_report.cpp
```

在 `l2m_add_test(...)` 清單最後（`l2m_add_test(test_gaze_director)` 之後）加入：

```cmake
  l2m_add_test(test_crash_report)
```

- [ ] **Step 4: 跑測試確認它「編不過」**

```powershell
cmake -S . -B build/rel 1>$null; cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel --target test_crash_report'
```

Expected: FAIL，連結期 `unresolved external symbol` 指向 `l2m::writeHex` 與 `l2m::formatFrame`（`crash_report.cpp` 還是空的）。

- [ ] **Step 5: 寫最小實作**

`src/core/crash_report.cpp`：

```cpp
#include "crash_report.h"

#include <algorithm>
#include <cstring>

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

// 把字面值接到游標上，放不下回 false。所有 append 都走這一支，
// 「寫出半行」才不會有第二種可能
bool appendLiteral(char*& out, size_t& left, const char* text) {
  const size_t n = std::strlen(text);
  if (n > left) return false;
  std::memcpy(out, text, n);
  out += n;
  left -= n;
  return true;
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

size_t formatFrame(char* out, size_t cap, unsigned long long address,
                   unsigned long long imageBase, unsigned long long imageSize,
                   const char* imageName) {
  if (!out || cap == 0) return 0;
  char* cursor = out;
  size_t left = cap - 1;  // 留一格給結尾的 '\0'

  // imageSize 0 ＝ 基底沒抓到，一律當界外
  const bool inside = imageName != nullptr && imageSize > 0 && address >= imageBase &&
                      address - imageBase < imageSize;
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

}  // namespace l2m
```

- [ ] **Step 6: 跑測試確認通過**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel --target test_crash_report'
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"; ctest --test-dir build/rel -R test_crash_report --output-on-failure
```

Expected: PASS，9 個 slot 全綠。

- [ ] **Step 7: Commit**

```bash
git add src/core/crash_report.h src/core/crash_report.cpp tests/test_crash_report.cpp CMakeLists.txt
git commit -m "新增：當機報告的位址格式化（writeHex／formatFrame）"
```

---

## Task 2: crash 檔名的組裝與解析

**Files:**
- Modify: `src/core/crash_report.h`, `src/core/crash_report.cpp`
- Modify: `tests/test_crash_report.cpp`

**Interfaces:**
- Consumes: Task 1 的 `writeHex` 內部輔助（`appendLiteral`）
- Produces:
  - `struct l2m::CrashStamp { int year, month, day, hour, minute, second; unsigned long pid; }`
  - `size_t l2m::writeCrashFileName(char* out, size_t cap, const CrashStamp&)`
  - `std::string l2m::crashFileName(const CrashStamp&)`
  - `std::optional<CrashStamp> l2m::parseCrashFileName(std::string_view)`
  - `inline constexpr const char* l2m::kCrashFileStem = "crash";`

- [ ] **Step 1: 擴充標頭**

在 `src/core/crash_report.h` 的 `#include <cstddef>` 之後補上：

```cpp
#include <optional>
#include <string>
#include <string_view>
#include <vector>
```

在 `formatFrame` 宣告之後加入：

```cpp
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
    return a.year == b.year && a.month == b.month && a.day == b.day && a.hour == b.hour &&
           a.minute == b.minute && a.second == b.second && a.pid == b.pid;
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
```

- [ ] **Step 2: 寫失敗的測試**

在 `tests/test_crash_report.cpp` 的 `frameRefusesShortBuffer()` 之後加入：

```cpp
  // 日期時間補零、pid 不補零
  void crashNameFormat() {
    QCOMPARE(crashFileName({2026, 8, 31, 14, 23, 45, 12345}),
             std::string("crash-20260831-142345-12345.log"));
    QCOMPARE(crashFileName({2026, 1, 2, 3, 4, 5, 7}),
             std::string("crash-20260102-030405-7.log"));
  }

  // 固定緩衝版與 std::string 版必須一字不差 —— 前者給 handler 用、
  // 後者給啟動掃描用，兩邊對不上就是「寫出來的檔案自己掃不到」
  void crashNameBufferMatchesString() {
    const CrashStamp stamp{2026, 12, 31, 23, 59, 59, 4294967295u};
    char buf[64];
    const size_t n = writeCrashFileName(buf, sizeof(buf), stamp);
    QCOMPARE(std::string(buf, n), crashFileName(stamp));
  }

  // 緩衝放不下就完全不寫
  void crashNameRefusesShortBuffer() {
    char buf[8];
    QCOMPARE(writeCrashFileName(buf, sizeof(buf), {2026, 8, 31, 14, 23, 45, 12345}), size_t(0));
  }

  // 組裝與解析要能往返
  void crashNameRoundTrips() {
    const CrashStamp cases[] = {
      {2026, 8, 31, 14, 23, 45, 12345},
      {2028, 2, 29, 0, 0, 0, 1},          // 閏日、午夜
      {2026, 12, 31, 23, 59, 59, 4294967295u},  // DWORD 上限的 pid
    };
    for (const CrashStamp& stamp : cases) {
      const auto parsed = parseCrashFileName(crashFileName(stamp));
      QVERIFY(parsed.has_value());
      QVERIFY(*parsed == stamp);
    }
  }

  // logs/ 裡的外來檔案一律認不得。**"live2d_mate-2026-08-31.log" 是重點**：
  // crash 檔跟一般 log 同一個資料夾，認錯就會把一般 log 拿去做保留 20 份的清理
  void crashNameRejectsForeignNames() {
    const char* rejected[] = {
      "readme.txt",
      "live2d_mate-2026-08-31.log",         // 一般 log，絕對不能認
      "live2d_mate-2026-08-31.2.log",
      "crash.log",                          // 沒有時間戳
      "crash-20260831-142345.log",          // 少了 pid
      "crash-2026-08-31-142345-1.log",      // 日期帶分隔線
      "crash-20260831-142345-12345.txt",    // 副檔名不對
      "crash-20261331-142345-1.log",        // 月份 13
      "crash-20260832-142345-1.log",        // 日 32
      "crash-20260800-142345-1.log",        // 日 0
      "crash-20260831-246000-1.log",        // 時 24
      "crash-20260831-146000-1.log",        // 分 60
      "crash-20260831-142360-1.log",        // 秒 60
      "crash-20260831-142345-.log",         // 空 pid
      "crash-20260831-142345-x.log",        // pid 不是數字
      "crash-20260831-142345-99999999999.log",  // pid 位數超出 DWORD
      "crash--142345-1.log",
      ".log",
      "",
    };
    for (const char* name : rejected) {
      QVERIFY2(!parseCrashFileName(name).has_value(), name);
    }
  }
```

- [ ] **Step 3: 跑測試確認它失敗**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel --target test_crash_report'
```

Expected: FAIL，`unresolved external symbol` 指向 `l2m::crashFileName`、`l2m::writeCrashFileName`、`l2m::parseCrashFileName`。

- [ ] **Step 4: 寫實作**

在 `src/core/crash_report.cpp` 的匿名命名空間裡，`appendLiteral` 之後加入：

```cpp
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
  return !text.empty() &&
         std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
}

// 不檢查溢位，呼叫端負責先限制位數
unsigned long long toULL(std::string_view text) {
  unsigned long long value = 0;
  for (const char c : text) value = value * 10 + static_cast<unsigned long long>(c - '0');
  return value;
}
```

在命名空間外，`formatFrame` 之後加入：

```cpp
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

  std::string_view middle =
    name.substr(prefix.size(), name.size() - prefix.size() - kSuffix.size());

  // 版面固定：8 碼日期 '-' 6 碼時間 '-' pid
  if (middle.size() < 8 + 1 + 6 + 1 + 1) return std::nullopt;
  if (middle[8] != '-' || middle[15] != '-') return std::nullopt;

  const std::string_view date = middle.substr(0, 8);
  const std::string_view time = middle.substr(9, 6);
  const std::string_view pid = middle.substr(16);
  if (!allDigits(date) || !allDigits(time) || !allDigits(pid)) return std::nullopt;
  // DWORD 上限是 4294967295，十位數。超出就不是我們寫的檔案
  if (pid.size() > 10) return std::nullopt;

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
```

- [ ] **Step 5: 跑測試確認通過**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel --target test_crash_report'
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"; ctest --test-dir build/rel -R test_crash_report --output-on-failure
```

Expected: PASS，14 個 slot 全綠。

- [ ] **Step 6: Commit**

```bash
git add src/core/crash_report.h src/core/crash_report.cpp tests/test_crash_report.cpp
git commit -m "新增：crash 檔名的組裝與解析"
```

---

## Task 3: 保留策略與錯誤碼文字

**Files:**
- Modify: `src/core/crash_report.h`, `src/core/crash_report.cpp`
- Modify: `tests/test_crash_report.cpp`

**Interfaces:**
- Consumes: Task 2 的 `parseCrashFileName`
- Produces:
  - `std::optional<std::string> l2m::latestCrashFile(const std::vector<std::string>&)`
  - `std::vector<std::string> l2m::expiredCrashFiles(const std::vector<std::string>&, int keepCount)`
  - `const char* l2m::crashReasonText(unsigned long code)`
  - `inline constexpr int l2m::kCrashKeepCount = 20;`

- [ ] **Step 1: 擴充標頭**

在 `src/core/crash_report.h` 的 `parseCrashFileName` 宣告之後加入：

```cpp
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
```

- [ ] **Step 2: 寫失敗的測試**

在 `tests/test_crash_report.cpp` 的 `crashNameRejectsForeignNames()` 之後加入：

```cpp
  // 字典序最大的那一份就是最新的
  void latestPicksNewest() {
    const std::vector<std::string> names = {
      "crash-20260830-090000-1.log",
      "crash-20260831-142345-12345.log",
      "crash-20260831-010000-2.log",
      "live2d_mate-2026-08-31.log",  // 一般 log，不參與
      "readme.txt",
    };
    QVERIFY(latestCrashFile(names).has_value());
    QCOMPARE(*latestCrashFile(names), std::string("crash-20260831-142345-12345.log"));
    QVERIFY(!latestCrashFile({}).has_value());
    QVERIFY(!latestCrashFile({"readme.txt"}).has_value());
  }

  // 不到保留份數就一個都不刪
  void expiredKeepsEverythingUnderLimit() {
    std::vector<std::string> names;
    for (int i = 1; i <= 20; ++i) names.push_back(crashFileName({2026, 8, 1, 0, 0, 0, (unsigned long)i}));
    QVERIFY(expiredCrashFiles(names, 20).empty());
  }

  // 超過就刪最舊的那些，且回傳保持輸入順序
  void expiredDropsOldestBeyondLimit() {
    const std::vector<std::string> names = {
      "crash-20260801-000000-1.log",  // 最舊，要刪
      "crash-20260831-000000-3.log",
      "crash-20260815-000000-2.log",  // 次舊，要刪
      "crash-20260901-000000-4.log",
    };
    const auto expired = expiredCrashFiles(names, 2);
    QCOMPARE(expired.size(), size_t(2));
    QCOMPARE(expired[0], std::string("crash-20260801-000000-1.log"));
    QCOMPARE(expired[1], std::string("crash-20260815-000000-2.log"));
  }

  // 一般 log 與外來檔案永遠不進刪除清單 —— 兩種檔案住同一個資料夾，
  // 認錯就是拿 crash 的保留規則去砍一般 log
  void expiredIgnoresForeignFiles() {
    const std::vector<std::string> names = {
      "live2d_mate-2026-08-31.log",
      "live2d_mate-2026-08-30.log",
      "readme.txt",
      "crash-20260801-000000-1.log",
      "crash-20260831-000000-2.log",
    };
    const auto expired = expiredCrashFiles(names, 1);
    QCOMPARE(expired.size(), size_t(1));
    QCOMPARE(expired[0], std::string("crash-20260801-000000-1.log"));
  }

  // keepCount 非正數是設定錯誤，寧可一個都不刪也不要把目錄清空
  void expiredRefusesNonPositiveKeepCount() {
    const std::vector<std::string> names = {"crash-20260801-000000-1.log"};
    QVERIFY(expiredCrashFiles(names, 0).empty());
    QVERIFY(expiredCrashFiles(names, -1).empty());
  }

  // 三個歷史現場的錯誤碼都要認得（見設計文件 §1）
  void reasonTextCoversHistoricalCodes() {
    QCOMPARE(std::string(crashReasonText(0xC0000005)), std::string("ACCESS_VIOLATION"));
    QCOMPARE(std::string(crashReasonText(0xC0000409)), std::string("STACK_BUFFER_OVERRUN"));
    QCOMPARE(std::string(crashReasonText(0xC00000FD)), std::string("STACK_OVERFLOW"));
    QCOMPARE(std::string(crashReasonText(0xE06D7363)), std::string("CPP_EXCEPTION"));
  }

  // 認不得的碼要有退路 —— 報告裡的 0x 數字本身仍然有用，
  // 不能因為查不到名字就整行不印
  void reasonTextFallsBackToUnknown() {
    QCOMPARE(std::string(crashReasonText(0x12345678)), std::string("UNKNOWN"));
    QCOMPARE(std::string(crashReasonText(0)), std::string("UNKNOWN"));
  }
```

- [ ] **Step 3: 跑測試確認它失敗**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel --target test_crash_report'
```

Expected: FAIL，`unresolved external symbol` 指向 `l2m::latestCrashFile`、`l2m::expiredCrashFiles`、`l2m::crashReasonText`。

- [ ] **Step 4: 寫實作**

在 `src/core/crash_report.cpp` 頂端補 `#include <set>`，並在檔案尾端（`parseCrashFileName` 之後）加入：

```cpp
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
    case 0xC0000005: return "ACCESS_VIOLATION";
    case 0xC0000006: return "IN_PAGE_ERROR";
    case 0xC000001D: return "ILLEGAL_INSTRUCTION";
    case 0xC0000025: return "NONCONTINUABLE_EXCEPTION";
    case 0xC0000026: return "INVALID_DISPOSITION";
    case 0xC000008C: return "ARRAY_BOUNDS_EXCEEDED";
    case 0xC000008E: return "FLT_DIVIDE_BY_ZERO";
    case 0xC0000094: return "INT_DIVIDE_BY_ZERO";
    case 0xC0000096: return "PRIV_INSTRUCTION";
    case 0xC00000FD: return "STACK_OVERFLOW";
    // MSVC 的 __fastfail 用這個碼（terminate -> abort 走的路）。
    // 三個歷史現場有兩個死在這裡，見設計文件 §1
    case 0xC0000409: return "STACK_BUFFER_OVERRUN";
    case 0xC0000374: return "HEAP_CORRUPTION";
    // MSVC C++ 例外的 SEH 編碼（'msc' | 0xE0000000）
    case 0xE06D7363: return "CPP_EXCEPTION";
    case 0x80000003: return "BREAKPOINT";
    default: return "UNKNOWN";
  }
}
```

- [ ] **Step 5: 跑測試確認通過**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel --target test_crash_report'
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"; ctest --test-dir build/rel -R test_crash_report --output-on-failure
```

Expected: PASS，21 個 slot 全綠。

- [ ] **Step 6: Commit**

```bash
git add src/core/crash_report.h src/core/crash_report.cpp tests/test_crash_report.cpp
git commit -m "新增：crash 檔的保留策略與錯誤碼文字"
```

---

## Task 4: handler 骨架、安裝點與 `L2M_CRASH_TEST`

這一任務的可驗收成果：**五種死法各觸發一次，`logs/` 都會出現一份含表頭的 `crash-*.log`**（還沒有 stack 段，那是 Task 5）。

**Files:**
- Create: `src/platform/crash_handler.h`, `src/platform/crash_handler_win.cpp`, `src/platform/crash_handler_mac.mm`
- Modify: `CMakeLists.txt:435-444`（`target_sources` 的 WIN32 與 APPLE 兩段）
- Modify: `src/app/main.cpp`（第 99 行 `LoggingGuard` 之後、第 103 行 `--mcp-stdio` 分流之前；以及第 588 行 `L2M_SAY` 區塊之後）

**Interfaces:**
- Consumes: Task 1–3 的 `l2m::kMaxFrames`、`formatFrame`、`CrashStamp`、`writeCrashFileName`、`crashReasonText`
- Produces:
  - `std::filesystem::path l2m::platform::appDataDirFromEnv()`
  - `void l2m::platform::installCrashHandler(const std::filesystem::path& crashDir, const char* mode)`
  - `void l2m::platform::triggerCrashTestFromEnv()`

- [ ] **Step 1: 建立介面標頭**

`src/platform/crash_handler.h`：

```cpp
#pragma once

// 行程異常結束時，把呼叫堆疊寫進 %APPDATA%/live2d_mate/logs/crash-*.log。
// 設計全文：docs/superpowers/specs/2026-08-31-crash-stack-logging-design.md
//
// **四個進場點，缺一個就有抓不到的死法：**
//   SetUnhandledExceptionFilter    0xC0000005 存取違規、0xC00000FD 堆疊溢位…
//   std::set_terminate             未捕捉的 C++ 例外。**0xC0000409 只有這裡抓得到** ——
//                                  那是 MSVC 的 __fastfail（terminate -> abort 走的路），
//                                  它刻意繞過 vectored handler 與 unhandled exception
//                                  filter，直接交給 WER。本專案三個歷史現場有兩個死在這裡。
//   _set_purecall_handler          建構／解構期間呼叫純虛擬函式
//   _set_invalid_parameter_handler CRT 參數驗證失敗（要搭 _CrtSetReportMode，
//                                  否則 Debug 建置會先彈斷言對話框把行程卡住）
//
// 刻意**不用** AddVectoredExceptionHandler：它會在每一個 first-chance 例外被呼叫，
// 包括正常被 catch 掉的 C++ throw，以及 DirectWrite 字型 fallback 那類 Windows 內部
// 例外 —— 成本高、雜訊大，而且會把正常運作誤記成當機。
//
// **terminate handler 在 MSVC 上是 per-thread 的。** 主執行緒設了不代表別條有。
// 本專案三條執行緒（見 CLAUDE.md「執行緒模型」）：GUI 完整受保護；miniaudio 即時執行緒
// 依既有鐵律不配置不上鎖不碰 Qt，本來就不該有例外；httplib worker pool 由
// mcp_http_server.cpp 的 set_exception_handler 補上。
//
// **寫檔分兩階段。** handler 絕對不能走 spdlog 那條路：DailyRotatingFileSink 有
// std::mutex，spdlog 格式化訊息又會配置記憶體 —— 崩在那個鎖上或崩在 heap 損毀上時，
// 「寫 log」這個動作本身就會二次死掉。所以：
//   第一階段：只用不配置、不上鎖拿得到的東西（CaptureStackBackTrace /
//             RtlVirtualUnwind、安裝時快照的 exe 基底、WriteFile），寫完立刻
//             FlushFileBuffers。**光這些就足以離線精確還原到 file:line。**
//   第二階段：DbgHelp 符號化、模組名、C++ 例外的 what()。會配置、會上鎖、可能失敗，
//             但失敗只損失好看的那半邊。
//
// **第一階段的禁用清單**（違反任何一條，「當機了卻沒有報告」就會變成常態）：
//   不呼叫 Qt、不 new/malloc、不上任何鎖、不用 CRT stdio（fprintf 會配置也會上鎖）、
//   不碰 std::string、**連 GetModuleHandleEx 都不碰** —— 它會取 loader lock，
//   而崩在 loader lock 上時那一下就是死鎖。模組基底改成安裝時快照一次。
//
// **已知限制**（寫在這裡，免得日後有人以為是 bug）：
//   · 只抓崩潰那一條執行緒的堆疊。要抓另外兩條得 SuspendThread + StackWalk64，
//     在已經不健康的行程裡風險大過收穫。
//   · **x64 限定** —— SEH 路徑的展開靠 .pdata 的 unwind table，32 位元 x86 沒有，
//     那邊才非得用 StackWalk64。真要出 32 位元版時這一段得重寫。
//   · 第二階段可能靜靜失敗。那時 crash 檔只有第一階段的內容，仍可離線還原，但要動手。
//   · 精簡 pdb 沒有行號，要行號得抓 release 另掛的完整 pdb。
//   · _set_invalid_parameter_handler 在 release 建置拿不到 expression／function／
//     file／line（CRT 不帶那些 debug 字串），只會是一堆 nullptr。堆疊仍然完整。
//
// **crashDir 必須在安裝時就備妥**：安裝點排在 main() 的 --mcp-stdio 分流之前
//（三種行程才都受保護），那時 QApplication 還沒建、setApplicationName 還沒跑，
// QStandardPaths 查不到路徑。所以路徑由 appDataDirFromEnv() 手刻讀 %APPDATA%
// 算出來 —— 這跟 main.cpp 的 readDisableHardwareAcceleration() 為了搶在
// QApplication 之前而手刻 yyjson 讀一次是同一種**刻意的重複**。

#include <filesystem>

namespace l2m {
namespace platform {

// 手刻算出 %APPDATA%/live2d_mate。QStandardPaths 用不了的時機專用
//（macOS 回 ~/Library/Application Support/live2d_mate）。算不出來回空路徑
std::filesystem::path appDataDirFromEnv();

// 安裝四個進場點。crashDir 不存在會嘗試建立；建不起來仍然安裝
//（handler 有 %TEMP% 退路）。重複呼叫只有第一次生效。
// mode 原樣寫進報告的 process 那一行："main" / "mcp-stdio" / "splash"
void installCrashHandler(const std::filesystem::path& crashDir, const char* mode);

// L2M_CRASH_TEST 的觸發：av / terminate / purecall / invalidparam / stackoverflow。
// 沒設或認不得就什麼都不做（認不得時印一行 qWarning）。
//
// 這是驗證 handler 唯一可行的手段 —— 「守衛空間夠不夠」「重入防護有沒有效」
// 「精簡 pdb 解不解得出函式名」都只有真的崩一次才知道。**release 版刻意留著**：
// 現場排查時「使用者那台機器上 handler 到底有沒有在運作」是會真的遇到的問題。
void triggerCrashTestFromEnv();

}  // namespace platform
}  // namespace l2m
```

- [ ] **Step 2: 建立 macOS 空殼**

`src/platform/crash_handler_mac.mm`：

```cpp
#include "crash_handler.h"

#include <cstdlib>

namespace l2m {
namespace platform {

std::filesystem::path appDataDirFromEnv() {
  const char* home = std::getenv("HOME");
  if (!home || !*home) return {};
  return std::filesystem::path(home) / "Library" / "Application Support" / "live2d_mate";
}

// macOS 的 install 版面（資源要進 Contents/Resources）還沒接，
// 所以 crash 報告也一併留到那時候一起做。空殼保證 main.cpp 只有一份程式碼路徑
void installCrashHandler(const std::filesystem::path&, const char*) {}

void triggerCrashTestFromEnv() {}

}  // namespace platform
}  // namespace l2m
```

- [ ] **Step 3: 建立 Windows 實作（本任務只做到表頭）**

`src/platform/crash_handler_win.cpp`：

```cpp
#include "crash_handler.h"

#include <windows.h>

#include <QDebug>

#include <csignal>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>

#include "core/crash_report.h"

namespace l2m {
namespace platform {
namespace {

// 安裝時一次算好、之後只讀。handler 裡不能配置記憶體，
// 所以路徑存成固定寬度的寬字元陣列而不是 std::wstring
wchar_t g_crashDir[MAX_PATH] = L"";
char g_mode[16] = "main";

// exe 的基底與大小。**只快照自己這一個模組**：EnumProcessModules 與
// GetModuleHandleEx 都會取 loader lock，崩在 loader lock 上時那一下就是死鎖。
// 其他模組（Qt、系統 DLL）反正沒有 pdb，留到第二階段補名字就好
unsigned long long g_imageBase = 0;
unsigned long long g_imageSize = 0;

// 重入防護。第二階段本來就是「可能會死」的那半邊，而它一死就會再進 handler
// 一次，變成無窮遞迴。第二次進入直接 TerminateProcess，不搶檔案、不遞迴
LONG g_inHandler = 0;

// 第一階段的輸出緩衝。static 是刻意的：當機時堆疊可能已經爆了（0xC00000FD），
// 一個 4 KB 的區域變數自己就會再爆一次
char g_buffer[4096];
size_t g_used = 0;

void reset() { g_used = 0; }

void put(const char* text) {
  const size_t n = std::strlen(text);
  if (g_used + n >= sizeof(g_buffer)) return;
  std::memcpy(g_buffer + g_used, text, n);
  g_used += n;
}

void putHex(unsigned long long value, int digits) {
  const size_t n = writeHex(g_buffer + g_used, sizeof(g_buffer) - g_used - 1, value, digits);
  g_used += n;
}

void putDec(unsigned long value) {
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  if (g_used + static_cast<size_t>(n) >= sizeof(g_buffer)) return;
  for (int i = n - 1; i >= 0; --i) g_buffer[g_used++] = tmp[i];
}

CrashStamp nowStamp() {
  SYSTEMTIME t{};
  ::GetLocalTime(&t);
  CrashStamp stamp;
  stamp.year = t.wYear;
  stamp.month = t.wMonth;
  stamp.day = t.wDay;
  stamp.hour = t.wHour;
  stamp.minute = t.wMinute;
  stamp.second = t.wSecond;
  stamp.pid = ::GetCurrentProcessId();
  return stamp;
}

// 開 crash 檔。主路徑失敗（磁碟滿、權限、目錄被刪）就試一次 %TEMP%，
// 再失敗就放棄 —— 當機路徑上三次以上的重試沒有意義
HANDLE openCrashFile(const CrashStamp& stamp) {
  char name[64];
  const size_t n = writeCrashFileName(name, sizeof(name), stamp);
  if (n == 0) return INVALID_HANDLE_VALUE;

  wchar_t wname[64];
  for (size_t i = 0; i <= n; ++i) wname[i] = static_cast<wchar_t>(name[i]);

  wchar_t full[MAX_PATH * 2];
  const auto tryOpen = [&](const wchar_t* dir) -> HANDLE {
    if (!dir || !*dir) return INVALID_HANDLE_VALUE;
    full[0] = L'\0';
    ::lstrcpynW(full, dir, MAX_PATH);
    ::lstrcatW(full, L"\\");
    ::lstrcatW(full, wname);
    return ::CreateFileW(full, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                         FILE_ATTRIBUTE_NORMAL, nullptr);
  };

  HANDLE file = tryOpen(g_crashDir);
  if (file != INVALID_HANDLE_VALUE) return file;

  wchar_t temp[MAX_PATH] = L"";
  if (::GetEnvironmentVariableW(L"TEMP", temp, MAX_PATH) == 0) return INVALID_HANDLE_VALUE;
  return tryOpen(temp);
}

void flushTo(HANDLE file) {
  if (file == INVALID_HANDLE_VALUE || g_used == 0) return;
  DWORD written = 0;
  ::WriteFile(file, g_buffer, static_cast<DWORD>(g_used), &written, nullptr);
  reset();
}

// 第一階段的表頭。Task 5 會在這之後補 stack 段
void writeHeader(HANDLE file, const CrashStamp& stamp, unsigned long code, const char* detail) {
  reset();
  put("=== live2d_mate crash ===\r\n");
  put("version : " L2M_APP_VERSION "\r\n");
  put("process : live2d_mate.exe  pid ");
  putDec(stamp.pid);
  put("  mode=");
  put(g_mode);
  put("\r\n");
  put("time    : ");
  putDec(static_cast<unsigned long>(stamp.year));
  put("-");
  putDec(static_cast<unsigned long>(stamp.month));
  put("-");
  putDec(static_cast<unsigned long>(stamp.day));
  put(" ");
  putDec(static_cast<unsigned long>(stamp.hour));
  put(":");
  putDec(static_cast<unsigned long>(stamp.minute));
  put(":");
  putDec(static_cast<unsigned long>(stamp.second));
  put("\r\n");
  put("reason  : 0x");
  putHex(code, 8);
  put(" ");
  put(crashReasonText(code));
  if (detail && *detail) {
    put(" (");
    put(detail);
    put(")");
  }
  put("\r\n");
  put("thread  : ");
  putDec(::GetCurrentThreadId());
  put("\r\n");
  put("image   : live2d_mate.exe base=0x");
  putHex(g_imageBase, 16);
  put(" size=0x");
  putHex(g_imageSize, 8);
  put("\r\n");
  flushTo(file);
  ::FlushFileBuffers(file);
}

// 所有進場點的共同出口。detail 是給 terminate／invalidparam 的補充字串
void report(unsigned long code, const char* detail) {
  // 重入＝第二階段自己死了，或另一條執行緒同時崩。直接結束，不搶檔案
  if (::InterlockedCompareExchange(&g_inHandler, 1, 0) != 0) {
    ::TerminateProcess(::GetCurrentProcess(), code);
  }

  const CrashStamp stamp = nowStamp();
  HANDLE file = openCrashFile(stamp);
  writeHeader(file, stamp, code, detail);
  if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);

  // ExitProcess 會跑 DLL detach 與 atexit，在已損毀的狀態下可能再死一次或卡住。
  // 跳過 LoggingGuard 解構的 flush 沒有損失 —— DailyRotatingFileSink 每則都
  // 已經 flush 過了（見 app/logging.cpp:134）
  ::TerminateProcess(::GetCurrentProcess(), code);
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* info) {
  report(info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0, nullptr);
  return EXCEPTION_EXECUTE_HANDLER;  // 到不了，report 不會回來
}

void onTerminate() { report(0xC0000409, "std::terminate"); }

void onPureCall() { report(0xC0000025, "purecall"); }

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
  // 這四個參數在 release 建置一律是 nullptr（CRT 不帶 debug 字串），
  // 所以刻意不去讀它們 —— 堆疊本來就比它們有用
  report(0xC0000409, "invalid CRT parameter");
}

}  // namespace

std::filesystem::path appDataDirFromEnv() {
  wchar_t appData[MAX_PATH] = L"";
  if (::GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH) == 0) return {};
  return std::filesystem::path(appData) / L"live2d_mate";
}

void installCrashHandler(const std::filesystem::path& crashDir, const char* mode) {
  static bool installed = false;
  if (installed) return;
  installed = true;

  if (mode && *mode) ::lstrcpynA(g_mode, mode, sizeof(g_mode));

  std::error_code ec;
  std::filesystem::create_directories(crashDir, ec);
  const std::wstring dir = crashDir.wstring();
  if (!dir.empty() && dir.size() < MAX_PATH) ::lstrcpynW(g_crashDir, dir.c_str(), MAX_PATH);

  // exe 的基底與 SizeOfImage 快照一次。exe 不會被卸載或搬移，所以永遠有效
  if (HMODULE self = ::GetModuleHandleW(nullptr)) {
    g_imageBase = reinterpret_cast<unsigned long long>(self);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(self);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(reinterpret_cast<char*>(self) + dos->e_lfanew);
    if (nt->Signature == IMAGE_NT_SIGNATURE) g_imageSize = nt->OptionalHeader.SizeOfImage;
  }

  // 堆疊溢位（0xC00000FD）進到 filter 時堆疊已經爆了，handler 自己沒空間跑。
  // 預留 64 KB 守衛空間，否則這一類當機**永遠寫不出報告**
  ULONG guard = 64 * 1024;
  ::SetThreadStackGuarantee(&guard);

  ::SetUnhandledExceptionFilter(&onUnhandledException);
  std::set_terminate(&onTerminate);
  _set_purecall_handler(&onPureCall);
  _set_invalid_parameter_handler(&onInvalidParameter);
  // 沒有這一行的話，Debug 建置會先彈 CRT 斷言對話框把行程卡住，
  // 我們的 handler 永遠等不到
  _CrtSetReportMode(_CRT_ASSERT, 0);

  qInfo() << "[crash] 當機報告已安裝，目錄:" << QString::fromStdWString(g_crashDir);
}

void triggerCrashTestFromEnv() {
  char value[32] = "";
  if (::GetEnvironmentVariableA("L2M_CRASH_TEST", value, sizeof(value)) == 0) return;

  qWarning() << "[crash] L2M_CRASH_TEST=" << value << "：以下是刻意觸發的當機，不是真的 bug";

  if (std::strcmp(value, "av") == 0) {
    volatile int* p = reinterpret_cast<volatile int*>(8);
    *p = 1;
  } else if (std::strcmp(value, "terminate") == 0) {
    throw std::runtime_error("L2M_CRASH_TEST=terminate");
  } else if (std::strcmp(value, "purecall") == 0) {
    struct Base {
      Base() { callIt(); }  // 建構期間呼叫純虛擬 -> purecall handler
      virtual void callIt() = 0;
    };
    struct Derived : Base {
      void callIt() override {}
    };
    Derived d;
    (void)d;
  } else if (std::strcmp(value, "invalidparam") == 0) {
    // 對已關閉的 fd 呼叫 -> CRT 參數驗證失敗
    ::_close(9999);
  } else if (std::strcmp(value, "stackoverflow") == 0) {
    struct Recurse {
      static int go(int n) {
        volatile char pad[4096];
        pad[0] = static_cast<char>(n);
        return go(n + 1) + pad[0];
      }
    };
    (void)Recurse::go(0);
  } else {
    qWarning() << "[crash] L2M_CRASH_TEST 認不得的值:" << value
               << "（可用：av / terminate / purecall / invalidparam / stackoverflow）";
  }
}

}  // namespace platform
}  // namespace l2m
```

`_close` 需要 `#include <io.h>`，加在 `<new>` 之後。

- [ ] **Step 4: 接上 CMake**

`CMakeLists.txt:435` 的 WIN32 那一組 `target_sources` 加入 `src/platform/crash_handler_win.cpp`；
`CMakeLists.txt:441` 的 APPLE 那一組加入 `src/platform/crash_handler_mac.mm`。

- [ ] **Step 5: 接上 main.cpp**

`src/app/main.cpp` 的 include 區加入：

```cpp
#include "../platform/crash_handler.h"
```

在第 99 行 `const l2m::LoggingGuard logging;` 之後、第 103 行的 `--mcp-stdio` 判斷之前插入：

```cpp
  // 當機報告：安裝點必須在 --mcp-stdio 分流「之前」，三種行程才都受保護。
  // 代價是那時 QApplication 還沒建、setApplicationName 還沒跑，QStandardPaths
  // 查不到路徑 —— 所以目錄由 appDataDirFromEnv() 手刻讀 %APPDATA% 算出來。
  // 這跟下面 readDisableHardwareAcceleration() 手刻 yyjson 讀一次是同一種
  // 刻意的重複，理由見 platform/crash_handler.h。
  const char* crashMode = "main";
  if (argc >= 2 && std::strcmp(argv[1], "--mcp-stdio") == 0) {
    crashMode = "mcp-stdio";
  } else if (argc >= 2 && std::strcmp(argv[1], l2m::kSplashFlag) == 0) {
    crashMode = "splash";
  }
  l2m::platform::installCrashHandler(l2m::platform::appDataDirFromEnv() / "logs", crashMode);
```

在第 602 行（`L2M_SAY` 的 `if` 區塊結尾 `}`）之後插入：

```cpp
  // 診斷用：設了 L2M_CRASH_TEST 就在啟動 3 秒後故意崩一次，驗證 crash handler
  //（與 L2M_SAY／L2M_PROFILE 同一套慣例，理由見 platform/crash_handler.h）
  if (!qEnvironmentVariable("L2M_CRASH_TEST").isEmpty()) {
    QTimer::singleShot(3000, [] { l2m::platform::triggerCrashTestFromEnv(); });
  }
```

- [ ] **Step 6: 建置**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel'
```

Expected: 建置成功。若出現 `LNK1168`，先關掉正在跑的桌寵（可能是 `build/Desktop_Qt_6_11_2_MSVC2022_64bit_RelWithDebInfo/` 那棵），**並把 `build/rel/live2d_mate.exe` 刪掉再建**（連結失敗那次仍會更新 mtime，ninja 下次會謊報 "no work to do"）。

- [ ] **Step 7: 五種死法各驗一次**

```powershell
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
foreach ($m in @("av","terminate","purecall","invalidparam","stackoverflow")) {
  $env:L2M_CRASH_TEST = $m
  Start-Process -FilePath "build\rel\live2d_mate.exe" -Wait
}
Remove-Item Env:\L2M_CRASH_TEST
Get-ChildItem "$env:APPDATA\live2d_mate\logs\crash-*.log" | Select-Object Name, Length
```

Expected: **五個 crash 檔**，每一份都有 `=== live2d_mate crash ===` 表頭、正確的 `reason`（`av` → `0xC0000005 ACCESS_VIOLATION`、`stackoverflow` → `0xC00000FD STACK_OVERFLOW`、其餘三種 → `0xC0000409` 或 `0xC0000025`），以及非零的 `image base=`。

**`stackoverflow` 那一份是這一步的重點**——它證明 `SetThreadStackGuarantee` 真的留到了空間。沒產出檔案就是守衛空間不足，把 64 KB 調大再試。

- [ ] **Step 8: Commit**

```bash
git add src/platform/crash_handler.h src/platform/crash_handler_win.cpp src/platform/crash_handler_mac.mm src/app/main.cpp CMakeLists.txt
git commit -m "新增：當機 handler 的四個進場點與第一階段表頭"
```

---

## Task 5: 第一階段的堆疊擷取

可驗收成果：**五份 crash 檔都多出 `stack :` 段，且自己的 frame 印成 `live2d_mate.exe+0xRVA`。**

**Files:**
- Modify: `src/platform/crash_handler_win.cpp`

**Interfaces:**
- Consumes: `l2m::kMaxFrames`、`l2m::formatFrame`
- Produces: `report()` 多吃一個 `CONTEXT*` 參數（`nullptr` 表示走 `CaptureStackBackTrace`）

- [ ] **Step 1: 加入擷取與輸出**

在 `crash_handler_win.cpp` 的匿名命名空間裡，`writeHeader` 之前加入：

```cpp
void* g_frames[kMaxFrames];

// SEH 路徑：從當機瞬間的 CONTEXT 開始展開。
//
// **不用 StackWalk64**：那支會配置記憶體、會進 DbgHelp 的內部鎖，不符合第一階段
// 「一定寫得出來」的要求。RtlLookupFunctionEntry + RtlVirtualUnwind 正是
// CaptureStackBackTrace 內部用的東西，同樣不配置不上鎖。
//
// x64 限定：32 位元 x86 沒有 unwind table（.pdata），那邊才非得靠 StackWalk64。
int captureFromContext(const CONTEXT& seed) {
  CONTEXT ctx = seed;  // **一定要複製**：RtlVirtualUnwind 會就地改寫 CONTEXT
  UNWIND_HISTORY_TABLE history{};
  int n = 0;
  while (n < kMaxFrames && ctx.Rip != 0) {
    g_frames[n++] = reinterpret_cast<void*>(ctx.Rip);
    DWORD64 imageBase = 0;
    PRUNTIME_FUNCTION entry = ::RtlLookupFunctionEntry(ctx.Rip, &imageBase, &history);
    if (!entry) {
      // 葉函式沒有 unwind 資料：返回位址就躺在堆疊頂端，手動退一格再繼續
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += sizeof(DWORD64);
      continue;
    }
    PVOID handlerData = nullptr;
    DWORD64 establisher = 0;
    ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, entry, &ctx, &handlerData,
                       &establisher, nullptr);
  }
  return n;
}

void writeStack(HANDLE file, int count) {
  reset();
  put("stack   :\r\n");
  for (int i = 0; i < count; ++i) {
    put("  #");
    if (i < 10) put("0");
    putDec(static_cast<unsigned long>(i));
    put(" ");
    const size_t n =
      formatFrame(g_buffer + g_used, sizeof(g_buffer) - g_used - 3,
                  reinterpret_cast<unsigned long long>(g_frames[i]), g_imageBase, g_imageSize,
                  "live2d_mate.exe");
    // 緩衝不夠就整行跳過，不寫半行
    if (n == 0) break;
    g_used += n;
    put("\r\n");
    // 一行約 40 位元組，緩衝快滿就先吐出去
    if (g_used + 64 >= sizeof(g_buffer)) flushTo(file);
  }
  flushTo(file);
  ::FlushFileBuffers(file);
}
```

- [ ] **Step 2: 讓 `report` 收 CONTEXT 並輸出堆疊**

把 `report` 改成：

```cpp
// seed 為 nullptr ＝ 當下這條堆疊就是現場（terminate / purecall / invalidparam），
// 用 CaptureStackBackTrace；有值 ＝ SEH，要從當機瞬間的暫存器狀態展開
void report(unsigned long code, const char* detail, const CONTEXT* seed) {
  if (::InterlockedCompareExchange(&g_inHandler, 1, 0) != 0) {
    ::TerminateProcess(::GetCurrentProcess(), code);
  }

  const int count = seed ? captureFromContext(*seed)
                         : static_cast<int>(::CaptureStackBackTrace(0, kMaxFrames, g_frames,
                                                                    nullptr));

  const CrashStamp stamp = nowStamp();
  HANDLE file = openCrashFile(stamp);
  writeHeader(file, stamp, code, detail);
  writeStack(file, count);
  if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);

  ::TerminateProcess(::GetCurrentProcess(), code);
}
```

四個進場點對應改成：

```cpp
LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* info) {
  report(info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0, nullptr,
         info ? info->ContextRecord : nullptr);
  return EXCEPTION_EXECUTE_HANDLER;
}

void onTerminate() { report(0xC0000409, "std::terminate", nullptr); }

void onPureCall() { report(0xC0000025, "purecall", nullptr); }

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
  report(0xC0000409, "invalid CRT parameter", nullptr);
}
```

- [ ] **Step 3: 建置並重跑五種死法**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel'
Remove-Item "$env:APPDATA\live2d_mate\logs\crash-*.log"
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
foreach ($m in @("av","terminate","purecall","invalidparam","stackoverflow")) {
  $env:L2M_CRASH_TEST = $m; Start-Process -FilePath "build\rel\live2d_mate.exe" -Wait
}
Remove-Item Env:\L2M_CRASH_TEST
Get-Content (Get-ChildItem "$env:APPDATA\live2d_mate\logs\crash-*.log" | Select-Object -First 1).FullName
```

Expected: 每一份都有 `stack   :` 段、至少 5 層 frame，其中**至少一層**是 `live2d_mate.exe+0x…`（不是純絕對位址）。`av` 那一份的最上層幾個 frame 應該指向 `triggerCrashTestFromEnv` 附近。

- [ ] **Step 4: Commit**

```bash
git add src/platform/crash_handler_win.cpp
git commit -m "新增：當機第一階段的堆疊擷取（CaptureStackBackTrace 與 RtlVirtualUnwind）"
```

---

## Task 6: 第二階段符號化與 pdb 出貨

可驗收成果：**crash 檔多出 `--- symbolized ---` 段，自己的 frame 顯示函式名。**

**Files:**
- Modify: `src/platform/crash_handler_win.cpp`
- Modify: `CMakeLists.txt`（`target_link_libraries(live2d_mate ...)`、新增 `target_link_options`、`install(FILES ...)`）

**Interfaces:**
- Consumes: Task 5 的 `g_frames` / `g_used`
- Produces: 無對外新符號

- [ ] **Step 1: CMake 接上 DbgHelp 與精簡 pdb**

`CMakeLists.txt` 的 `target_link_libraries(live2d_mate PRIVATE ...)` 清單（`Qt${QT_VERSION_MAJOR}::Network` 之後）加入：

```cmake
            $<$<BOOL:${WIN32}>:Dbghelp>
```

在同一個 `if(WIN32)` 區塊內（`install(TARGETS live2d_mate ...)` 之前）加入：

```cmake
    # 精簡符號檔：只有 public symbols（函式名，沒有行號與型別），
    # 實測完整 pdb 是 84–168 MB，這一份約 3–8 MB，隨 exe 出貨負擔得起。
    # 要行號時再抓 release 另掛的完整 pdb 離線解（見設計文件 §9）。
    target_link_options(
      live2d_mate PRIVATE
      "$<$<CONFIG:RelWithDebInfo,Release>:/PDBSTRIPPED:$<TARGET_FILE_DIR:live2d_mate>/live2d_mate.stripped.pdb>"
    )

    # **RENAME 是必要的，不是美觀問題**：DbgHelp 是靠 exe 的 debug directory 裡記的
    # 「pdb 檔名 + GUID + age」去找符號檔的，檔名對不上就根本不會去看它。
    # OPTIONAL 是因為 Debug 建置不產這個檔
    install(
      FILES "$<TARGET_FILE_DIR:live2d_mate>/live2d_mate.stripped.pdb"
      DESTINATION .
      RENAME live2d_mate.pdb
      OPTIONAL)
```

- [ ] **Step 2: 加入符號化**

`crash_handler_win.cpp` 的 include 區，`<windows.h>` 之後加入 `#include <dbghelp.h>`（順序敏感：dbghelp 依賴 windows.h）。

在匿名命名空間裡，`writeStack` 之後加入：

```cpp
// DbgHelp 的初始化。**在安裝時就做，不留到當機當下**：SymInitialize 慢而且會配置。
//
// **一定要覆寫搜尋路徑。** 使用者機器上若設過 _NT_SYMBOL_PATH（裝過 WinDbg 或 VS
// 的人很常見），SymInitialize 會照那個路徑去連 Microsoft 符號伺服器下載 ——
// 在當機當下連網抓幾十 MB，使用者看到的就是「程式卡住幾分鐘然後才消失」。
void initSymbols() {
  ::SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES);
  wchar_t exePath[MAX_PATH] = L"";
  ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  wchar_t* lastSlash = ::wcsrchr(exePath, L'\\');
  if (lastSlash) *lastSlash = L'\0';
  // 第三個參數 fInvadeProcess = FALSE：不要一次載入所有模組的符號，
  // 配合 SYMOPT_DEFERRED_LOADS 讓註冊維持在毫秒等級
  ::SymInitializeW(::GetCurrentProcess(), exePath, FALSE);
}

// 第二階段。會配置記憶體、會上 DbgHelp 的內部鎖、可能整段失敗 ——
// 但第一階段已經 flush 過了，失敗只損失好看的那半邊
void writeSymbols(HANDLE file, int count) {
  if (file == INVALID_HANDLE_VALUE) return;
  const HANDLE process = ::GetCurrentProcess();

  alignas(SYMBOL_INFO) char symbolStorage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
  auto* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);

  reset();
  put("--- symbolized ---\r\n");
  for (int i = 0; i < count; ++i) {
    const DWORD64 address = reinterpret_cast<DWORD64>(g_frames[i]);
    put("  #");
    if (i < 10) put("0");
    putDec(static_cast<unsigned long>(i));
    put(" ");

    std::memset(symbol, 0, sizeof(SYMBOL_INFO));
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (::SymFromAddr(process, address, &displacement, symbol)) {
      put(symbol->Name);
    } else {
      put("(no symbol)");
    }

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    DWORD lineDisplacement = 0;
    if (::SymGetLineFromAddr64(process, address, &lineDisplacement, &line) && line.FileName) {
      put("  ");
      // 只印檔名不印完整路徑：路徑會洩漏建置機器的目錄結構，
      // 而對照原始碼時檔名就夠了
      const char* slash = std::strrchr(line.FileName, '\\');
      put(slash ? slash + 1 : line.FileName);
      put(":");
      putDec(line.LineNumber);
    }
    put("\r\n");
    if (g_used + 512 >= sizeof(g_buffer)) flushTo(file);
  }
  flushTo(file);
  ::FlushFileBuffers(file);
}

// C++ 例外的訊息。**放第二階段**：current_exception 會配置記憶體。
// 但診斷價值極高 —— 0xC0000409 那種現場，what() 那一行往往比整個堆疊還關鍵
void writeExceptionMessage(HANDLE file) {
  if (file == INVALID_HANDLE_VALUE) return;
  const std::exception_ptr current = std::current_exception();
  if (!current) return;
  reset();
  put("--- exception ---\r\n  ");
  // rethrow 一定要完整包住，否則會遞迴回 terminate
  try {
    std::rethrow_exception(current);
  } catch (const std::exception& e) {
    put(e.what());
  } catch (...) {
    put("(non-std exception)");
  }
  put("\r\n");
  flushTo(file);
  ::FlushFileBuffers(file);
}
```

- [ ] **Step 3: 接進 `report` 與 `installCrashHandler`**

`report()` 裡，`writeStack(file, count);` 之後、`CloseHandle` 之前插入：

```cpp
  // 第二階段：到這裡為止的內容已經在磁碟上了，以下失敗都不影響可離線還原的部分
  writeSymbols(file, count);
  writeExceptionMessage(file);
```

`installCrashHandler()` 裡，`SetThreadStackGuarantee` 之後、`SetUnhandledExceptionFilter` 之前插入：

```cpp
  initSymbols();
```

- [ ] **Step 4: 建置並驗證**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel'
Remove-Item "$env:APPDATA\live2d_mate\logs\crash-*.log"
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
$env:L2M_CRASH_TEST = "uncaught"; Start-Process -FilePath "build\rel\live2d_mate.exe" -Wait
Remove-Item Env:\L2M_CRASH_TEST
Get-Content (Get-ChildItem "$env:APPDATA\live2d_mate\logs\crash-*.log" | Select-Object -First 1).FullName
```

Expected：檔案裡同時有 `stack   :`、`--- symbolized ---`（含 `triggerCrashTestFromEnv` 之類的真函式名與 `crash_handler_win.cpp:NNN` 行號，因為 `build/rel` 旁邊就有完整 pdb）、以及 `--- exception ---` 底下的 `L2M_CRASH_TEST=uncaught`。

**一定要用 `uncaught` 而不是 `terminate`**：Task 4 實測發現未捕捉的 `throw` 走的是 unhandled exception filter，兩個模式因此拆開了——`terminate` 直接呼叫 `std::terminate()`，那時沒有例外在飛，`std::current_exception()` 是空的，`--- exception ---` 整段不會出現。拿 `terminate` 驗這一步會白白判定失敗。

若連結時出現 `LNK4075: ignoring /INCREMENTAL due to /PDBSTRIPPED`，在同一個 `target_link_options` 加上 `"$<$<CONFIG:RelWithDebInfo,Release>:/INCREMENTAL:NO>"`。

- [ ] **Step 5: 驗證精簡 pdb 真的產出且明顯較小**

```powershell
Get-ChildItem build\rel\live2d_mate*.pdb | Select-Object Name, @{n='MB';e={[math]::Round($_.Length/1MB,1)}}
```

Expected: `live2d_mate.pdb` 數十 MB、`live2d_mate.stripped.pdb` 個位數 MB。

- [ ] **Step 6: Commit**

```bash
git add src/platform/crash_handler_win.cpp CMakeLists.txt
git commit -m "新增：當機第二階段符號化，並讓精簡 pdb 隨執行檔出貨"
```

---

## Task 7: 補上 httplib worker 的缺口

**Files:**
- Modify: `src/mcp/mcp_http_server.cpp:161`（`set_error_handler` 之後）

**Interfaces:**
- Consumes: 無（透過 `std::rethrow_exception` 讓 terminate handler 接手不適用；這裡是把例外轉成 500 並記錄）
- Produces: 無對外新符號

- [ ] **Step 1: 加入 exception handler**

在 `server_->set_error_handler(...)` 那個 lambda 結尾的 `});` 之後插入：

```cpp
  // **terminate handler 在 MSVC 上是 per-thread 的**，主執行緒設的那一份
  // 蓋不到 httplib 自己建的 worker pool。沒有這一段的話，worker 裡逸出的例外
  // 會走到一個沒有 handler 的 terminate，行程直接消失且什麼都不留
  //（見 platform/crash_handler.h）。
  // httplib 收下例外之後會回 500，所以順帶也讓 client 拿得到錯誤而不是斷線
  server_->set_exception_handler(
    [](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
      std::string what = "unknown exception";
      try {
        std::rethrow_exception(ep);
      } catch (const std::exception& e) {
        what = e.what();
      } catch (...) {
      }
      qWarning() << "[mcp] worker 執行緒逸出例外:" << QString::fromStdString(what)
                 << "path:" << QString::fromStdString(req.path);
      res.status = 500;
      res.set_content("{\"error\":\"Internal error: " + jsonEscape(what) + "\"}",
                      "application/json");
    });
```

- [ ] **Step 2: 建置**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel'
```

Expected: 建置成功。若 `std::exception_ptr` 未宣告，在檔案的 std 標頭區加 `#include <exception>`。

- [ ] **Step 3: 驗證既有 MCP 測試沒有回歸**

```powershell
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"; ctest --test-dir build/rel -R "test_mcp" --output-on-failure
```

Expected: `test_mcp_host`、`test_mcp_origin`、`test_mcp_snippets`、`test_mcp_tools` 全綠。

- [ ] **Step 4: Commit**

```bash
git add src/mcp/mcp_http_server.cpp
git commit -m "修正：httplib worker 執行緒逸出的例外沒有任何記錄"
```

---

## Task 8: config 的 `crash.lastNotified`

**Files:**
- Modify: `src/core/config_schema.h`（`LlmConfig` 之後、`AppConfig` 之前；並在 `AppConfig` 加成員）
- Modify: `src/core/config_schema.cpp`（`parseConfig` 尾端、`serializeConfig` 尾端）
- Modify: `tests/test_config.cpp`

**Interfaces:**
- Consumes: 無
- Produces: `l2m::AppConfig::crash.lastNotified`（`std::string`）

- [ ] **Step 1: 寫失敗的測試**

在 `tests/test_config.cpp` 的類別裡加入：

```cpp
  // crash section 的往返。預設是空字串＝「還沒提示過任何一份」
  void crashLastNotifiedRoundTrips() {
    AppConfig config = defaultConfig();
    QVERIFY(config.crash.lastNotified.empty());
    config.crash.lastNotified = "crash-20260831-142345-12345.log";

    const std::string json = serializeConfig(config);
    auto doc = jsonu::Doc::parse(json);
    QVERIFY(doc.has_value());
    const auto parsed = parseConfig(doc->root());
    QVERIFY(parsed.has_value());
    QCOMPARE(parsed->crash.lastNotified, std::string("crash-20260831-142345-12345.log"));
  }

  // crash 是最後一個 section：鍵順序是既有 config.json 的合約，
  // 插在中間會讓每次重寫都整份 diff
  void crashSectionIsLast() {
    const std::string json = serializeConfig(defaultConfig());
    QVERIFY(json.find("\"crash\"") != std::string::npos);
    QVERIFY(json.find("\"llm\"") < json.find("\"crash\""));
  }
```

- [ ] **Step 2: 跑測試確認它失敗**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel --target test_config'
```

Expected: FAIL，編譯錯誤 `'crash': is not a member of 'l2m::AppConfig'`。

- [ ] **Step 3: 加入 schema**

`src/core/config_schema.h`，在 `struct LlmConfig { … };` 之後加入：

```cpp
// 當機報告的狀態。**只有一個欄位，而且刻意只記檔名不記時間**：
// 「這一份提示過了沒」是唯一需要跨啟動保存的東西，
// 檔名本身就帶時間戳（見 core/crash_report.h）
struct CrashConfig {
  std::string lastNotified;  // 最近一次提示過的 crash 檔名；空＝還沒提示過任何一份
};
```

在 `struct AppConfig` 的 `LlmConfig llm;` 之後加入：

```cpp
  CrashConfig crash;
```

同時把 `serializeConfig` 上方那段註解的最後一句改成：

```cpp
// persona / autonomy / wind / llm / crash 是後來加的 section，**刻意接在最後面**；
// 五者之間的順序（persona → autonomy → wind → llm → crash）依實作先後排定，
// 出貨後即為合約，不能事後對調。
```

- [ ] **Step 4: 加入 parse 與 serialize**

`src/core/config_schema.cpp` 的 `parseConfig` 裡，最後一個 `if (yyjson_val* llm = section(ctx, root, "llm")) { … }` 區塊**之後**加入：

```cpp
  if (yyjson_val* crash = section(ctx, root, "crash")) {
    readStr(ctx, crash, "lastNotified", config.crash.lastNotified, 0, "crash.lastNotified");
  }
```

`serializeConfig` 裡，最後一個 `{ yyjson_mut_val* llm = addSection(doc, root, "llm"); … }` 區塊**之後**加入：

```cpp
  {
    yyjson_mut_val* crash = addSection(doc, root, "crash");
    putStr(doc, crash, "lastNotified", config.crash.lastNotified);
  }
```

- [ ] **Step 5: 跑測試確認通過**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel --target test_config'
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"; ctest --test-dir build/rel -R test_config --output-on-failure
```

Expected: PASS（含既有的 config 測試全綠）。

- [ ] **Step 6: Commit**

```bash
git add src/core/config_schema.h src/core/config_schema.cpp tests/test_config.cpp
git commit -m "新增：config 的 crash.lastNotified"
```

---

## Task 9: 啟動時的清理與系統匣通知

可驗收成果：**故意崩一次之後重新啟動，系統匣跳出通知；再啟動一次不會重複跳。**

**Files:**
- Modify: `src/windows/tray.h`, `src/windows/tray.cpp`
- Modify: `src/app/main.cpp`（`Tray` 建立之後、`L2M_CRASH_TEST` 區塊附近）

**Interfaces:**
- Consumes: `l2m::latestCrashFile`、`l2m::expiredCrashFiles`、`l2m::kCrashKeepCount`、`AppConfig::crash.lastNotified`
- Produces: `void l2m::Tray::notify(const QString& title, const QString& body, std::function<void()> onClicked)`

- [ ] **Step 1: Tray 加上通知能力**

`src/windows/tray.h`，在 `void rebuild();` 之後加入：

```cpp
  // 系統匣氣球通知。**刻意不是 modal 對話框**：目前唯一的呼叫端是
  // 「上次異常結束」的提示，而那一定發生在剛開機的時候 —— 桌寵才剛出現就
  // 跳一個要按確定的視窗太兇。點通知會叫 onClicked（Windows 上使用者忽略
  // 通知是常態，所以那個 callback 不保證會被呼叫）
  void notify(const QString& title, const QString& body, std::function<void()> onClicked);
```

`src/windows/tray.cpp`，在 `Tray::rebuild()` 的實作之後加入：

```cpp
void Tray::notify(const QString& title, const QString& body, std::function<void()> onClicked) {
  // 每次都先斷開上一個：messageClicked 是 icon_ 的訊號，不斷開的話
  // 第二次通知被點到時會把前一次的 callback 也叫一遍
  disconnect(&icon_, &QSystemTrayIcon::messageClicked, nullptr, nullptr);
  if (onClicked) {
    connect(&icon_, &QSystemTrayIcon::messageClicked, this,
            [handler = std::move(onClicked)] { handler(); });
  }
  icon_.showMessage(title, body, QSystemTrayIcon::Warning, 10000);
}
```

`tray.cpp` 若還沒有 `#include <functional>` 就補上。

- [ ] **Step 2: main.cpp 接上掃描、清理與提示**

`src/app/main.cpp` 的 include 區加入：

```cpp
#include <QDesktopServices>
#include <QUrl>

#include "core/config_patch.h"
#include "core/crash_report.h"
```

在第 602 行 `L2M_SAY` 區塊之後（Task 4 加的 `L2M_CRASH_TEST` 區塊之前）插入：

```cpp
  // 上次異常結束的提示與 crash 檔清理。
  //
  // **刻意排在這裡而不是 handler 裡**：當機當下呼叫 UI 在堆疊已經爆掉
  //（0xC00000FD）的情況下很可能二次崩潰，所以提示交給下一次正常啟動的
  // 一般 Qt 程式碼路徑（見 platform/crash_handler.h）。
  {
    const fs::path logsDir = appData / "logs";
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(logsDir, ec)) {
      if (entry.is_regular_file(ec)) names.push_back(entry.path().filename().string());
    }

    // 清理排在提示之前：先算出「最新那一份」再刪，順序反過來會在
    // 剛好累積到第 21 份時把要提示的那一份刪掉
    const auto latest = l2m::latestCrashFile(names);
    for (const std::string& stale : l2m::expiredCrashFiles(names, l2m::kCrashKeepCount)) {
      fs::remove(logsDir / stale, ec);
    }

    if (latest && *latest != store.config().crash.lastNotified) {
      qWarning() << "[crash] 偵測到上次異常結束:" << QString::fromStdString(*latest);
      // stringPatch 是 config_patch.h 既有的單欄位捷徑，逃脫由它負責 ——
      // 手拼 JSON 的話，檔名裡的反斜線會讓整包 patch 解析失敗
      store.patch(l2m::stringPatch("crash", "lastNotified", *latest));
      const QString folder = QString::fromStdString(logsDir.string());
      tray.notify(
        QString::fromStdString(l2m::i18n::translate(controller.uiLocale(), "crash.notifyTitle")),
        QString::fromStdString(l2m::i18n::translate(controller.uiLocale(), "crash.notifyBody")),
        [folder] { QDesktopServices::openUrl(QUrl::fromLocalFile(folder)); });
    }
  }
```

不需要新增任何 patch 輔助函式：`config_patch.h:88` 的 `stringPatch(section, field, value)` 就是為這種單欄位更新準備的，逃脫由它內部的 `jsonEscape` 負責。

- [ ] **Step 3: 加入 i18n 鍵**

在 `i18n/{en,ja,ko,zh-CN,zh-TW}.json` 各加兩個鍵（面向使用者的字串一律英文原文在 en.json）：

| key | en | ja | ko | zh-CN | zh-TW |
|---|---|---|---|---|---|
| `crash.notifyTitle` | `Live2D Mate closed unexpectedly` | `Live2D Mate が予期せず終了しました` | `Live2D Mate가 예기치 않게 종료되었습니다` | `Live2D Mate 上次异常结束` | `Live2D Mate 上次異常結束` |
| `crash.notifyBody` | `A crash report was saved. Click to open the log folder.` | `クラッシュレポートを保存しました。クリックするとログフォルダーを開きます。` | `크래시 보고서를 저장했습니다. 클릭하면 로그 폴더를 엽니다.` | `已保存崩溃报告。点击打开日志文件夹。` | `已儲存當機報告。點擊開啟日誌資料夾。` |

- [ ] **Step 4: 建置並驗證**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel'
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
$env:L2M_CRASH_TEST = "av"; Start-Process -FilePath "build\rel\live2d_mate.exe" -Wait
Remove-Item Env:\L2M_CRASH_TEST
Start-Process -FilePath "build\rel\live2d_mate.exe"
```

Expected：第二次啟動幾秒內系統匣跳出「Live2D Mate 上次異常結束」通知，點下去開啟 `%APPDATA%\live2d_mate\logs`。關掉再開一次**不會**再跳（`crash.lastNotified` 已寫回 config.json，可用 `Select-String '"lastNotified"' "$env:APPDATA\live2d_mate\config.json"` 確認）。

- [ ] **Step 5: Commit**

```bash
git add src/windows/tray.h src/windows/tray.cpp src/app/main.cpp src/core/config_patch.h src/core/config_patch.cpp i18n
git commit -m "新增：啟動時提示上次的當機報告，並清掉過期的 crash 檔"
```

---

## Task 10: 關於分頁的「開啟日誌資料夾」按鈕

**Files:**
- Modify: `src/windows/settings/about_page.ui`, `about_page.h`, `about_page.cpp`
- Modify: `i18n/{en,ja,ko,zh-CN,zh-TW}.json`

**Interfaces:**
- Consumes: `SettingsContext`（既有）
- Produces: 無對外新符號

- [ ] **Step 1: 在 .ui 加一顆按鈕**

`about_page.ui` 的外層是 `QVBoxLayout name="rootLayout"`，項目順序是 `appName` → `tagline` → `versions` → `infoForm`（QFormLayout，程式碼填四列診斷資料）→ `supportText`。把按鈕插在 **`infoForm` 那個 `</item>` 之後、`supportText` 那個 `<item>` 之前**（`about_page.ui:45` 附近）——按鈕就緊接在設定檔／模型資料夾路徑底下，語意上剛好。

逐字插入這一段：

```xml
   <item>
    <layout class="QHBoxLayout" name="logsRow">
     <item>
      <widget class="QPushButton" name="openLogsButton">
       <property name="text">
        <string>settings.about.openLogs</string>
       </property>
      </widget>
     </item>
     <item>
      <spacer name="logsSpacer">
       <property name="orientation">
        <enum>Qt::Horizontal</enum>
       </property>
       <property name="sizeHint" stdset="0">
        <size>
         <width>40</width>
         <height>20</height>
        </size>
       </property>
      </spacer>
     </item>
    </layout>
   </item>
```

spacer 是必要的：沒有它按鈕會被拉滿整列寬度，跟 `supportRow` 裡 `kofiButton` 的作法一致。

**`.ui` 裡不寫註解**（Designer 重存會刪掉），說明寫在 `about_page.h` 的區塊註解裡。`text` 填 key 是本專案的慣例：i18n 是執行期 JSON 沒有 `retranslateUi`，漏寫 `retranslate()` 時畫面上會直接顯示 raw key，跑一次就發現。**行尾必須是 LF**（`.gitattributes` 把 `*.ui` 釘成 LF）。

- [ ] **Step 2: 更新標頭的區塊註解**

`about_page.h` 的區塊註解，把「這頁扛三件事」改成四件，並在 ③ 之後加入：

```
// ④ **開啟日誌資料夾** —— 使用者回報問題時最常需要的一個動作。這顆按鈕不只服務
//    當機報告：一般的 live2d_mate-*.log 與 crash-*.log 住在同一個資料夾，
//    請對方「把 logs 整個壓縮寄過來」時有個地方按。開不起來的退路同 Ko-fi 那顆
//    （複製路徑到剪貼簿 + 狀態列說明），理由一樣：openUrl 失敗時畫面毫無反應。
```

並在「對應的表單是 about_page.ui」那一段的控制項清單裡補上 `openLogsButton`。

- [ ] **Step 3: 接上點擊**

`about_page.cpp` 的建構子裡，`kofiButton` 的 `connect` 之後加入：

```cpp
  connect(ui_->openLogsButton, &QPushButton::clicked, this, [this] {
    const QString dir =
      QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + QStringLiteral("/logs");
    // 同 kofiButton：openUrl 失敗時畫面一點反應都沒有，
    // 退成「複製路徑 + 狀態列說明」才不會變成無聲失敗
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(dir))) return;
    QApplication::clipboard()->setText(dir);
    ctx_.setStatus(tr2("settings.about.openLogsCopied"));
  });
```

`about_page.cpp` 補 `#include <QStandardPaths>`。

在 `AboutPage::retranslate()` 裡加入：

```cpp
  ui_->openLogsButton->setText(tr2("settings.about.openLogs"));
```

- [ ] **Step 4: 加入 i18n 鍵**

| key | en | ja | ko | zh-CN | zh-TW |
|---|---|---|---|---|---|
| `settings.about.openLogs` | `Open log folder` | `ログフォルダーを開く` | `로그 폴더 열기` | `打开日志文件夹` | `開啟日誌資料夾` |
| `settings.about.openLogsCopied` | `Could not open the folder. The path was copied to the clipboard.` | `フォルダーを開けませんでした。パスをクリップボードにコピーしました。` | `폴더를 열 수 없습니다. 경로를 클립보드에 복사했습니다.` | `无法打开文件夹，路径已复制到剪贴板。` | `無法開啟資料夾，路徑已複製到剪貼簿。` |

- [ ] **Step 5: 建置並驗證**

```powershell
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\rel'
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"; Start-Process -FilePath "build\rel\live2d_mate.exe"
```

Expected：系統匣 → 設定 → 關於分頁出現「開啟日誌資料夾」按鈕（**不是** raw key `settings.about.openLogs`；顯示 raw key 就代表 `retranslate()` 漏寫），按下去開啟 `%APPDATA%\live2d_mate\logs`。

- [ ] **Step 6: Commit**

```bash
git add src/windows/settings/about_page.ui src/windows/settings/about_page.h src/windows/settings/about_page.cpp i18n
git commit -m "新增：關於分頁的「開啟日誌資料夾」按鈕"
```

---

## Task 11: release 掛上完整 pdb

**Files:**
- Modify: `.github/workflows/release.yml`

**Interfaces:**
- Consumes: Task 6 產生的 `live2d_mate.pdb`（完整）與已 install 的精簡 pdb
- Produces: release 的獨立資產 `live2d_mate-<version>-pdb.zip`

現況（已讀過）：`Configure` 用 `-G "Visual Studio 17 2022" -A x64 -B build`，`Build` 用 `--config RelWithDebInfo`。CMakeLists 沒有設 `RUNTIME_OUTPUT_DIRECTORY`，所以 **多組態產生器會把輸出放在 `build/RelWithDebInfo/`**（跟本機 Ninja 那棵平放在根目錄不同，別照抄本機路徑）。`Zip` 那一步把 zip 檔名寫進 `$env:GITHUB_ENV` 的 `ASSET`，最後 `gh release create $env:GITHUB_REF_NAME $env:ASSET` 上傳。

- [ ] **Step 1: 在 `Zip` 步驟之後、`actions/upload-artifact` 之前插入 pdb 步驟**

```yaml
      # 完整 pdb 是 84–168 MB，太肥不能塞進主 zip；但少了它 crash 報告就只有函式名
      # 沒有行號。掛成獨立資產：crash 檔記了 RVA 與 PDB GUID/age，抓這一包下來就能用
      # llvm-symbolizer / WinDbg 精確還原到 file:line（見設計文件 §9）。
      # 出貨 zip 裡的是 /PDBSTRIPPED 產的精簡版（已由 install 規則改名成 live2d_mate.pdb），
      # 這裡要的是建置樹裡的**完整**那一份，所以要排除 stripped。
      - name: Package debug symbols
        shell: pwsh
        run: |
          # 多組態產生器的輸出在 build/RelWithDebInfo/；用找的而不是寫死，
          # 免得日後換產生器就靜靜壓出一包空的
          $pdb = Get-ChildItem -Path build -Recurse -Filter live2d_mate.pdb |
                 Where-Object { $_.FullName -notlike "*stripped*" } |
                 Select-Object -First 1
          if (-not $pdb) { throw "建置樹裡找不到完整的 live2d_mate.pdb" }
          $name = if ($env:GITHUB_REF -like "refs/tags/*") { $env:GITHUB_REF_NAME } else { "dev" }
          Compress-Archive -Path $pdb.FullName `
                           -DestinationPath "live2d_mate-$name-pdb.zip"
          "PDB_ASSET=live2d_mate-$name-pdb.zip" | Out-File -Append $env:GITHUB_ENV
```

- [ ] **Step 2: 把 pdb 一起上傳**

`actions/upload-artifact` 那一步的 `path:` 改成兩行：

```yaml
          path: |
            ${{ env.ASSET }}
            ${{ env.PDB_ASSET }}
```

`Publish GitHub Release` 那一步最後一行改成：

```yaml
          gh release create $env:GITHUB_REF_NAME $env:ASSET $env:PDB_ASSET `
            --title $env:GITHUB_REF_NAME --notes $notes
```

- [ ] **Step 3: 在 release notes 說明那一包是什麼**

`$notes` 的 here-string 裡，`### Licensing` 之前加入：

```
          ### Debug symbols

          `live2d_mate-<tag>-pdb.zip` contains the full PDB for this build. You only
          need it to turn the `module+0xRVA` lines of a crash report into exact source
          locations; the shipped archive already resolves function names on its own.
```

- [ ] **Step 4: 驗證 workflow 語法**

```powershell
python -c "import yaml,sys; yaml.safe_load(open('.github/workflows/release.yml', encoding='utf-8')); print('ok')"
```

Expected: `ok`。

- [ ] **Step 5: 更新 CLAUDE.md 的相關段落**

在「常用指令」的打包段落後補一句，讓下一個人知道符號檔的兩層安排：

```markdown
打包會一併裝上 `/PDBSTRIPPED` 產的精簡 pdb（改名成 `live2d_mate.pdb` 放在 exe 旁邊——
DbgHelp 是靠 debug directory 裡記的檔名找符號檔的，改名是必要的）。完整 pdb 太肥
（84–168 MB），由 `release.yml` 掛成獨立資產；crash 報告記了 RVA 與 PDB GUID/age，
抓那一包就能離線精確還原到 `file:line`。
```

- [ ] **Step 6: Commit**

```bash
git add .github/workflows/release.yml CLAUDE.md
git commit -m "新增：release 掛上完整 pdb 供離線符號化"
```

---

## 收尾驗證

- [ ] **全部測試**

```powershell
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"; ctest --test-dir build/rel --output-on-failure
```

Expected: 64 支測試全綠（原本 63 支 + `test_crash_report`）。

- [ ] **七種死法最終回歸**

`uncaught` 與 `systemerror` 是後來補上的第六、七種（分別釘住 unhandled exception filter
這條派送路徑，以及 `--- exception ---` 的 `what()` 那一行有沒有真的內容），最終回歸要跑滿七種。

```powershell
Remove-Item "$env:APPDATA\live2d_mate\logs\crash-*.log" -ErrorAction SilentlyContinue
$env:PATH = "D:\Qt\6.8.3\msvc2022_64\bin;$env:PATH"
foreach ($m in @("av","uncaught","systemerror","terminate","purecall","invalidparam","stackoverflow")) {
  $env:L2M_CRASH_TEST = $m; Start-Process -FilePath "build\rel\live2d_mate.exe" -Wait
}
Remove-Item Env:\L2M_CRASH_TEST
Get-ChildItem "$env:APPDATA\live2d_mate\logs\crash-*.log" | ForEach-Object {
  "=== $($_.Name) ==="; Get-Content $_.FullName -TotalCount 12
}
```

Expected: 七份檔案，每一份都有正確的 `reason`、`stack`、`--- symbolized ---`，
而 `--- exception ---` **只在 `uncaught` 與 `systemerror` 兩份出現**（其餘五種沒有 C++ 例外在飛）。

- [ ] **確認 CI 那半邊獨立成立**（`tests.yml` 走的就是這條，不需要 Cubism Core）

```powershell
cmake -S . -B build/citest -G Ninja -DCMAKE_PREFIX_PATH=D:/Qt/6.8.3/msvc2022_64 -DL2M_BUILD_APP=OFF
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul && cmake --build build\citest --target test_crash_report'
```

Expected: configure 與建置都成功（`crash_report` 只依賴 `l2m_core`，沒有把 OS API 洩漏進去）。

---

## 自審紀錄

**Spec 覆蓋**：§5 兩階段 → Task 4–6；§6 擷取 → Task 5；§7 四個進場點 → Task 4，§7.1 ① httplib → Task 7，② 守衛空間 → Task 4 Step 3；§8 模組基底快照 → Task 4 Step 3；§9 符號策略與 §9.1 RENAME → Task 6 Step 1；§10 crash 檔格式 → Task 4 Step 3 + Task 5 Step 1 + Task 6 Step 2；§11 保留 20 份 → Task 3 + Task 9；§12 提示 → Task 8 + Task 9 + Task 10；§13 錯誤處理（重入、`%TEMP%` 退路、不連網、`TerminateProcess`）→ Task 4 Step 3 + Task 6 Step 2；§14 檔案切法 → File Structure；§15 建置 → Task 6 + Task 11；§16 測試 → Task 1–3 + 各任務的 `L2M_CRASH_TEST` 驗證；§17 已知限制 → Task 4 Step 1 的 `crash_handler.h` 區塊註解（五條逐條列出）。

**修掉的三處含糊**（第一輪自審）：Task 9 原本要新增一個 `jsonEscapeToLiteral`，實際上 `config_patch.h:88` 的 `stringPatch()` 就是幹這個的，改成直接用；Task 10 原本只說「用 Designer 加一顆按鈕」，改成逐字的 `.ui` XML 與確切插入位置（`about_page.ui:45` 附近，`infoForm` 與 `supportText` 之間）；Task 11 原本要執行者自己去讀 `release.yml` 推路徑，改成已讀過的現況（VS 產生器 → 輸出在 `build/RelWithDebInfo/`，不是本機 Ninja 那種平放）與完整的三處 YAML 修改。

**型別一致性**：`report(code, detail, seed)` 在 Task 5 從兩參數改成三參數，四個進場點同步更新；`g_frames` / `g_used` / `g_buffer` 三個 static 在 Task 4 定義、Task 5 與 6 使用；`kMaxFrames` 只在 `crash_report.h` 定義一次，`captureFromContext` 與 `CaptureStackBackTrace` 共用。
