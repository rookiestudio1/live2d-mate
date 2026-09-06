#pragma once

// 行程異常結束時，把呼叫堆疊寫進 %APPDATA%/live2d_mate/logs/crash-*.log。
// 設計全文：docs/superpowers/specs/2026-08-31-crash-stack-logging-design.md
//
// **四個進場點，缺一個就有抓不到的死法：**
//   SetUnhandledExceptionFilter    0xC0000005 存取違規、0xC00000FD 堆疊溢位…
//   std::set_terminate             noexcept 違規、解構期間丟例外 —— 也就是
//                                  「沒有 SEH 例外在飛」卻要收掉行程的那兩條路。
//                                  **未捕捉的 throw 不走這裡**（理由見下一段），
//                                  **abort() 也不走這裡**（見已知限制）。
//   _set_purecall_handler          建構／解構期間呼叫純虛擬函式
//   _set_invalid_parameter_handler CRT 參數驗證失敗（要搭 _CrtSetReportMode，
//                                  否則 Debug 建置會先彈斷言對話框把行程卡住）
//
// **未捕捉的 C++ 例外走的是 unhandled exception filter，不是 terminate handler。**
// 這是 2026-08-31 用 L2M_CRASH_TEST 實跑出來的結果，不是從文件推論的 ——
// 設計文件 §7 原本寫「0xC0000409 只有 std::set_terminate 抓得到」，實測直接推翻了它。
// 真正的派送順序是：throw 找不到 handler 就一路展開到堆疊頂端，CRT 的 _seh_filter_exe
// 接著呼叫 OS 的 UnhandledExceptionFilter()，於是 onUnhandledException 先接到、
// 當場收掉行程 —— std::terminate 連跑的機會都沒有，那條
// terminate -> abort -> __fastfail 的路自然也走不到。
// 報告上看到的是 0xE06D7363（MSVC C++ 例外的 SEH 編碼 'msc' | 0xE0000000）。
//
// 「__fastfail 繞過 vectored handler 與 UEF、直接交給 WER」這句本身沒錯，
// 錯的是「未捕捉的 throw 會走到那裡」。而且實際行為比原設計好：攔得更早、
// reason 更精確（CPP_EXCEPTION 而不是籠統的 STACK_BUFFER_OVERRUN），
// 第二階段要取的 what() 這時也還拿得到。
// **所以四個進場點一個都不能拆掉** —— set_terminate 保的是上表那兩條路，
// 只是它不再是「未捕捉例外」的那一個。L2M_CRASH_TEST 的 uncaught 與 terminate
// 兩項就是分別釘住這兩條派送路徑的迴歸測試，**改這一段之前請先各跑一次**。
//
// 刻意**不用** AddVectoredExceptionHandler：它會在每一個 first-chance 例外被呼叫，
// 包括正常被 catch 掉的 C++ throw，以及 DirectWrite 字型 fallback 那類 Windows 內部
// 例外 —— 成本高、雜訊大，而且會把正常運作誤記成當機。
//
// **terminate handler 在 MSVC 上是 per-thread 的。** 主執行緒設了不代表別條有。
// 本專案三條執行緒（見 CLAUDE.md「執行緒模型」）：
//   · GUI 執行緒：`installCrashHandler` 在主執行緒裝的四個進場點完全覆蓋
//   · miniaudio 即時執行緒：依既有鐵律不配置不上鎖不碰 Qt，本來就不該有例外
//   · httplib worker pool：各執行緒在「服務第一個 POST /mcp 請求」時呼叫
//     `installThreadCrashSupport()`（由 POST handler 開頭的 `thread_local`
//     旗標駕馭），裝上該執行緒自己的 terminate handler **與堆疊守衛空間** ——
//     兩者都是 per-thread 的，讓該執行緒的例外與堆疊溢位也能走到 crash 報告。
//     額外的 `set_exception_handler`
//     攔的是我們自己的 route handler 執行期間的例外，回 500 給 client 而不是讓
//     httplib 自己的例外處理留下斷線。
//
// **寫檔分兩階段。** handler 絕對不能走 spdlog 那條路：DailyRotatingFileSink 有
// std::mutex，spdlog 格式化訊息又會配置記憶體 —— 崩在那個鎖上或崩在 heap 損毀上時，
// 「寫 log」這個動作本身就會二次死掉。所以：
//   第一階段：只用不配置、不上鎖拿得到的東西（CaptureStackBackTrace /
//             RtlVirtualUnwind、安裝時快照的 exe 基底、WriteFile），寫完立刻
//             FlushFileBuffers。**光這些就足以離線精確還原到 file:line。**
//   第二階段：C++ 例外的型別與 what()、DbgHelp 符號化、模組名。會配置、會上鎖、
//             可能失敗，但失敗只損失好看的那半邊。
//             **兩者的順序是刻意的：例外資訊在前、符號化在後。** 前者只是幾次指標
//             讀取卻最有診斷價值，後者要載入幾十 MB 的 pdb、要進 DbgHelp 的內部鎖，
//             是這一階段最可能卡住或再崩一次的部分。
//             **第二階段有 10 秒硬上限**（crash_handler_win.cpp 的 phase2Watchdog）：
//             允許配置與上鎖就等於允許死鎖 —— 崩在 CRT heap lock 上（也就是
//             「崩在 malloc/free 裡」，正是這個功能最想活下來的那一家當機）時，
//             what()／UnDecorateSymbolName／DbgHelp 任何一支都可能當場卡住，
//             TerminateProcess 永遠走不到，使用者會得到一隻**凍住卻還在畫面上**
//             的桌寵 —— 那比完全沒有 handler 更糟。所以安裝時就備好一條常駐的
//             看門狗執行緒，第一階段落地後 SetEvent 上膛，逾時就代為 TerminateProcess。
//
// **C++ 例外的訊息不能用 std::current_exception() 取** —— 2026-08-31 實測踩到的坑。
// 它在 unhandled exception filter 裡**永遠回 null**：MSVC 是進入 catch handler 時
//（__InternalCxxFrameHandler → CatchIt）才把例外寫進 per-thread data 的
// _curexception，而 last-chance filter 發生在展開**之前**、從來沒有進過任何 catch。
// 症狀是「--- exception --- 整段永遠不出現」而且沒有任何錯誤訊息 —— **當時的六種**
// L2M_CRASH_TEST 沒有一種抓得到，連 uncaught 也一樣（那時堆疊還停在 throw 那一行，
// 正好證明我們是在展開前被叫到的）。（systemerror 是修好之後才補的第七種，
// 它釘住的正是這個坑修對了沒有 —— what() 那一行有沒有真的內容。）
// 所以改成直接讀 EXCEPTION_RECORD：0xE06D7363 的 ExceptionInformation 帶著
// [0] magic、[1] 被丟出的物件、[2] ThrowInfo、[3] 模組基底（x64 的 ThrowInfo
// 內部全是 RVA，要靠它還原）。型別名走 ThrowInfo → CatchableTypeArray[0] →
// TypeDescriptor，是純讀靜態唯讀資料；what() 則只在 CatchableTypeArray 裡真的
// 找得到 std::exception 時才呼叫，兩步各自包一層 __try。細節與理由寫在
// crash_handler_win.cpp 的 ThrowInfoRec 那一段。
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
//   · **直接 __fastfail 的死法四個進場點一個都攔不到，寫不出任何報告。**
//     abort()（UCRT 走 __fastfail(FAST_FAIL_FATAL_APP_EXIT)）與 /GS cookie
//     檢查失敗都屬於這一類 —— __fastfail 依設計繞過 vectored handler 與
//     unhandled exception filter，也不經過 terminate handler，直接交給 WER。
//     （注意方向：abort() 不會呼叫 terminate handler，是 std::terminate 的
//     預設 handler 才去呼叫 abort。）要接得另外裝 SIGABRT handler，
//     那是設計變更，不在目前四個進場點的範圍內。
//   · **堆積損毀也多半走 __fastfail，所以多半一份報告都沒有。** 這一條要單獨講，
//     因為它跟這個功能的動機案例（設計文件 §1 的記憶體損毀那一家）**正面衝突**，
//     而且 crashReasonText 裡確實有一筆 "0xC0000374 HEAP_CORRUPTION"，讀起來
//     像是在承諾涵蓋 —— 不是。Windows 10/11 的多項堆積完整性檢查（釋放時的
//     header／encoded pointer 檢驗、LFH bucket 驗證…）觸發時走的是
//     RtlFailFast(FAST_FAIL_HEAP_METADATA_CORRUPTION)，跟 abort() 同一條路，
//     四個進場點一個都碰不到。真的丟出 STATUS_HEAP_CORRUPTION(0xC0000374) 這個
//     SEH 例外的路徑存在但少見，那一筆是留給它的。**實務上的結論：堆積被寫壞時
//     多半是「什麼都沒留下」，而不是「報告寫不完整」。** 那一家當機真正要靠的是
//     頁堆積（gflags /p）或 ASan 這種在損毀當下就攔住的工具，不是事後的報告。
//   · **報告裡凡是帶 detail 括號的 reason 碼，都是我們自己合成的。** 目前有兩個：
//     0xC0000409 STACK_BUFFER_OVERRUN（onTerminate 的 "(std::terminate)" 與
//     onInvalidParameter 的 "(invalid CRT parameter)"）與 0xC0000025
//     NONCONTINUABLE_EXCEPTION（onPureCall 的 "(purecall)"）。兩個都不是真的
//     發生了那個 SEH 例外 —— 那三個進場點手上根本沒有 EXCEPTION_RECORD，
//     碼是寫死的。所以看到 STACK_BUFFER_OVERRUN 卻找不到任何緩衝溢位、
//     看到 NONCONTINUABLE_EXCEPTION 卻沒有任何不可繼續的例外，都是正常的。
//     **判斷規則就一條：有括號 ＝ 我們合成的，沒括號 ＝ 作業系統真的給的。**
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

// **這條執行緒需要的兩件事**（兩件都是 per-thread 的，主執行緒設的那一份蓋不到別條）：
//   ① terminate handler —— MSVC 的 std::set_terminate 是 per-thread 的。
//   ② 堆疊守衛空間 —— SetThreadStackGuarantee 只作用在呼叫它的那條執行緒；
//      少了它，該執行緒的堆疊溢位（0xC00000FD）永遠寫不出報告。
// httplib worker 執行緒各自呼叫一次（見 CLAUDE.md「執行緒模型」與 mcp_http_server.cpp）；
// installCrashHandler 自己也呼叫它，主執行緒與 worker 才不會有兩份會漂移的程式碼。
//
// 名字從 installThreadTerminateHandler 改過來，就是因為它已經不只裝 terminate handler
void installThreadCrashSupport();

// L2M_CRASH_TEST 的觸發：av / uncaught / systemerror / terminate / purecall /
// invalidparam / stackoverflow。沒設或認不得就什麼都不做（認不得時印一行 qWarning）。
//
// uncaught（沒人接的 throw）與 terminate（直接 std::terminate()）**是兩項不是一項**：
// 前者實測走 onUnhandledException，後者才走 onTerminate。合成一項的話，
// 四個進場點裡就會有一個完全沒有測試覆蓋（理由見上面的派送順序那一段）。
//
// systemerror 跟 uncaught 走同一條派送路徑，差別只在丟的型別 —— 它釘住的是
// **--- exception --- 的 what() 那一行有沒有真的內容**。挑 std::system_error 是因為
// 它最貼近本專案的歷史現場（CLAUDE.md「持鎖不得 emit」那則 0xC0000409），
// 而且光有型別名不知道是哪個 errno，正是「只拿型別名不夠」的活證據。
//
// 這是驗證 handler 唯一可行的手段 —— 「守衛空間夠不夠」「重入防護有沒有效」
// 「精簡 pdb 解不解得出函式名」都只有真的崩一次才知道。**release 版刻意留著**：
// 現場排查時「使用者那台機器上 handler 到底有沒有在運作」是會真的遇到的問題。
void triggerCrashTestFromEnv();

}  // namespace platform
}  // namespace l2m
