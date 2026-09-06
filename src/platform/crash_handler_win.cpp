#include "crash_handler.h"

#include <windows.h>

// **一定要排在 windows.h 之後**：dbghelp.h 用的 HANDLE／GUID／IMAGEHLP_* 全靠
// windows.h 先定義好，順序對調就是一整串 C2146
#include <dbghelp.h>

#include <QDebug>

#include <crtdbg.h>
// _set_purecall_handler 與 _set_invalid_parameter_handler 的正式歸屬地。
// 拿掉也編得過（<crtdbg.h> 會把 <stdlib.h> 一起帶進來），但那是**別人的實作細節** ——
// 哪天 UCRT 的標頭關係一改，這裡就會變成一則跟本檔完全無關的 C2065
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <exception>
#include <io.h>
#include <stdexcept>
#include <system_error>

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

// exe 的 pdb 識別碼（PE debug directory 裡 RSDS 那一筆的 GUID 與 age）。
// **這一欄是承重的**：離線符號化要靠它確認手上那份 pdb 跟這支 exe 是不是同一次
// 連結的產物（設計文件 §9），少了它「精確解析而不是猜」這個賣點就不成立。
// 跟 imageBase 同一個作法：安裝時快照，第一階段只做格式化
GUID g_pdbGuid{};
unsigned long g_pdbAge = 0;
bool g_pdbValid = false;

// 本地時間相對 UTC 的偏移（分鐘）。安裝時算一次 —— GetTimeZoneInformation 要查登錄檔，
// 不能放在當機路徑上。**沒有時區的時間戳一定會害人對錯時間**：使用者跨時區把 crash 檔
// 寄回來時，「當地時間但沒標時區」跟同資料夾的一般 log 根本兜不起來
int g_tzOffsetMinutes = 0;

// 重入防護。**兩種重入的正確處置相反，混成一句就會把報告寫壞：**
//   · **同一條執行緒二次進來** ＝ 第二階段自己崩了（那本來就是「可能會死」的那半邊），
//     再往下走就是無窮遞迴 —— 必須立刻 TerminateProcess，一格都不能等。
//   · **另一條執行緒同時崩** ＝ 第一位此刻正夾在 openCrashFile／WriteFile／
//     FlushFileBuffers 之間（光 CreateFileW 就可能是毫秒級）。這時立刻
//     TerminateProcess 不是「不搶檔案」，而是**把正在寫報告的行程整個殺掉**，
//     留下 0 位元組或半截的 crash 檔。而 heap 損毀、共用物件被釋放那種
//     「多條執行緒幾乎同時 fault」的相關性當機，正是這個功能存在的理由 ——
//     所以這條路要讓路，等第一位寫完再收尾。
//
// 一次 CAS 同時完成「搶所有權」與「記下是誰」：thread id 在 Windows 上不會是 0，
// 拿 0 當「沒人」的哨兵是安全的。拆成旗標＋另一個 id 變數的話，輸家可能在贏家
// 還沒寫下 id 的那一格空窗裡，把同執行緒重入誤判成另一條執行緒而去空等 5 秒
LONG g_handlerThread = 0;

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

// digits 是最小寬度，不足補零；0 表示用最短表示。
//
// **時間欄位一定要補零**：檔名（core/crash_report.h 的 writeCrashFileName）本來就是
// 零填補的固定寬度，報告內文卻寫成 2026-8-31 14:5:3 的話，同一份檔案裡就有兩種格式，
// 最容易讓人以為自己讀錯；而且不補零的欄位沒辦法可靠地跟同資料夾的一般 log 對時間。
// pid 與 thread id 是識別碼不是時間欄位，維持最短表示。
void putDec(unsigned long value, int digits = 0) {
  char tmp[24];
  int n = 0;
  do {
    tmp[n++] = static_cast<char>('0' + value % 10);
    value /= 10;
  } while (value != 0);
  // tmp 是反向存放（個位在前），所以補在尾端就是前導零
  while (n < digits && n < static_cast<int>(sizeof(tmp))) tmp[n++] = '0';
  if (g_used + static_cast<size_t>(n) >= sizeof(g_buffer)) return;
  for (int i = n - 1; i >= 0; --i) g_buffer[g_used++] = tmp[i];
}

// 把一段可能很長的字串夾到 maxChars 之內寫進緩衝。**符號名非要這一支不可**：
// SYMOPT_UNDNAME 還原出來的名字可以長到 MAX_SYM_NAME（2000 字元，範本特化很容易
// 逼近上限），而 put() 對「塞不下」的處置是**整串靜默丟棄** —— 結果會是「這一格有
// 位址卻沒有名字」，在檔案裡跟「查不到符號」長得一模一樣，排查時分不出是哪一種。
// 夾掉尾巴至少留得住前面那段（命名空間 + 類別 + 函式名），判讀綽綽有餘
void putClamped(const char* text, size_t maxChars) {
  size_t n = std::strlen(text);
  if (n > maxChars) n = maxChars;
  if (g_used + n >= sizeof(g_buffer)) return;
  std::memcpy(g_buffer + g_used, text, n);
  g_used += n;
}

// PE debug directory 裡 CODEVIEW 那一筆的內容。windows.h 沒有這個結構，
// 因為它是連結器與 DbgHelp 之間的私有約定，只能自己宣告
struct CvInfoPdb70 {
  DWORD signature;  // 'RSDS'，小端讀出來是 0x53445352
  GUID guid;
  DWORD age;
  char pdbFileName[1];
};

// 從已載入的 exe 影像取出 pdb 的 GUID 與 age。**安裝時呼叫一次**，
// 走的是自己這個模組的 PE 標頭，不碰 loader lock（同 g_imageBase 的理由）
void snapshotPdbInfo(const char* base, const IMAGE_NT_HEADERS* nt) {
  const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
  if (dir.VirtualAddress == 0 || dir.Size < sizeof(IMAGE_DEBUG_DIRECTORY)) return;

  const auto* entries = reinterpret_cast<const IMAGE_DEBUG_DIRECTORY*>(base + dir.VirtualAddress);
  const size_t count = dir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
  for (size_t i = 0; i < count; ++i) {
    if (entries[i].Type != IMAGE_DEBUG_TYPE_CODEVIEW) continue;
    if (entries[i].AddressOfRawData == 0) continue;
    if (entries[i].SizeOfData < sizeof(CvInfoPdb70)) continue;
    // 已載入的影像要用 AddressOfRawData（RVA），不是檔案裡的 PointerToRawData
    const auto* cv = reinterpret_cast<const CvInfoPdb70*>(base + entries[i].AddressOfRawData);
    if (cv->signature != 0x53445352) continue;
    g_pdbGuid = cv->guid;
    g_pdbAge = cv->age;
    g_pdbValid = true;
    return;
  }
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
    return ::CreateFileW(full, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
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

// 第一階段的 frame 緩衝。static 的理由跟 g_buffer 一樣：堆疊溢位（0xC00000FD）進到
// handler 時只剩 SetThreadStackGuarantee 留下的那 64 KB，62 格指標（496 位元組）
// 沒有必要去分那杯羹
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

  // volatile 不是保守寫法而是必要的：底下的 __except 是靠 SEH 跳進來的，
  // 編譯器把 n 放進暫存器時，例外展開不保證會把它寫回堆疊 —— 那樣 return 拿到的
  // 就是過期的層數（通常是 0），等於白抓。這是 MSVC 明文記載的行為
  volatile int n = 0;

  // **整個展開迴圈包一層 SEH。** 這個功能存在的理由有一半是「堆疊被寫壞」那類當機，
  // 而展開本身就是在讀那條已經不可信的堆疊：RtlVirtualUnwind 會照 unwind info
  // 去讀被呼叫端存起來的暫存器與返回位址，位置被寫壞就是當場再爆一次。
  // 擷取排在 writeHeader 之前，所以那一下的後果不是「stack 段短一截」，
  // 而是**連表頭都不存在的一份空報告** —— 比沒有堆疊還糟。
  // 爆掉就拿已經抓到的層數往下走：少幾層遠優於整份報告消失。
  //
  // 函式裡刻意只有 POD（CONTEXT 與 UNWIND_HISTORY_TABLE 都沒有解構子）：
  // MSVC 不准同一個函式同時有 __try 與需要解構的物件（C2712）
  __try {
    while (n < kMaxFrames && ctx.Rip != 0) {
      g_frames[n] = reinterpret_cast<void*>(ctx.Rip);
      n = n + 1;
      DWORD64 imageBase = 0;
      PRUNTIME_FUNCTION entry = ::RtlLookupFunctionEntry(ctx.Rip, &imageBase, &history);
      if (!entry) {
        // 葉函式沒有 unwind 資料：返回位址就躺在堆疊頂端，手動退一格再繼續。
        //
        // **這條退路只給第一個 frame（n == 1，即剛推入 ctx.Rip 之後）。**
        // 真正的葉函式只可能出現在堆疊最頂端；更深的位置查不到 RUNTIME_FUNCTION，
        // 代表 Rip 已經不是合法的程式碼位址（堆疊被寫壞、跳進了資料），
        // 這時再猜「Rsp 指的就是返回位址」只會一路生出垃圾 frame，
        // 讓人拿著假的呼叫鏈去查一條不存在的路徑。而且那一下的解參考本身就可能
        // 再爆一次 —— 有外層的 __except 接住，但代價是整段 stack 就到此為止
        if (n != 1) break;
        ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
        ctx.Rsp += sizeof(DWORD64);
        continue;
      }
      PVOID handlerData = nullptr;
      DWORD64 establisher = 0;
      ::RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, entry, &ctx, &handlerData, &establisher, nullptr);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // 刻意什麼都不做：n 已經是「再爆一次之前抓到幾層」，直接拿去輸出
  }
  return n;
}

// 第一階段的表頭
void writeHeader(HANDLE file, const CrashStamp& stamp, unsigned long code, const char* detail, const EXCEPTION_RECORD* record) {
  reset();
  put("=== live2d_mate crash ===\r\n");
  // **建置型別跟版號一樣是承重欄位，不是裝飾。** Debug 與 RelWithDebInfo 在
  // 「_set_invalid_parameter_handler 拿不拿得到 expression／file／line」與
  // 「inline 把多少層 frame 收掉」上差很多，同一份堆疊在兩種建置下讀起來不一樣；
  // 而使用者寄回來的報告不會附帶「我裝的是哪一種建置」
  put("version : " L2M_APP_VERSION "  (" L2M_BUILD_CONFIG ")\r\n");
  put("process : live2d_mate.exe  pid ");
  putDec(stamp.pid);
  put("  mode=");
  put(g_mode);
  put("\r\n");
  put("time    : ");
  putDec(static_cast<unsigned long>(stamp.year), 4);
  put("-");
  putDec(static_cast<unsigned long>(stamp.month), 2);
  put("-");
  putDec(static_cast<unsigned long>(stamp.day), 2);
  put("T");
  putDec(static_cast<unsigned long>(stamp.hour), 2);
  put(":");
  putDec(static_cast<unsigned long>(stamp.minute), 2);
  put(":");
  putDec(static_cast<unsigned long>(stamp.second), 2);
  // 時區尾巴。ISO 8601 的 T 分隔加上 +08:00，跨時區收到這份檔案才對得上時間
  put(g_tzOffsetMinutes < 0 ? "-" : "+");
  const unsigned long tzAbs = static_cast<unsigned long>(g_tzOffsetMinutes < 0 ? -g_tzOffsetMinutes : g_tzOffsetMinutes);
  putDec(tzAbs / 60, 2);
  put(":");
  putDec(tzAbs % 60, 2);
  put("\r\n");
  put("reason  : 0x");
  putHex(code, 8);
  put(" ");
  put(crashReasonText(code));
  // 存取違規與 in-page error 的 ExceptionInformation：[0] 是存取型別、[1] 是位址。
  // 「碰了哪個位址」常常一眼就看得出誰是兇手 —— 例如 0x8 就是對 nullptr 取成員。
  // 只有 SEH 進場點拿得到 record，另外三個進場點沒有這一段
  if (record && record->NumberParameters >= 2 && (code == 0xC0000005 || code == 0xC0000006)) {
    const ULONG_PTR kind = record->ExceptionInformation[0];
    if (kind == 0) {
      put(" reading 0x");
    } else if (kind == 1) {
      put(" writing 0x");
    } else if (kind == 8) {
      put(" executing 0x");  // DEP
    } else {
      put(" accessing 0x");
    }
    putHex(record->ExceptionInformation[1], 16);
  }
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
  put("pdb     : ");
  if (g_pdbValid) {
    // GUID 的前三段在 PE 裡是小端整數，最後 8 個位元組則是位元組序列 ——
    // 要分開輸出，印出來才跟 WinDbg／llvm-symbolizer 認的那一串一模一樣
    putHex(g_pdbGuid.Data1, 8);
    put("-");
    putHex(g_pdbGuid.Data2, 4);
    put("-");
    putHex(g_pdbGuid.Data3, 4);
    put("-");
    putHex(g_pdbGuid.Data4[0], 2);
    putHex(g_pdbGuid.Data4[1], 2);
    put("-");
    for (int i = 2; i < 8; ++i) putHex(g_pdbGuid.Data4[i], 2);
    put(" age=");
    putDec(g_pdbAge);
  } else {
    // 整行不能消失 —— 事後排查時「報告裡沒有這一行」跟「這支 exe 沒有 debug
    // directory」是完全不同的兩件事，看不到欄位就分不出是哪一種
    put("(unavailable)");
  }
  put("\r\n");
  flushTo(file);
  ::FlushFileBuffers(file);
}

// 第一階段的堆疊。符號名稱是第二階段的事，這裡只做「位址 → live2d_mate.exe+0xRVA」
// 這一段純算術 —— RVA 是唯一離線還原得回來的形式（ASLR 讓絕對位址每次都不同，
// 事後對不回任何一份 pdb），理由寫在 core/crash_report.h 的 formatFrame
void writeStack(HANDLE file, int count) {
  reset();
  put("stack   :\r\n");
  if (count <= 0) {
    // 整段不能只剩一個標題 —— 同 pdb 那一行的理由：事後看報告時，
    // 「一層都抓不到」與「這一版根本還沒做堆疊擷取」長得會一模一樣。
    // 堆疊爛到連展開都跑不動本身就是線索，要看得見
    put("  (unavailable)\r\n");
    flushTo(file);
    ::FlushFileBuffers(file);
    return;
  }
  for (int i = 0; i < count; ++i) {
    // 一行約 40 位元組，緩衝快滿就先吐出去。**這個檢查一定要在寫之前而不是寫完之後**：
    // 開檔失敗時 flushTo 是 no-op（g_used 不會歸零），寫到緩衝幾乎滿了之後，
    // 底下 formatFrame 的 cap 那一條減法會在 size_t 上倒繞成天文數字，
    // 於是「放不下就回 0」的保護整個失效，直接寫出 g_buffer 之外。
    // 吐不出去（檔案無效）就收工 —— 反正也沒人讀得到
    if (g_used + 64 >= sizeof(g_buffer)) {
      flushTo(file);
      if (g_used + 64 >= sizeof(g_buffer)) break;
    }
    put("  #");
    // 補零到兩位，行首才會對齊（#09 / #10）；putDec 的寬度參數就是為這種欄位留的
    putDec(static_cast<unsigned long>(i), 2);
    put(" ");
    // 留 3 個位元組：formatFrame 自己的結尾 '\0' 與底下的 "\r\n"
    const size_t n = formatFrame(g_buffer + g_used, sizeof(g_buffer) - g_used - 3, reinterpret_cast<unsigned long long>(g_frames[i]), g_imageBase, g_imageSize, "live2d_mate.exe");
    // 緩衝不夠就整行跳過，不寫半行（handler 沒有第二次機會）
    if (n == 0) break;
    g_used += n;  // n 不含結尾 '\0'，下一個 put 會直接蓋掉它
    put("\r\n");
  }
  flushTo(file);
  ::FlushFileBuffers(file);
}

// DbgHelp 是否初始化成功。失敗就整個第二階段跳過 —— 對著沒 SymInitialize 過的
// process handle 呼叫 SymFromAddr 只會每一格都失敗，白寫一整段 "(no symbol)"
bool g_symbolsReady = false;

// DbgHelp 的初始化。**在安裝時就做，不留到當機當下**：SymInitializeW 會配置記憶體、
// 會建起一整套內部狀態，慢到不該出現在崩潰路徑上。
//
// **搜尋路徑一定要明確覆寫成「只有 exe 所在目錄」。** 使用者機器上若設過
// _NT_SYMBOL_PATH（裝過 WinDbg 或 VS 的人很常見），DbgHelp 會照那條路徑去連
// Microsoft 的符號伺服器下載 —— 在當機當下連網抓幾十 MB，使用者看到的就是
// 「按下去卡住好幾分鐘然後才消失」，比沒有這個功能還糟。
// SymInitializeW 的 UserSearchPath 給非 NULL 就已經蓋掉環境變數，這裡**還是再補一次
// SymSetSearchPathW**：路徑是延遲載入符號的那一刻才真正生效的，明確設一次，
// 比去賭「哪一版 dbghelp.dll 在什麼情況下會回頭看環境變數」便宜太多
void initSymbols() {
  // DEFERRED_LOADS：符號表要用到才載，安裝維持在毫秒等級。
  // UNDNAME：直接拿到還原過的 C++ 名字，省掉在崩潰路徑上再呼叫 UnDecorateSymbolName。
  // LOAD_LINES：少了這一個就只有函式名沒有行號，而行號才是這個功能的賣點。
  // FAIL_CRITICAL_ERRORS + NO_PROMPTS：pdb 落在已拔掉的隨身碟或斷線的網路磁碟時，
  //   預設行為是彈系統對話框 —— 在當機當下彈一個沒人會按的視窗等於把行程掛在那裡
  ::SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_FAIL_CRITICAL_ERRORS | SYMOPT_NO_PROMPTS);

  wchar_t exePath[MAX_PATH] = L"";
  const DWORD n = ::GetModuleFileNameW(nullptr, exePath, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return;
  // 搜尋路徑要的是目錄，載入模組要的是完整路徑 —— 複製一份再就地砍掉檔名
  wchar_t exeDir[MAX_PATH];
  ::lstrcpynW(exeDir, exePath, MAX_PATH);
  if (wchar_t* lastSlash = std::wcsrchr(exeDir, L'\\')) *lastSlash = L'\0';

  const HANDLE process = ::GetCurrentProcess();
  // fInvadeProcess = FALSE：不要一次列舉並載入行程裡所有模組。**代價不只是啟動時間** ——
  // Qt 與系統 DLL 都沒有 pdb，DbgHelp 會退而拿「最接近的匯出符號」當名字，
  // 而那種名字經常是錯的（本專案實測過：一格 Qt 的位址被標成
  // qt_plugin_query_metadata_v2）。報告裡誠實的 "(no symbol)" 比一個看起來很像
  // 真的、實際上指錯函式的名字有用得多
  if (!::SymInitializeW(process, exeDir, FALSE)) return;
  ::SymSetSearchPathW(process, exeDir);

  // **fInvadeProcess = FALSE 的另一面：一個模組都沒註冊，SymFromAddr 會每一格都失敗。**
  // 這一行漏掉的症狀是六份報告的 --- symbolized --- 段整段都是 "(no symbol)"，
  // pdb 明明就躺在 exe 旁邊，而且從頭到尾沒有任何錯誤訊息 —— 所以這裡明確把自己
  // 這一支 exe（也正是唯一有 pdb 的模組）掛上去。SYMOPT_DEFERRED_LOADS 讓這一步
  // 只記下位址範圍，那份幾十 MB 的 pdb 要等真的查詢時才讀。
  // **相依 installCrashHandler 裡的影像快照先跑過**：base／size 就是那裡填的
  if (g_imageBase == 0) return;
  if (::SymLoadModuleExW(process, nullptr, exePath, nullptr, g_imageBase, static_cast<DWORD>(g_imageSize), nullptr, 0) == 0) {
    return;
  }
  g_symbolsReady = true;
}

// 位址落在哪一個模組，回傳檔名與模組內的 RVA。
//
// **第二階段限定，第一階段一個字都不准碰這支**：GetModuleHandleEx 會取 loader lock，
// 而崩在 loader lock 上時那一下就是死鎖（crash_handler.h 的禁用清單指名了它）。
// 第一階段因此只認自己這一支 exe 的區間 —— 那份基底是安裝時快照好的整數。
//
// **這一欄是承重的，不是裝飾。** 沒有它，非 exe 的 frame 在報告裡就只剩一串
// 絕對位址：ASLR 讓那串數字每次載入都不同，拿到 log 的人**事後再也還原不了**
// 它屬於誰。設計文件 §9 把「死在誰家看得出來」列為沒有符號檔時仍然拿得到的東西，
// 而 Qt 與系統 DLL 本來就是 --- symbolized --- 段裡的多數格子。
//
// UNCHANGED_REFCOUNT：不動 refcount，拿到的 handle 也就不必（不可以）關。
// 只取檔名不取完整路徑：完整路徑會把建置機器的目錄結構寫進使用者寄回來的檔案，
// 而對照時檔名就夠了（同 writeSymbols 印行號檔名的理由）
bool moduleOfAddress(unsigned long long address, char* nameOut, size_t nameCap, unsigned long long* rvaOut) {
  if (!nameOut || nameCap == 0 || !rvaOut) return false;
  nameOut[0] = '\0';
  HMODULE mod = nullptr;
  if (!::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &mod) || mod == nullptr) {
    return false;
  }
  wchar_t path[MAX_PATH] = L"";
  const DWORD n = ::GetModuleFileNameW(mod, path, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return false;
  const wchar_t* leaf = path;
  if (const wchar_t* slash = std::wcsrchr(path, L'\\')) leaf = slash + 1;
  // crash 檔是 UTF-8 無 BOM（設計文件 §10），所以這裡也照 UTF-8 轉。
  // 模組名幾乎都是純 ASCII，但使用者的外掛 DLL 不保證
  if (::WideCharToMultiByte(CP_UTF8, 0, leaf, -1, nameOut, static_cast<int>(nameCap), nullptr, nullptr) == 0) {
    return false;
  }
  *rvaOut = address - reinterpret_cast<unsigned long long>(mod);
  return true;
}

// 第二階段：符號化。**跟第一階段的分野就在這裡** —— 這一段可以配置記憶體、可以進
// DbgHelp 的內部鎖、可以整段失敗，因為表頭與 stack 段在進來之前就已經各自
// FlushFileBuffers 落地了。它掛掉只損失「好看」的那半邊，可離線還原的 RVA 一格不少。
//
// **sehFaultingFirstFrame 承載一條會讓整份符號化靜靜偏移的規則：查詢前要先減 1。**
// #01 以後的每一格都是「call 的下一道指令」的位址。那個 call 若剛好是某一行
// （甚至某個函式）的最後一道指令 —— 尾呼叫與 noreturn 的呼叫非常常見 —— 直接餵給
// SymFromAddr／SymGetLineFromAddr64 就會歸到下一行、下一個函式，而且完全沒有徵兆：
// 產出的是一份「解出來了但指錯地方」的報告，比解不出來更會把人帶去查一條不存在的路。
// **唯一的例外是 SEH 路徑的 #00**：那一格是 ctx.Rip，是肇事指令本身而不是回傳位址，
// 減 1 反而會指到它的前一道指令。兩者必須分開處理。
//
// 減 1 **只用於查詢** —— 印出來的位址一律是原值，這一段才跟上面的 stack 段逐行對得起來
void writeSymbols(HANDLE file, int count, bool sehFaultingFirstFrame) {
  if (file == INVALID_HANDLE_VALUE || !g_symbolsReady || count <= 0) return;
  const HANDLE process = ::GetCurrentProcess();

  // SYMBOL_INFO 是變長結構（名字接在 Name[1] 後面），要自己備一塊夠大的儲存空間。
  // 2 KB 出頭，堆疊溢位那條路只剩 SetThreadStackGuarantee 的 64 KB 也還撐得住
  alignas(SYMBOL_INFO) char symbolStorage[sizeof(SYMBOL_INFO) + MAX_SYM_NAME];
  auto* const symbol = reinterpret_cast<SYMBOL_INFO*>(symbolStorage);

  reset();
  put("--- symbolized ---\r\n");
  for (int i = 0; i < count; ++i) {
    // 一行可能長到「位址 + 還原後的 C++ 名字 + 檔名行號」，每格先把緩衝吐乾淨，
    // 底下才有完整的 4 KB 可用（file 無效時上面就 return 了，不會踩到 flushTo 的 no-op）
    flushTo(file);
    const unsigned long long address = reinterpret_cast<unsigned long long>(g_frames[i]);
    // 只有 SEH 路徑的第 0 格是肇事指令；其餘每一格都是回傳位址，查詢前退一個位元組
    const DWORD64 lookup = static_cast<DWORD64>((i == 0 && sehFaultingFirstFrame) ? address : address - 1);

    put("  #");
    putDec(static_cast<unsigned long>(i), 2);
    put(" ");
    // 位址欄位刻意跟 writeStack 走同一支 formatFrame：兩段的第 i 行必須一字不差，
    // 否則讀報告的人得自己在心裡把兩份清單對起來 —— 而那正是最容易對錯的地方
    const size_t n = formatFrame(g_buffer + g_used, sizeof(g_buffer) - g_used - 3, address, g_imageBase, g_imageSize, "live2d_mate.exe");
    if (n == 0) break;
    g_used += n;  // n 不含結尾 '\0'，下一個 put 會直接蓋掉它

    // **非 exe 的 frame 補上模組名與模組內 RVA。** exe 自己的格子上面那支
    // formatFrame 已經寫成 "live2d_mate.exe+0xRVA" 了，再補一次只是重複。
    // 取不到模組（位址根本不在任何已載入影像裡 —— 堆疊被寫壞、跳進了資料）
    // 就維持現狀只留絕對位址，不去猜
    const bool insideExe = g_imageSize > 0 && address >= g_imageBase && address - g_imageBase < g_imageSize;
    if (!insideExe) {
      char moduleName[MAX_PATH] = "";
      unsigned long long moduleRva = 0;
      if (moduleOfAddress(address, moduleName, sizeof(moduleName), &moduleRva)) {
        put("  ");
        putClamped(moduleName, 64);
        put("+0x");
        putHex(moduleRva, 8);
      }
    }
    put("  ");

    std::memset(symbol, 0, sizeof(SYMBOL_INFO));
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;
    DWORD64 displacement = 0;
    if (::SymFromAddr(process, lookup, &displacement, symbol)) {
      putClamped(symbol->Name, 240);
      // 函式內位移。**要拿原值去減而不是 lookup**，不然每一格都會少 1，
      // 拿去跟反組譯對位置時會差一個位元組。出貨版只有 public symbol（沒有行號），
      // 這個位移就是「進到函式多深」的唯一線索，不能省
      if (address > symbol->Address) {
        put("+0x");
        putHex(address - symbol->Address, 0);
      }
    } else {
      // 系統 DLL 沒有 pdb，這裡本來就會是多數。**整行不能省** —— 少掉一行，
      // 底下的編號就跟上面 stack 段錯開了，那正是這一段最需要保住的性質
      put("(no symbol)");
    }

    IMAGEHLP_LINE64 line{};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    DWORD lineDisplacement = 0;
    if (::SymGetLineFromAddr64(process, lookup, &lineDisplacement, &line) && line.FileName) {
      put("  ");
      // 只印檔名不印完整路徑：完整路徑會把建置機器的目錄結構寫進使用者寄回來的檔案，
      // 而對照原始碼時檔名就夠了
      const char* const slash = std::strrchr(line.FileName, '\\');
      putClamped(slash ? slash + 1 : line.FileName, 80);
      put(":");
      putDec(line.LineNumber);
    }
    put("\r\n");
  }
  flushTo(file);
  ::FlushFileBuffers(file);
}

// ---------------------------------------------------------------------------
// MSVC C++ 例外（0xE06D7363）的 EXCEPTION_RECORD 內容
//
// **為什麼不用 std::current_exception()** —— 2026-08-31 實測踩到的坑：
// 它在 unhandled exception filter 裡**永遠回 null**。MSVC 是進入 catch handler 時
//（__InternalCxxFrameHandler → CatchIt）才把例外寫進 per-thread data 的
// _curexception，而 last-chance filter 發生在展開**之前**、從來沒有進過任何 catch。
// 症狀是「--- exception --- 整段永遠不出現」，而且完全沒有錯誤訊息：
// 當時六種 L2M_CRASH_TEST 沒有一種抓得到，連 uncaught 也一樣（堆疊還停在 throw
// 那一行，正好證明我們是在展開前被叫到的）。所以改成直接讀 EXCEPTION_RECORD。
//
// windows.h 沒有底下這幾個結構 —— 它們是編譯器與 vcruntime 之間的私有約定
//（同 CvInfoPdb70 的處境），只能自己宣告。**x64 專用**：那邊的 ThrowInfo 內部
// 一律是 32 位元 RVA，要靠 ExceptionInformation[3] 的模組基底才還原得回指標
// ---------------------------------------------------------------------------

struct ThrowPmd {
  int mdisp;  // 基底子物件在完整物件裡的位移
  int pdisp;  // 虛擬基底表的位移；**< 0 ＝ 非虛擬繼承**
  int vdisp;
};

struct ThrowTypeDescriptor {
  const void* vftable;
  void* spare;
  char name[1];  // NUL 結尾的修飾名，例如 ".?AVruntime_error@std@@"
};

struct ThrowCatchableType {
  unsigned int properties;
  int pType;  // RVA -> ThrowTypeDescriptor
  ThrowPmd thisDisplacement;
  int sizeOrOffset;
  int copyFunction;  // RVA
};

struct ThrowCatchableTypeArray {
  int count;
  int types[1];  // RVA -> ThrowCatchableType；**[0] 是最衍生的那一個**
};

struct ThrowInfoRec {
  unsigned int attributes;
  int pmfnUnwind;           // RVA
  int pForwardCompat;       // RVA
  int pCatchableTypeArray;  // RVA
};

// CatchableTypeArray 的走訪上限。count 是從可能已損毀的記憶體讀出來的 ——
// 讀爆了有 __except 接住，但一個天文數字的迴圈會讓當機路徑卡在那裡轉。
// 真實的繼承鏈個位數就到頂，64 綽綽有餘
constexpr int kMaxCatchableTypes = 64;

// 拆出四個參數。[0] magic、[1] 被丟出的物件、[2] ThrowInfo、[3] 模組基底。
// magic 由 vcruntime 的 _CxxThrowException 寫死成 EH_MAGIC_NUMBER1(0x19930520)，
// 另外兩個是後來擴充 ThrowInfo 時加的版本，一起接受。
// **rethrow（throw;）的 NumberParameters 是 0**，那種記錄一個欄位都取不到 ——
// 所以下面的檢查不是保守，是不檢查就會拿 ExceptionInformation 的垃圾去解參考
bool cppExceptionParts(const EXCEPTION_RECORD* record, const char** imageBase, const ThrowInfoRec** info, const char** object) {
  if (!record || record->ExceptionCode != 0xE06D7363) return false;
  if (record->NumberParameters < 4) return false;
  const ULONG_PTR magic = record->ExceptionInformation[0];
  if (magic < 0x19930520 || magic > 0x19930522) return false;
  *object = reinterpret_cast<const char*>(record->ExceptionInformation[1]);
  *info = reinterpret_cast<const ThrowInfoRec*>(record->ExceptionInformation[2]);
  *imageBase = reinterpret_cast<const char*>(record->ExceptionInformation[3]);
  return *object != nullptr && *info != nullptr && *imageBase != nullptr;
}

// 步驟一：型別名。**純讀，完全不碰被丟出的那個物件** —— 走的是編譯器產生的
// 靜態唯讀資料（ThrowInfo 與 TypeDescriptor 都在 .rdata），比步驟二安全一個數量級，
// 所以就算步驟二整個放棄，這一行通常還是拿得到。
//
// **整段包一層 SEH。** 這個功能存在的理由有一半是 heap 損毀那類當機，而上面那幾個
// 指標正是從已經不可信的記憶體裡撈出來的。爆掉就回 false 讓呼叫端印 (unavailable)——
// 絕不能讓第二階段的失敗把行程帶走，第一階段已經落地的內容更不能受影響。
// 函式裡刻意只有 POD：MSVC 不准同一個函式同時有 __try 與需要解構的物件（C2712）
bool readThrowTypeName(const EXCEPTION_RECORD* record, char* out, size_t cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  const char* imageBase = nullptr;
  const ThrowInfoRec* info = nullptr;
  const char* object = nullptr;
  if (!cppExceptionParts(record, &imageBase, &info, &object)) return false;

  __try {
    if (info->pCatchableTypeArray == 0) return false;
    const auto* array = reinterpret_cast<const ThrowCatchableTypeArray*>(imageBase + info->pCatchableTypeArray);
    if (array->count <= 0) return false;
    const auto* type = reinterpret_cast<const ThrowCatchableType*>(imageBase + array->types[0]);
    const auto* descriptor = reinterpret_cast<const ThrowTypeDescriptor*>(imageBase + type->pType);
    // TypeDescriptor 的名字是 ".?AVruntime_error@std@@" 這種 RTTI 修飾名。
    // **要還原它得先跳過開頭那個句點、而且要用這一組旗標** —— 直接整串餵給
    // UnDecorateSymbolName 一律回 0（實測過：報告裡就會留著一整串 ".?AV…@@"）。
    // 句點是 RTTI 專用的前綴，__unDName 認的是後面那個 '?' 開頭的部分；
    // NO_ARGUMENTS + 32_BIT_DECODE 則是 MSVC 自己的 type_info::name() 用的同一組，
    // 所以輸出跟偵錯器裡看到的 "class std::runtime_error" 一字不差。
    // **UnDecorateSymbolName 不需要 SymInitialize** —— 它是 DbgHelp 裡少數的純字串
    // 函式，不看 process handle、不碰任何符號表。這件事是承重的：initSymbols 失敗時
    // g_symbolsReady 為 false、整個 --- symbolized --- 段會被跳過，但
    // --- exception --- 的 type 那一行**照樣要有還原過的名字**（那正是整份報告
    // 最有價值的一行）。把兩者的相依綁在一起會讓「沒有 pdb 的機器」連型別名都不見。
    //
    // **原始修飾名先寫進 out 當保底，還原成功才覆蓋上去。** UnDecorateSymbolName
    // 是這支函式裡唯一會配置記憶體的呼叫，也就是唯一可能卡在已損毀的 heap 上或
    // 自己再爆一次的一步；先把保底值放好，那一下就算走到底下的 __except，
    // ".?AVsystem_error@std@@" 也還留在報告裡 —— 難看，但認得出是哪個型別
    const char* const mangled = descriptor->name;
    ::lstrcpynA(out, mangled, static_cast<int>(cap));
    // 句點是 RTTI 專用的前綴，__unDName 認的是後面那個 '?' 開頭的部分
    const char* const undecoratable = mangled[0] == '.' ? mangled + 1 : mangled;
    char undecorated[256] = "";
    if (::UnDecorateSymbolName(undecoratable, undecorated, sizeof(undecorated), UNDNAME_NO_ARGUMENTS | UNDNAME_32_BIT_DECODE) != 0 && undecorated[0] != '\0') {
      ::lstrcpynA(out, undecorated, static_cast<int>(cap));
    }
    return out[0] != '\0';
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    // **刻意不清空 out**：保底的修飾名可能已經寫進去了，留著遠好過整個欄位消失
    return out[0] != '\0';
  }
}

// 步驟二：what()。**只在 CatchableTypeArray 裡真的找得到 std::exception 時才做。**
// 找不到就不硬轉：把任意物件當成 std::exception* 去呼叫虛擬函式，讀到的是別的東西的
// vtable，那一下不是「拿不到訊息」而是「跳到亂七八糟的位址」——
// 而 throw 42、throw 自訂的非衍生類別都完全合法，這不是罕見情況。
//
// **只處理非虛擬繼承（pdisp < 0）。** 虛擬繼承要再走一次 vbtable，而
// std::exception 被虛擬繼承在實務上等於不存在 —— 為一條沒人會踩到、也驗不到的
// 指標算術冒險不划算，那種情況一律只留型別名。
//
// 呼叫 what() 與複製字串關在**同一個** __try 裡：回傳的指標指向那個可能已經半毀的
// 例外物件，讀它跟呼叫它一樣危險，只保護前半段等於沒保護
bool readThrowWhat(const EXCEPTION_RECORD* record, char* out, size_t cap) {
  if (!out || cap == 0) return false;
  out[0] = '\0';
  const char* imageBase = nullptr;
  const ThrowInfoRec* info = nullptr;
  const char* object = nullptr;
  if (!cppExceptionParts(record, &imageBase, &info, &object)) return false;

  __try {
    if (info->pCatchableTypeArray == 0) return false;
    const auto* array = reinterpret_cast<const ThrowCatchableTypeArray*>(imageBase + info->pCatchableTypeArray);
    const int count = array->count < kMaxCatchableTypes ? array->count : kMaxCatchableTypes;
    for (int i = 0; i < count; ++i) {
      const auto* type = reinterpret_cast<const ThrowCatchableType*>(imageBase + array->types[i]);
      const auto* descriptor = reinterpret_cast<const ThrowTypeDescriptor*>(imageBase + type->pType);
      if (std::strcmp(descriptor->name, ".?AVexception@std@@") != 0) continue;
      if (type->thisDisplacement.pdisp >= 0) return false;  // 虛擬繼承，不碰
      const auto* self = reinterpret_cast<const std::exception*>(object + type->thisDisplacement.mdisp);
      const char* const text = self->what();
      if (!text) return false;
      ::lstrcpynA(out, text, static_cast<int>(cap));
      return out[0] != '\0';
    }
    return false;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    out[0] = '\0';
    return false;
  }
}

// C++ 例外的型別與訊息。**第二階段的第一件事**（排在符號化之前）：這裡只是幾次
// 指標讀取，卻常常是整份報告最有價值的一行 —— 而符號化要載入幾十 MB 的 pdb、
// 要進 DbgHelp 的內部鎖，是這一階段最可能卡住或再崩一次的部分。
// **風險最高的不該擋在價值最高的前面。**
//
// 不是 C++ 例外就整段不輸出（terminate／purecall／invalidparam／存取違規／
// 堆疊溢位都是這樣）。印一行空的「--- exception ---」只會讓人以為是抓失敗，
// 而不是本來就沒有例外在飛
void writeExceptionMessage(HANDLE file, const EXCEPTION_RECORD* record) {
  if (file == INVALID_HANDLE_VALUE) return;
  if (!record || record->ExceptionCode != 0xE06D7363) return;

  char typeName[256] = "";
  char message[512] = "";
  const bool haveType = readThrowTypeName(record, typeName, sizeof(typeName));
  const bool haveWhat = readThrowWhat(record, message, sizeof(message));

  reset();
  put("--- exception ---\r\n");
  // 兩個欄位都恆存在。**拿不到也要留著那一行** —— 事後看報告時「沒有這一行」與
  // 「ThrowInfo 讀爆了」是完全不同的兩件事，看不到欄位就分不出是哪一種
  //（同表頭 pdb 那一行的理由）
  put("  type    : ");
  put(haveType ? typeName : "(unavailable)");
  put("\r\n");
  put("  what    : ");
  if (haveWhat) {
    put(message);
  } else if (haveType) {
    // 型別名拿得到代表 ThrowInfo 走得通，那 what() 拿不到就只有一個原因：
    // 這個東西根本不是從 std::exception 衍生的
    put("(not derived from std::exception)");
  } else {
    put("(unavailable)");
  }
  put("\r\n");
  flushTo(file);
  ::FlushFileBuffers(file);
}

// ---------------------------------------------------------------------------
// 第二階段的看門狗：卡住的話一定要死掉，不能凍住
//
// 第二階段刻意允許配置與上鎖（型別名的 UnDecorateSymbolName、what()、DbgHelp），
// 而**「崩在 malloc／free 裡」正是這整個功能最想活下來的那一家當機**。
// 那種現場的 CRT heap lock 還握在自己手上，上面任何一支都可能當場死鎖：
// TerminateProcess 永遠走不到，另一條執行緒讓路用的 Sleep(5000) 也幫不上忙
// （同一條執行緒，那條路根本不會走到），使用者看到的是一隻**永遠賴在畫面上、
// 只能去工作管理員砍掉**的桌寵 —— 比完全沒有這個功能還糟。
//
// **為什麼是安裝期就建好一條常駐執行緒，而不是進第二階段前才 CreateThread？**
// 這是整段唯一不直覺的地方。崩潰當下 CreateThread 會為每一個已載入的 DLL 跑一次
// DllMain(DLL_THREAD_ATTACH)，**那要取 loader lock** —— 而「崩在 loader lock 上」
// 正是 crash_handler.h 的禁用清單一路在迴避的情境（連 GetModuleHandleEx 都不准碰
// 就是這個理由）。在那種狀態下 CreateThread 自己就會卡住，看門狗連計時都沒開始，
// 等於白裝。所以改成：安裝時（行程還健康）把執行緒與事件都備好，執行緒開頭
// WaitForSingleObject(INFINITE) 睡著，崩潰路徑上只剩一次 SetEvent —— 那是
// 核心呼叫，不配置、不上鎖，守得住第一階段的禁用清單。
// 代價是行程生命期多一條閒置執行緒（一次 WaitForSingleObject，零 CPU），
// 換掉「在最不能失敗的時候去做最可能卡住的事」。
//
// 觸發點在**第一階段 FlushFileBuffers 落地之後、進第二階段之前**：到那一刻為止的
// 內容都已經在磁碟上，看門狗開火只會損失「好看」的那半邊。第二階段正常跑完會照
// 原路 TerminateProcess，看門狗自然來不及作用。
constexpr DWORD kPhase2WatchdogMs = 10000;

HANDLE g_watchdogEvent = nullptr;

// 看門狗開火時要用的結束碼。**在 SetEvent 之前寫**：SetEvent／WaitForSingleObject
// 這一對本身就是完整的記憶體屏障，看門狗醒來時一定讀得到新值。
// volatile 只是擋編譯器把這個寫入搬到 SetEvent 之後
volatile unsigned long g_watchdogExitCode = 0;

DWORD WINAPI phase2Watchdog(LPVOID) {
  // 一輩子只醒一次。醒不來（沒有人當機）就跟著行程一起結束
  if (::WaitForSingleObject(g_watchdogEvent, INFINITE) != WAIT_OBJECT_0) return 0;
  ::Sleep(kPhase2WatchdogMs);
  // 走到這裡代表第二階段超過 10 秒還沒收掉行程 —— 一律當成卡住
  ::TerminateProcess(::GetCurrentProcess(), g_watchdogExitCode);
  return 0;
}

// 所有進場點的共同出口。detail 是給 terminate／purecall／invalidparam 的補充字串；
// info 只有 SEH 進場點給得出來 —— 它同時帶著 ExceptionRecord（faulting address）
// 與 ContextRecord（當機瞬間的暫存器，也就是堆疊展開的起點）。
//
// **刻意收整包 EXCEPTION_POINTERS 而不是兩個獨立參數**：那兩份資料永遠來自同一次
// filter 呼叫、一起有或一起沒有，拆成兩個參數就多出「有 record 沒 context」這種
// 表示得出來卻不可能發生的狀態，將來一定有人只傳一半、而漏掉的那半是靜默失效的
void report(unsigned long code, const char* detail, const EXCEPTION_POINTERS* info = nullptr) {
  const LONG self = static_cast<LONG>(::GetCurrentThreadId());
  const LONG owner = ::InterlockedCompareExchange(&g_handlerThread, self, 0);
  if (owner != 0) {
    if (owner != self) {
      // 另一條執行緒正在寫報告：**讓它寫完**。這裡立刻收掉行程的話，
      // 留給使用者的就是半份 crash 檔（理由見 g_handlerThread 的註解）。
      // Sleep 是核心呼叫，不配置、不上鎖，守得住第一階段的禁用清單
      ::Sleep(5000);
    }
    // 同一條執行緒二次進來就直接落到這裡：立刻死，不能等也不能往下走
    ::TerminateProcess(::GetCurrentProcess(), code);
    return;
  }

  // **擷取一定要排在開檔與寫表頭之前：先把現場抓下來，再去做任何可能失敗的事。**
  // nowStamp／openCrashFile／writeHeader 之中最便宜的 CreateFileW 都可能是毫秒級
  // （磁碟滿、防毒的檔案系統過濾器、目錄被刪），而它們踩到的每一個坑都發生在
  // 一條已經不可信的堆疊上。順序反過來的話，那一下的後果不是「stack 段短一截」，
  // 而是**連現場都沒抓到就先死在開檔上** —— 同 captureFromContext 裡外層 __except
  // 的理由。（**不是**因為這幾支的框架會混進堆疊：CaptureStackBackTrace 抓的是
  // 呼叫當下那一條，它們早就返回、框架也早就彈掉了。真實報告的 #00 就是
  // report 自己，底下直接接 ucrtbase 與肇事的呼叫端，一格雜訊都沒有。）
  //
  // ContextRecord 為 nullptr ＝ 當下這條堆疊就是現場（terminate／purecall／
  // invalidparam），用 CaptureStackBackTrace；有值 ＝ SEH，例外派送已經在現場**之上**
  // 疊了好幾層系統框架（KiUserExceptionDispatcher、UnhandledExceptionFilter…），
  // 抓當下的堆疊只會看到那些，一定要從當機瞬間的暫存器狀態展開才會指到肇事的那一行
  //
  // **這個布林只能算一次**：它同時決定「擷取走哪一條路」與「第二階段的 #00 該不該
  // 套回傳位址減 1」。兩處各自判斷的話，將來只改一邊就會產出一份「解得出來、
  // 但第一格整個偏掉」的報告 —— 而且從輸出上完全看不出來
  const bool fromContext = info && info->ContextRecord;
  const int count = fromContext ? captureFromContext(*info->ContextRecord) : static_cast<int>(::CaptureStackBackTrace(0, kMaxFrames, g_frames, nullptr));

  const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
  const CrashStamp stamp = nowStamp();
  HANDLE file = openCrashFile(stamp);
  writeHeader(file, stamp, code, detail, record);
  writeStack(file, count);

  // **看門狗上膛。** 到上一行為止的內容都已經 FlushFileBuffers 落地，接下來的
  // 第二階段允許配置與上鎖，因此也就允許死鎖（崩在 CRT heap lock 上時，
  // what()／UnDecorateSymbolName／DbgHelp 任何一支都可能當場卡死）。
  // 超過 kPhase2WatchdogMs 還沒收掉行程就由看門狗執行緒代勞 ——
  // 詳細理由與「為什麼執行緒是安裝期就建好的」寫在 phase2Watchdog 上方
  g_watchdogExitCode = code;
  if (g_watchdogEvent) ::SetEvent(g_watchdogEvent);

  // 第二階段。**排在這裡不是隨意的**：第一階段（表頭與 stack 段）的內容都已經
  // FlushFileBuffers 落到磁碟了，所以底下這兩支就算配置失敗、上鎖卡住、甚至自己再崩一次
  // （同執行緒重入會被攔成立刻 TerminateProcess），損失的都只是「好看」的那半邊，
  // 可離線還原的表頭與 RVA 一格都不會少。
  //
  // **這兩支之間的順序也是刻意的：例外資訊在前、符號化在後。** 前者只是幾次指標
  // 讀取卻最有診斷價值（what() 那一行常常一句就結案），後者要載入幾十 MB 的 pdb、
  // 要進 DbgHelp 的內部鎖，是這一階段最可能卡住或再崩一次的部分。
  // 反過來排的話，符號化掛掉會連帶把例外訊息一起賠進去
  writeExceptionMessage(file, record);
  writeSymbols(file, count, fromContext);
  if (file != INVALID_HANDLE_VALUE) ::CloseHandle(file);

  // ExitProcess 會跑 DLL detach 與 atexit，在已損毀的狀態下可能再死一次或卡住。
  // 跳過 LoggingGuard 解構的 flush 沒有損失 —— DailyRotatingFileSink 每則都
  // 已經 flush 過了（見 app/logging.cpp:134）
  ::TerminateProcess(::GetCurrentProcess(), code);
}

LONG WINAPI onUnhandledException(EXCEPTION_POINTERS* info) {
  const EXCEPTION_RECORD* record = info ? info->ExceptionRecord : nullptr;
  report(record ? record->ExceptionCode : 0, nullptr, info);
  return EXCEPTION_EXECUTE_HANDLER;  // 到不了，report 不會回來
}

void onTerminate() { report(0xC0000409, "std::terminate"); }

void onPureCall() { report(0xC0000025, "purecall"); }

void onInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
  // 這四個參數在 release 建置一律是 nullptr（CRT 不帶 debug 字串），
  // 所以刻意不去讀它們 —— 堆疊本來就比它們有用
  report(0xC0000409, "invalid CRT parameter");
}

// L2M_CRASH_TEST=purecall 用的一對類別。**不能寫在 triggerCrashTestFromEnv 裡面**：
// MSVC 對區域類別要求「被參照到的虛擬成員一定要有定義」（error C3640），
// 而純虛擬正好給不出定義。
//
// 觸發原理：Base 的建構子跑的時候 vptr 還指著 Base 自己的 vtable，
// 那一格填的就是 CRT 的 _purecall —— 也就是我們用 _set_purecall_handler 換掉的那支。
//
// **一定要透過指標呼叫。** 建構子裡直接寫 callIt() 的話，MSVC 知道那當下的動態型別
// 就是 PureCallBase，會把它去虛擬化成直接呼叫 —— 而純虛擬沒有定義，結果是
// LNK2019 而不是執行期的 _purecall。noinline 則是不讓最佳化把這一層攤平之後
// 又去虛擬化回去
struct PureCallBase;
__declspec(noinline) void callThroughVtable(PureCallBase* self);

struct PureCallBase {
  PureCallBase() { callThroughVtable(this); }
  virtual void callIt() = 0;
};

struct PureCallDerived : PureCallBase {
  void callIt() override {}
};

void callThroughVtable(PureCallBase* self) { self->callIt(); }

}  // namespace

std::filesystem::path appDataDirFromEnv() {
  wchar_t appData[MAX_PATH] = L"";
  // **回傳值 >= MAX_PATH 一定要當成失敗。** 那個值不是「寫了幾個字」而是
  // 「要寫得下需要多大」，緩衝裡的內容此時**未定義** —— 只檢查 == 0 的話，
  // 一個超長的 %APPDATA%（重導向到深層路徑、或被惡意設過）會讓後面拿一段
  // 沒有初始化過的位元組去組路徑
  const DWORD n = ::GetEnvironmentVariableW(L"APPDATA", appData, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) return {};
  return std::filesystem::path(appData) / L"live2d_mate";
}

void installCrashHandler(const std::filesystem::path& crashDir, const char* mode) {
  static bool installed = false;
  if (installed) return;
  installed = true;

  if (mode && *mode) ::lstrcpynA(g_mode, mode, sizeof(g_mode));

  // **只收絕對路徑。** appDataDirFromEnv() 算不出來時回的是空路徑，呼叫端接上
  // "logs" 之後就變成相對的 "logs" —— crash 檔會落在當下的工作目錄，而那由誰啟動
  // 決定（捷徑、檔案總管、偵錯器各給一個不同的答案），使用者與我們都不知道要去哪裡撿。
  // 這時寧可整個放棄主路徑，讓 openCrashFile 走 %TEMP% 退路：位置至少是確定的
  if (crashDir.is_absolute()) {
    std::error_code ec;
    std::filesystem::create_directories(crashDir, ec);
    const std::wstring dir = crashDir.wstring();
    if (!dir.empty() && dir.size() < MAX_PATH) ::lstrcpynW(g_crashDir, dir.c_str(), MAX_PATH);
  }

  // exe 的基底、SizeOfImage 與 pdb 識別碼快照一次。exe 不會被卸載或搬移，所以永遠有效
  if (HMODULE self = ::GetModuleHandleW(nullptr)) {
    auto* const base = reinterpret_cast<char*>(self);
    g_imageBase = reinterpret_cast<unsigned long long>(self);
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(self);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature == IMAGE_NT_SIGNATURE) {
      g_imageSize = nt->OptionalHeader.SizeOfImage;
      snapshotPdbInfo(base, nt);
    }
  }

  // 時區偏移也在這裡算完。Windows 的 Bias 定義是「UTC ＝ 本地 + Bias」，
  // 報告要的是相反方向，所以取負號
  TIME_ZONE_INFORMATION tz{};
  const DWORD tzKind = ::GetTimeZoneInformation(&tz);
  if (tzKind != TIME_ZONE_ID_INVALID) {
    LONG bias = tz.Bias;
    if (tzKind == TIME_ZONE_ID_DAYLIGHT) {
      bias += tz.DaylightBias;
    } else if (tzKind == TIME_ZONE_ID_STANDARD) {
      bias += tz.StandardBias;
    }
    g_tzOffsetMinutes = -bias;
  }

  // 第二階段的看門狗。**建在這裡（行程還健康時）而不是崩潰路徑上** ——
  // 理由寫在 phase2Watchdog 上方那一段（崩潰當下 CreateThread 會走 loader lock）。
  // manual-reset、初始未設定：只會被 report() 設一次，之後永遠保持設定狀態。
  // 失敗（極罕見）就只是沒有看門狗，其餘一切照舊 —— 不能因此擋掉 handler 的安裝
  g_watchdogEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_watchdogEvent) {
    // 執行緒 handle 立刻關掉：我們從頭到尾不需要 join 也不需要查它，
    // 關掉的只是 handle，執行緒本身照樣活到行程結束
    if (HANDLE thread = ::CreateThread(nullptr, 0, &phase2Watchdog, nullptr, 0, nullptr)) {
      ::CloseHandle(thread);
    } else {
      ::CloseHandle(g_watchdogEvent);
      g_watchdogEvent = nullptr;
    }
  }

  // DbgHelp 也在這裡就緒。**不能留到當機當下** —— 理由與搜尋路徑的坑寫在 initSymbols
  initSymbols();

  ::SetUnhandledExceptionFilter(&onUnhandledException);
  // 主執行緒的 terminate handler 與堆疊守衛空間走跟 httplib worker 同一支，
  // 免得兩份程式碼各自漂移（那種漂移是靜默的：某一條執行緒的堆疊溢位就是寫不出報告）
  installThreadCrashSupport();
  _set_purecall_handler(&onPureCall);
  _set_invalid_parameter_handler(&onInvalidParameter);
  // 沒有這一行的話，Debug 建置會先彈 CRT 斷言對話框把行程卡住，
  // 我們的 handler 永遠等不到
  _CrtSetReportMode(_CRT_ASSERT, 0);

  if (g_crashDir[0] == L'\0') {
    // 主路徑沒了還是能寫報告（%TEMP% 退路），但位置換了一個地方 ——
    // 這一行是事後「logs/ 裡怎麼沒有 crash 檔」唯一的線索
    qWarning() << "[crash] 當機報告目錄不是絕對路徑，改用 %TEMP% 退路";
  } else {
    qInfo() << "[crash] 當機報告已安裝，目錄:" << QString::fromStdWString(g_crashDir);
  }
}

void installThreadCrashSupport() {
  // 這條執行緒需要的**兩**件事，兩件都是 per-thread 的：
  //
  // ① terminate handler —— MSVC 的 std::set_terminate 是 per-thread 的，
  //    主執行緒那一份蓋不到 httplib worker。
  // ② 堆疊守衛空間 —— SetThreadStackGuarantee 同樣**只作用在呼叫它的那條執行緒**。
  //    堆疊溢位（0xC00000FD）進到 filter 時堆疊已經爆了，handler 自己沒空間跑；
  //    少了這 64 KB，那條執行緒的這一類當機**永遠寫不出報告**。
  //    原本這一行只寫在 installCrashHandler 裡，等於「主執行緒堆疊溢位有報告、
  //    httplib worker 堆疊溢位靜默消失」—— 而且從輸出上完全看不出漏了誰
  ULONG guard = 64 * 1024;
  ::SetThreadStackGuarantee(&guard);
  std::set_terminate(&onTerminate);
}

// C4717：底下的 Recurse::go 在所有控制路徑上都遞迴，必然把堆疊撐爆 ——
// 那正是 stackoverflow 這一項要的效果。不關掉的話每次建置都多一則警告，
// 久了就是養成「警告都可以忽略」的習慣
#pragma warning(push)
#pragma warning(disable : 4717)

void triggerCrashTestFromEnv() {
  char value[32] = "";
  if (::GetEnvironmentVariableA("L2M_CRASH_TEST", value, sizeof(value)) == 0) return;

  qWarning() << "[crash] L2M_CRASH_TEST=" << value << "：以下是刻意觸發的當機，不是真的 bug";

  if (std::strcmp(value, "av") == 0) {
    volatile int* p = reinterpret_cast<volatile int*>(8);
    *p = 1;
  } else if (std::strcmp(value, "uncaught") == 0) {
    // 沒人接的 C++ 例外。**實測走的是 onUnhandledException 而不是 onTerminate**
    //（0xE06D7363，理由見 crash_handler.h 的派送順序那一段），
    // 所以這一項刻意不叫 terminate —— 名字要跟它實際走的路一致
    throw std::runtime_error("L2M_CRASH_TEST=uncaught");
  } else if (std::strcmp(value, "systemerror") == 0) {
    // 跟 uncaught 走同一條派送路徑，差別只在丟的型別 —— 這一項釘住的是
    // **what() 那一行有沒有真的內容**。
    // 之所以挑 std::system_error：它最貼近本專案的歷史現場（CLAUDE.md「持鎖不得
    // emit」那則 0xC0000409，McpHttpServer::setStatus() 的 mutex 重入丟的就是它），
    // 而且光有型別名根本不知道是哪個 errno —— 正是「只拿型別名不夠」的活證據
    throw std::system_error(std::make_error_code(std::errc::device_or_resource_busy), "L2M_CRASH_TEST=systemerror");
  } else if (std::strcmp(value, "terminate") == 0) {
    // 沒有 SEH 例外在飛的 terminate。這是唯一走得到 onTerminate 的路，
    // 代表的是 noexcept 違規與解構期間丟例外那一類。
    // **abort() 不在這一類裡** —— 它走 __fastfail，四個進場點一個都攔不到，
    // 寫不出任何報告（見 crash_handler.h 的已知限制）
    std::terminate();
  } else if (std::strcmp(value, "purecall") == 0) {
    PureCallDerived d;  // 建構期間呼叫純虛擬 -> purecall handler
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
    qWarning() << "[crash] L2M_CRASH_TEST 認不得的值:" << value << "（可用：av / uncaught / systemerror / terminate / purecall /"
               << " invalidparam / stackoverflow）";
  }
}

#pragma warning(pop)

}  // namespace platform
}  // namespace l2m
