# 當機堆疊寫進 log 檔 — 設計

日期：2026-08-31
狀態：設計已定案，待排實作計畫

---

## 1. 問題

這個 app 有三種真實發生過、而且**現場什麼都沒留下**的死法：

| 現場 | 錯誤碼 | 根因 |
|---|---|---|
| AI 擴寫的預覽框按下按鈕（commit 99d740c） | `0xC0000005` | 存取違規 |
| `McpHttpServer::setStatus()` 持鎖 emit（commit 565d928） | `0xC0000409` | 非遞迴 mutex 重入 → `std::system_error` 沒人接 → `terminate` |
| 「藿藿.zip」的非法 UTF-8 條目名（CLAUDE.md 的 model_assets 段） | `0xC0000409` | `std::filesystem::path` 丟例外沒人接 |

三次都只能靠事後推理與反覆重現去找。上一次甚至得靠「低 16 bit + `dumpbin` 反推函式」才勉強定位。

目標：**行程異常結束時，把呼叫堆疊寫進磁碟，而且拿到那份檔案的人看得懂。**

## 2. 現況（可以重用的東西）

「寫進 log 檔」這半邊已經完全就緒，不需要新的基礎建設：

- `src/app/logging.cpp` — spdlog 骨架、`DailyRotatingFileSink`。**每則訊息都 flush**
  （`logging.cpp:134` 的註解明說理由就是「這個 app 有 0xC0000409 直接 abort 的前科」）。
- `src/core/log_rotation.h`、`src/core/log_category.h` — 純邏輯在 `l2m_core`，有測試釘住。
- log 目錄 `%APPDATA%/live2d_mate/logs/`，由 `attachLogFile()`（`main.cpp:210`）建立。

真正缺的只有一件事：**例外發生的當下，去哪裡把呼叫堆疊撈出來。**

已知缺口：`CMakeLists.txt:526` 的 install 只裝 exe，**沒有裝任何符號檔**。

## 3. 四個定案的決策

| # | 決策 | 選擇 |
|---|---|---|
| 1 | 涵蓋範圍 | **全部四種死法**：SEH 硬當機、未捕捉 C++ 例外、purecall、CRT 參數驗證失敗 |
| 2 | 符號策略 | **精簡 pdb 隨 exe 出貨 + 完整 pdb 掛 release 資產** |
| 3 | 使用者提示 | **當下只寫檔，下次啟動再提示**（handler 絕不碰 UI） |
| 4 | crash 檔內容 | **只有堆疊與現場**，不附一般 log（一般 log 每則都 flush，就在同一個資料夾） |

## 4. 不採用的做法（連同理由，避免日後重新討論）

**C++23 `std::stacktrace`** — `CMakeLists.txt:22` 是 `CMAKE_CXX_STANDARD 17`。但標準不夠只是次要理由：
它內部會 `std::vector` 配置記憶體，而 **heap 損毀正是最常見的當機原因之一**，那會在「還沒寫出任何東西」
的狀態下二次死。就算升上 C++23 也不該在 handler 裡用它。

**Crashpad / Breakpad** — 業界標準沒錯，但 Crashpad 要 GN + depot_tools 才建得起來。
`third_party/` 現在全是 header-only 或單檔塞進來的（httplib、miniaudio、miniz、yyjson、spdlog、glew），
代價與本專案的規模完全不成比例。

**Boost.Stacktrace** — `basic` 後端就是包一層 `CaptureStackBackTrace`，`windbg` 後端就是包 DbgHelp。
等於為了我們自己寫得出來的兩段程式碼去背一個 Boost 相依。

**`AddVectoredExceptionHandler`** — 它會在**每一個** first-chance 例外被呼叫，包括正常被 `catch` 掉的
C++ `throw`，以及 CLAUDE.md 提過的 DirectWrite 字型 fallback 那類 Windows 內部例外。
成本高、雜訊大，而且會把「正常運作」誤記成當機。

**`StackWalk64`（DbgHelp）做擷取** — 它會配置記憶體、會進 DbgHelp 的內部鎖，不符合第一階段
「一定寫得出來」的要求。x64 有更低階也更安全的替代（見 §6）。

**minidump（`.dmp`）** — 暫不做。文字堆疊 + 精確符號化已經能解掉上述三種歷史現場；
`.dmp` 要教使用者傳檔，而在已經壞掉的行程裡寫 dump 又得另起子行程才穩。
真的遇到「看堆疊還是查不出來」再加，屆時是獨立的一次改動。

## 5. 兩階段寫入（核心設計）

crash handler **絕對不能走 spdlog 那條路**：`DailyRotatingFileSink` 有 `std::mutex`，而且 spdlog
格式化訊息會配置記憶體。若正好崩在持有那個鎖的執行緒上（或崩在 heap 損毀上），寫 log 這個動作
本身就會二次死掉。所以 crash 現場一定是**自己開一個檔、用最原始的 API 寫**。
`crash-*.log` 是結構上的必然，不是偏好。

在此前提下，handler 分兩階段：

**第一階段 — 保證寫得出來。** 只用「不配置記憶體、不上任何鎖」拿得到的東西：錯誤碼與旗標、
執行緒 id、原始堆疊位址、exe 的基底/大小/PDB GUID/age。寫完立刻 `FlushFileBuffers`。
**光這些就足以離線精確還原到 `file:line`**（見 §9）。

**第二階段 — 盡力而為。** DbgHelp 符號化、模組名解析、C++ 例外的 `what()` 訊息，append 上去。
這一段會配置記憶體、會上鎖、可能失敗——**但失敗只損失「好看」的那半邊**，
可離線還原的資訊已經在磁碟上了。

## 6. 擷取：Windows 上的 `backtrace` 等價物

`CaptureStackBackTrace`（kernel32，實為 `RtlCaptureStackBackTrace`）就是 glibc `backtrace()` 的
Windows 等價物：給它一個 `PVOID` 陣列，回填返回位址。在 x64 上它走 PE 的 unwind table（`.pdata`）
展開，**不需要 frame pointer**，所以 `/O2` 全開的最佳化建置照樣抓得準。不配置、不上鎖、不需要 DbgHelp。

Windows 缺的是另一半：**沒有 `backtrace_symbols()`**。位址轉函式名/行號一律得走 DbgHelp。
擷取與符號化在這個平台上本來就是兩組性質完全不同的 API——這正是 §5 那條分界線。

`CaptureStackBackTrace` 只能抓「呼叫它的當下這條堆疊」，餵不進現成的 `CONTEXT`。
對照四個進場點，只有 SEH filter 需要從 `ExceptionInfo->ContextRecord` 開始走，
而那個也**不必用 `StackWalk64`**：x64 有 `RtlLookupFunctionEntry` + `RtlVirtualUnwind`（ntdll，
正是 `CaptureStackBackTrace` 內部用的東西），從 `ContextRecord` 開始跑一個約 15 行的迴圈就能走完，
同樣不配置、不上鎖。

堆疊上限 `kMaxFrames = 62`。這個數字不是隨便挑的：`CaptureStackBackTrace` 在
Windows 8 以前的 x64 上就是卡在 62（`FramesToSkip + FramesToCapture < 63`），
兩個進場點用同一個上限，crash 檔的格式與 `core/crash_report.h` 的緩衝大小才只有一套。
實務上超過 62 層的堆疊只有無窮遞迴，而那種現場看最上面十層就夠了。

```cpp
// 一定要複製一份：RtlVirtualUnwind 會就地改寫 CONTEXT
CONTEXT ctx = *info->ContextRecord;
UNWIND_HISTORY_TABLE hist{};
for (int i = 0; i < kMaxFrames && ctx.Rip; ++i) {
  frames[n++] = reinterpret_cast<void*>(ctx.Rip);
  DWORD64 imageBase = 0;
  auto* fn = RtlLookupFunctionEntry(ctx.Rip, &imageBase, &hist);
  if (!fn) break;  // 葉函式或無 unwind 資料，走不下去
  PVOID handlerData = nullptr;
  DWORD64 establisher = 0;
  RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, ctx.Rip, fn, &ctx,
                   &handlerData, &establisher, nullptr);
}
```

這條路線成立的前提是 x64。32 位元 x86 沒有 unwind table，才非得靠 `StackWalk64`。
本專案是 `msvc2022_64`，不受影響——**但這也表示未來若真要出 32 位元版，這一段得重寫。**

## 7. 四個進場點

| 進場點 | 抓到什麼 | 擷取方式 |
|---|---|---|
| `SetUnhandledExceptionFilter` | `0xC0000005` 存取違規、`0xC00000FD` 堆疊溢位、`0xC000001D` 非法指令… | 複製 `ContextRecord` 後跑 §6 的展開迴圈 |
| `std::set_terminate` | `noexcept` 違規、解構期間丟例外——「沒有 SEH 例外在飛」卻要收掉行程的那兩條路（**不含未捕捉的 `throw`**，也**不含 `abort()`**，都見下） | `CaptureStackBackTrace` |
| `_set_purecall_handler` | 建構／解構期間呼叫純虛擬函式 | `CaptureStackBackTrace` |
| `_set_invalid_parameter_handler` | CRT 參數驗證失敗 | `CaptureStackBackTrace` |

**四個進場點缺一不可，但機制跟本節初稿寫的不一樣——以下是 2026-08-31 Task 4 的實測修正。**

初稿寫「未捕捉的 C++ 例外 → `abort` → `0xC0000409`，必須靠 `std::set_terminate` 才抓得到」。
`L2M_CRASH_TEST` 實跑推翻了這一句：未捕捉的 `throw` **走的是 unhandled exception filter**，
報告上是 `0xE06D7363`（MSVC C++ 例外的 SEH 編碼 `'msc' | 0xE0000000`）。
真正的順序是——`throw` 找不到 handler 就一路展開到堆疊頂端，CRT 的 `_seh_filter_exe`
呼叫 OS 的 `UnhandledExceptionFilter()`，`SetUnhandledExceptionFilter` 註冊的那支先接到、
當場收掉行程，`std::terminate` 連跑的機會都沒有，`terminate` → `abort` → `__fastfail`
那條路也就走不到。

「`__fastfail` 刻意繞過 vectored handler 與 unhandled exception filter、直接交給 WER」
這句本身沒錯，錯的是「未捕捉的 `throw` 會走到那裡」。實際行為比原設計好：攔得更早、
reason 更精確（`CPP_EXCEPTION` 而不是籠統的 `STACK_BUFFER_OVERRUN`），
而且 §9 第二階段要取的 `what()` 這時還拿得到。

**`abort()` 也不走 `std::set_terminate`——方向是相反的。** `std::terminate` 的預設 handler
才去呼叫 `abort`，不是 `abort` 去呼叫 terminate handler。而 UCRT 的 `abort()` 走
`__fastfail(FAST_FAIL_FATAL_APP_EXIT)`，跟上面保留的那句「`__fastfail` 繞過 vectored handler
與 UEF、直接交給 WER」合起來就是：**`abort()` 既不進 `onTerminate` 也不進
`onUnhandledException`，寫不出任何報告**（`/GS` cookie 檢查失敗同理）。列進 §17 已知限制。
要接得另外裝 `SIGABRT` handler，那是設計變更，不在目前四個進場點的範圍內。

**另外提醒一個容易誤導的地方**：本專案報告裡**凡是帶 detail 括號的 reason 碼都是我們自己
合成的**——`0xC0000409`（`onTerminate`／`onInvalidParameter`）與 `0xC0000025`（`onPureCall`）
兩者都是。那三個進場點手上根本沒有 `EXCEPTION_RECORD`，碼是寫死的，真正的 `__fastfail`
也走不到這裡。這就解釋了為什麼 reason 會寫 `STACK_BUFFER_OVERRUN` 卻找不到任何緩衝溢位、
寫 `NONCONTINUABLE_EXCEPTION` 卻沒有任何不可繼續的例外。詳見 §17。

**結論不變：四個進場點一個都不能拆。** `std::set_terminate` 保的是上表那兩條
「沒有 SEH 例外在飛」的路（`noexcept` 違規／解構期間丟例外），
只是它不再是「未捕捉例外」的那一個。`L2M_CRASH_TEST` 因此拆成 `uncaught`
（沒人接的 `throw`）與 `terminate`（直接 `std::terminate()`）兩項，
分別釘住這兩條派送路徑——合成一項的話 `onTerminate` 會完全沒有測試覆蓋。

`_set_invalid_parameter_handler` 要搭配 `_CrtSetReportMode(_CRT_ASSERT, 0)`，
否則 Debug 建置會先彈斷言對話框把行程卡住。

### 7.1 兩個 MSVC 特有的坑

**① terminate handler 在 MSVC 上是 per-thread 的。** 主執行緒設了不代表其他執行緒有。
對照專案的三條執行緒模型（CLAUDE.md「執行緒模型」段）：

- **GUI 執行緒** — 幾乎所有邏輯都在這裡，完整受保護。
- **miniaudio 即時音訊執行緒** — 依既有鐵律「不配置記憶體、不上鎖、不碰 Qt」，本來就不該有例外，風險極低。
- **httplib worker pool** — 需要執行緒層級的 terminate handler。MSVC 的 `std::set_terminate` 是
  per-thread 的，主執行緒那份蓋不到 httplib worker。
  
  `Server::set_exception_handler` 只攔我們自己註冊的 route handler 執行期間逸出的例外
  （回 500 給 client），**不涵蓋** httplib 自己的 request-line／header 解析、Range 解析、
  `Expect: 100-continue`、路由成功後的檔案輸出、以及 `ThreadPool::worker::operator()` 呼叫 `fn()`
  那一層（那裡完全沒有 try/catch）。
  
  真正補起缺口的是 `installThreadCrashSupport()`（`src/platform/crash_handler.h`）：
  在 POST handler 開頭用 `thread_local` 旗標裝一次，之後該執行緒**任何位置**逸出的例外都會
  走到同一份 crash 報告。**它裝的是兩件事**（terminate handler 與堆疊守衛空間，見 ② ），
  函式原名 `installThreadTerminateHandler` 就是因為只做了一半才改名的。
  
  **殘留缺口明講**：一條全新的 worker 執行緒在服務**第一個**請求的解析階段丟例外仍攔不到
  ——那時 handler 還沒裝上去。實務上 pool 執行緒都會服務請求，暖機後即全數覆蓋。
  
  此判斷基於實作期查證 `third_party/httplib/httplib.h` 的 `exception_handler_` 實裝
  （`:7330-7360` 只包住 `routed = routing(req, res, strm);` 一行）及 `ThreadPool::worker::operator()`
  （`:831` 無 try/catch）。

**② 堆疊溢位時 handler 自己沒空間跑。** `0xC00000FD` 進到 filter 時堆疊已經爆了。
安裝時呼叫 `SetThreadStackGuarantee(64 KB)` 預留守衛空間，否則這一類當機**永遠寫不出報告**。

**`SetThreadStackGuarantee` 跟 ① 的 terminate handler 一樣是 per-thread 的**，
所以它跟 ① 綁在同一支 `installThreadCrashSupport()` 裡，主執行緒與每一條 httplib worker
各呼叫一次。只裝主執行緒的話，症狀是「主執行緒堆疊溢位有報告、worker 堆疊溢位靜默消失」
——而且從輸出上完全看不出漏了誰。**兩件事寫成兩份程式碼就一定會漂移**，所以
`installCrashHandler()` 自己也是呼叫這一支，不另外抄一份。

## 8. 模組基底：只快照 exe 自己

第一階段要寫出可離線還原的 `module+RVA`，就得知道模組基底。但 `EnumProcessModules` 與
`GetModuleHandleEx` 都會取 **loader lock**——崩在 loader lock 上時會直接死鎖。

解法是把範圍縮到最小：**安裝 handler 時取一次 `live2d_mate.exe` 的基底與 `SizeOfImage`**
（`GetModuleHandleW(nullptr)` + PE 標頭），存成兩個整數。第一階段對每個位址只做一次區間判斷：

- 落在 `[base, base + size)` → 寫 `live2d_mate.exe+0xRVA`
- 否則 → 原樣寫絕對位址

**第一階段因此變成純算術 + `WriteFile`，一個可能上鎖的 API 都不呼叫。**
其他模組（Qt、系統 DLL）反正沒有 pdb，留到第二階段用 `GetModuleHandleEx` 補名字就好，
而那些正是「就算解不出來也不影響定位」的 frame。

**但第二階段那一半非補不可，不是可有可無的裝飾**（`writeSymbols` 的 `moduleOfAddress`）：
第一階段對非 exe 的位址只寫得出絕對位址，而 ASLR 讓那串數字每次載入都不同——
**拿到 log 的人事後再也還原不了它屬於誰**，§9 承諾的「死在誰家看得出來」就不成立了。
輸出形式見 §10（`Qt6Core.dll+0x000E24C1`，跟 `stack :` 段逐行對齊）。
只取檔名不取完整路徑：完整路徑會把建置機器的目錄結構寫進使用者寄回來的檔案。

exe 不會被卸載或搬移，所以這份快照永遠有效。

## 9. 符號策略

MSVC 的 `RelWithDebInfo` **不會**把行號資訊放進 exe：`/Zi /DEBUG` 產生的是獨立的 `.pdb`，
exe 只留一個 CodeView debug directory 項（pdb 路徑字串 + GUID + age）。
這跟 MinGW/Clang 的 DWARF 不同（那個確實嵌在 exe 裡），但本專案是 MSVC2022，沒得選。

不帶符號檔時實際拿得到的是：**自己的程式碼只有 `live2d_mate.exe+0x1a2b3c`**（exe 幾乎沒有
export table），但 **Qt 與系統 DLL 反而解得出來**（DbgHelp 會退回讀 export table）。
也就是「死在誰家」看得出來，「死在哪一行」看不出來。

實測本機的 pdb 大小：`build/rel/live2d_mate.pdb` **168 MB**、
`build/Desktop_Qt_6_11_2_MSVC2022_64bit_RelWithDebInfo/live2d_mate.pdb` **84 MB**。
完整 pdb 隨 zip 出貨太肥，所以採兩層：

**① 精簡 pdb 隨 exe 出貨。** `/PDBSTRIPPED` 產一份只有 public symbols（函式名，無行號與型別）
的 pdb，估 3–8 MB。執行期 DbgHelp 就解得出函式名，八成的回報到這裡就結案。

**② 完整 pdb 掛 release 獨立資產。** crash 檔記了 RVA 與 PDB GUID/age，
用 `llvm-symbolizer` 或 WinDbg 離線還原出精確的 `file:line`。
**這跟以前靠 `dumpbin` 反推完全不同——這是對得上 GUID 的精確解析，不是猜。**

### 9.1 一個弄錯就整個功能無效的細節

DbgHelp 是靠 exe 的 debug directory 裡記的 **pdb 檔名 + GUID + age** 去找符號檔的。
所以 `/PDBSTRIPPED:live2d_mate.stripped.pdb` 產出的檔案，**出貨時必須改名成 `live2d_mate.pdb`**
放在 exe 旁邊，否則 DbgHelp 根本不會去看它。精簡 pdb 與完整 pdb 出自同一次連結，GUID/age 相同，
所以兩者都對得上同一份 crash 報告。

## 10. crash 檔

檔名 `crash-YYYYMMDD-HHMMSS-<pid>.log`，放**同一個** `%APPDATA%/live2d_mate/logs/`——
請使用者回報時整個資料夾壓起來就好，不必解釋兩個位置。
帶 pid 是因為主行程／`--mcp-stdio` 橋接／`--splash` 可能同秒一起死。

**「最新」一律以檔名的字典序判定，不看 mtime。** 檔名裡的 `YYYYMMDD-HHMMSS` 是零填補的固定寬度，
字典序即時間序；而 mtime 會被複製、還原、同步工具改掉。§11 的清理與 §12 的提示掃描共用這個定義。
同秒多份時 pid 決定先後——那是任意但穩定的順序，兩個不同行程的當機沒有「誰比較新」可言。

UTF-8 無 BOM，走 `WriteFile` 直接寫，**不經 CRT stdio**（`fprintf` 會配置、會上鎖）。

**底下兩段都是逐字貼上的真實報告，不是示意。** 這一節是日後維護者拿新報告來對照的基準，
所以捏造的樣本會直接害人 —— 例如原稿寫過 `reason : SEH 0xC0000005 …`（實作沒有 `SEH ` 前綴）
與一組不對應任何真實輸出的 `--- symbolized ---`。要更新時請重跑 `L2M_CRASH_TEST` 再貼一次。

第一階段（`crash-20260831-160546-25588.log`，`L2M_CRASH_TEST=av`，`build/rel` RelWithDebInfo）：

```
=== live2d_mate crash ===
version : 1.0.0  (RelWithDebInfo)
process : live2d_mate.exe  pid 25588  mode=main
time    : 2026-08-31T16:05:46+08:00
reason  : 0xC0000005 ACCESS_VIOLATION writing 0x0000000000000008
thread  : 23800
image   : live2d_mate.exe base=0x00007FF701B80000 size=0x008B6000
pdb     : 363CEC20-7EF5-4788-8AAF-9F412360CF6C age=145
stack   :
  #00 live2d_mate.exe+0x00180188
  #01 0x00007FFCBF1124C1
  #02 0x00007FFCBF114FD4
  #03 0x00007FFCBF12A091
  #04 0x00007FFCBF11C226
  #05 0x00007FFCD3B534C2
  #06 0x00007FFCD3B525F6
  #07 0x00007FFCBF0D3B8F
  #08 0x00007FFCBF263138
  #09 0x00007FFCBF261443
  #10 0x00007FFDB480C366
  #11 0x00007FFDB480A7BD
  #12 0x00007FFCBF260D14
  #13 0x00007FFCB9438619
  #14 0x00007FFCBF0DA694
  #15 0x00007FFCBF0D2082
  #16 live2d_mate.exe+0x0002B431
  #17 live2d_mate.exe+0x00423810
  #18 live2d_mate.exe+0x0040F6AA
  #19 0x00007FFDB475CCB7
  #20 0x00007FFDB58EAD6C
--- symbolized ---
  #00 live2d_mate.exe+0x00180188  l2m::platform::triggerCrashTestFromEnv+0xF8  crash_handler_win.cpp:1086
`version` 的括號是建置型別（`L2M_BUILD_CONFIG`，`CMakeLists.txt` 用 `$<CONFIG>` 帶進來）。
**這是診斷欄位不是裝飾**：Debug 與 RelWithDebInfo 在
「`_set_invalid_parameter_handler` 拿不拿得到 expression／file／line」與
「inline 把多少層 frame 收掉」上差很多，而使用者寄回來的報告不會附帶「我裝的是哪一種建置」。

第二階段（append；`crash-20260831-160554-28600.log`，`L2M_CRASH_TEST=systemerror`，同一批）：

```
--- exception ---
  type    : class std::system_error
  what    : L2M_CRASH_TEST=systemerror: device or resource busy
--- symbolized ---
  #00 0x00007FFDB322187A  KERNELBASE.dll+0x000C187A  (no symbol)
  #01 0x00007FFD9D6152C7  VCRUNTIME140.dll+0x000052C7  (no symbol)
  #02 live2d_mate.exe+0x0018033F  l2m::platform::triggerCrashTestFromEnv+0x2AF  crash_handler_win.cpp:1098
  #03 0x00007FFCBF1124C1  Qt6Core.dll+0x000E24C1  (no symbol)
  #04 0x00007FFCBF114FD4  Qt6Core.dll+0x000E4FD4  (no symbol)
  #05 0x00007FFCBF12A091  Qt6Core.dll+0x000FA091  (no symbol)
  #06 0x00007FFCBF11C226  Qt6Core.dll+0x000EC226  (no symbol)
  #07 0x00007FFCD3B534C2  Qt6Widgets.dll+0x000134C2  (no symbol)
  #08 0x00007FFCD3B525F6  Qt6Widgets.dll+0x000125F6  (no symbol)
  #09 0x00007FFCBF0D3B8F  Qt6Core.dll+0x000A3B8F  (no symbol)
  #10 0x00007FFCBF263138  Qt6Core.dll+0x00233138  (no symbol)
  #11 0x00007FFCBF261443  Qt6Core.dll+0x00231443  (no symbol)
  #12 0x00007FFDB480C366  USER32.dll+0x0000C366  (no symbol)
  #13 0x00007FFDB480A7BD  USER32.dll+0x0000A7BD  (no symbol)
  #14 0x00007FFCBF260D14  Qt6Core.dll+0x00230D14  (no symbol)
  #15 0x00007FFCB9438619  Qt6Gui.dll+0x003A8619  (no symbol)
  #16 0x00007FFCBF0DA694  Qt6Core.dll+0x000AA694  (no symbol)
  #17 0x00007FFCBF0D2082  Qt6Core.dll+0x000A2082  (no symbol)
  #18 live2d_mate.exe+0x0002B431  main+0x3491  main.cpp:682
  #19 live2d_mate.exe+0x00423810  qtEntryPoint+0x30  qtentrypoint_win.cpp:45
  #20 live2d_mate.exe+0x0040F6AA  __scrt_common_main_seh+0x106  exe_common.inl:288
  #21 0x00007FFDB475CCB7  KERNEL32.DLL+0x0002CCB7  (no symbol)
  #22 0x00007FFDB58EAD6C  ntdll.dll+0x000AAD6C  (no symbol)
```

**中間那一欄 `Qt6Core.dll+0x000E24C1` 是模組名與模組內 RVA**，只有第二階段寫得出來
（`GetModuleHandleEx` 會取 loader lock，第一階段的禁用清單指名了它，見 §8）。
沒有它的話，非 exe 的 15 格就只剩一串絕對位址 —— **ASLR 之下事後再也還原不了它們屬於誰**，
而 §9 把「死在誰家看得出來」列為沒有符號檔時仍然拿得到的東西。這個缺口事後補不回來：
log 沒記模組名，位址又每次載入都不同。取不到模組（位址不在任何已載入影像裡）就維持
只有絕對位址，不去猜。只取檔名不取完整路徑，理由同下面行號那一欄。

出貨版面（`cmake --install` 出來的目錄，旁邊只有精簡 pdb）跑同一個 `av` 的結果
**函式名有、行號沒有** —— 以下三行節錄自 `crash-20260831-160723-28500.log`
（行號欄整份都不存在，不是這裡省略掉的；`(no symbol)` 那些格子則跟開發樹一字不差）：

```
  #00 live2d_mate.exe+0x00180188  l2m::platform::triggerCrashTestFromEnv+0xF8
  #01 0x00007FFCBF1124C1  Qt6Core.dll+0x000E24C1  (no symbol)
  #16 live2d_mate.exe+0x0002B431  main+0x3491
```

**`--- symbolized ---` 的每一行都重印位址，而且跟上面 `stack   :` 段用同一支
`formatFrame`**——兩段的第 *i* 行位址一字不差，讀報告的人不必自己在心裡把兩份清單對起來。
沒有 pdb 的模組一律寫 `(no symbol)`，**不去拿「最接近的匯出符號」充數**：那種名字經常是錯的，
比誠實的 `(no symbol)` 更會把人帶去查一條不存在的路徑。

**查詢符號前，回傳位址一律先減 1。** `#01` 以後的每一格都是「`call` 的下一道指令」的位址；
那個 `call` 若剛好是某一行（或某個函式）的最後一道指令，直接查就會歸到下一行、甚至下一個函式。
實測 `purecall` 的呼叫點：原值查到第 748 行（`if` 鏈裡**另一個** `else if` 分支），
減 1 才是第 746 行的建構敘述。**唯一的例外是 SEH 路徑的 `#00`**——那一格是 `ctx.Rip`，
是肇事指令本身而不是回傳位址，減了反而錯（`av` 原值 733 是 `*p = 1;`，減 1 會變成 731）。
減 1 只用於查詢，印出來的位址一律是原值。

### C++ 例外的型別與 `what()`

刻意放第二階段（會配置、會上鎖），但**排在符號化之前**：它只是幾次指標讀取，
卻常常是整份報告最有價值的一行——`0xC0000409` 那種現場，`what()` 往往比整個堆疊還關鍵；
而符號化要載入幾十 MB 的 pdb、要進 DbgHelp 的內部鎖，是這一階段最可能卡住或再崩一次的部分。
**風險最高的不該擋在價值最高的前面。**

**不能用 `std::current_exception()`**——2026-08-31 實測踩到的坑，本文件原先寫錯了。
它在 unhandled exception filter 裡**永遠回 null**：MSVC 是進入 catch handler 時
（`__InternalCxxFrameHandler` → `CatchIt`）才把例外寫進 per-thread data 的 `_curexception`，
而 last-chance filter 發生在展開**之前**、從來沒有進過任何 catch。
症狀是「`--- exception ---` 整段永遠不出現」而且沒有任何錯誤訊息——**當時的六種**
`L2M_CRASH_TEST` 沒有一種抓得到，連 `uncaught` 也一樣（那時堆疊還停在 `throw` 那一行，
正好證明 handler 是在展開前被叫到的）。（`systemerror` 是修好之後才補的第七種，
釘住的正是這個坑修對了沒有。）

改成直接讀 `EXCEPTION_RECORD`。`0xE06D7363` 的 `ExceptionInformation` 帶著
`[0]` magic（`0x19930520`）、`[1]` 被丟出的物件、`[2]` `ThrowInfo`、`[3]` 模組基底
（x64 的 `ThrowInfo` 內部全是 32 位元 RVA，要靠它還原成指標）。分兩步、各自包一層 `__try`：

1. **型別名**：`ThrowInfo` → `CatchableTypeArray[0]`（最衍生的型別）→ `TypeDescriptor.name`。
   **純讀編譯器產生的靜態唯讀資料**（都在 `.rdata`），不碰被丟出的物件，安全一個數量級。
   還原修飾名時**要先跳過開頭那個句點、並用 `UNDNAME_NO_ARGUMENTS | UNDNAME_32_BIT_DECODE`**
   ——整串 `.?AVsystem_error@std@@` 直接餵給 `UnDecorateSymbolName` 一律回 0。
   那組旗標正是 MSVC 自己的 `type_info::name()` 用的，所以輸出跟偵錯器裡看到的一致。
2. **`what()`**：**只在 `CatchableTypeArray` 裡真的找得到 `std::exception`
   （修飾名 `.?AVexception@std@@`）時才做**，套用該項的 `thisDisplacement.mdisp`
   把物件指標調整到基底子物件再呼叫。找不到就只印型別名，不硬轉——
   `throw 42`、`throw` 自訂的非衍生類別都完全合法，硬轉會跳進別的 vtable。
   虛擬繼承（`pdisp >= 0`）一律放棄：`std::exception` 被虛擬繼承實務上等於不存在，
   為一條驗不到的指標算術冒險不划算。

兩個欄位**都恆存在**，拿不到就寫 `(unavailable)`——看不到欄位就分不出是
「沒有例外在飛」還是「`ThrowInfo` 讀爆了」（同表頭 `pdb` 那一行的理由）。
非 C++ 例外（`terminate`／`purecall`／`invalidparam`／存取違規／堆疊溢位）整段不輸出。

`mode=` 三種值：`main`、`mcp-stdio`、`splash`。

## 11. 清理策略：跟一般 log **不同**

`log_rotation.h` 是「留今天與前兩天」。crash 檔**不能照抄**：使用者可能一個月才崩一次，
按天數清會把唯一的現場清掉。改成**保留最近 20 份、不看日期**。

規則放 `core/crash_report.h` 配測試，理由跟 `log_rotation.h` 那句註解一字不差——
**少刪只是佔空間，多刪就是把使用者昨天的當機現場砍掉了。**

清理時機：啟動時掃一次（跟 §12 的提示掃描共用同一輪列舉）。不在 crash handler 裡清——
那條路徑上任何多餘的檔案系統操作都是風險。

## 12. 下次啟動的提示

handler 當下**絕不碰 UI**：stack overflow 那種堆疊已經沒空間的情況，當場叫 UI 很可能二次崩潰。
提示交給下一次正常啟動時的一般 Qt 程式碼路徑。

- config.json 新增 `crash.lastNotified`（檔名字串），**接在既有鍵最後面**，
  遵守 CLAUDE.md 講的 `serializeConfig` 鍵順序合約。
- 啟動時掃 `logs/crash-*.log` 取最新一份，檔名不等於 `lastNotified` 就提示，然後寫回。
- 提示形式：**系統匣氣球通知**（`QSystemTrayIcon::showMessage`），不是 modal——
  桌寵剛開機就跳對話框太兇。點通知開啟 logs 資料夾，走 `QDesktopServices::openUrl`
  這條專案已經用了四處的既有路徑（`model_page.cpp:69`、`persona_page.cpp:613` 等）。
- 另外在**關於分頁**加一顆常駐的「開啟日誌資料夾」按鈕。這顆本來就該有，不只為了當機。

## 13. 錯誤處理

**重入防護是必須的，不是保險。** 用一次 `InterlockedCompareExchange` 同時搶下所有權並記住
擁有者的 thread id（thread id 不會是 0，拿 0 當「沒人」的哨兵是安全的）：第一條進來的執行緒寫報告。
**但第二次進入有兩種情境，正確處置相反，本節初稿把它們寫成同一句是錯的**
（2026-08-31 Task 4 review 修正）：

- **同一條執行緒二次進來** ＝ 第二階段自己崩了（那本來就是「可能會死」的那半邊），
  再往下走就是無窮遞迴。**立刻 `TerminateProcess`，一格都不能等。**
- **另一條執行緒同時崩** ＝ 第一位此刻正夾在 `openCrashFile`／`WriteFile`／`FlushFileBuffers`
  之間（光 `CreateFileW` 就可能是毫秒級）。這時立刻 `TerminateProcess` 不是「不搶檔案」，
  而是**把正在寫報告的行程整個殺掉**，留下 0 位元組或半截的 crash 檔。
  而 heap 損毀、共用物件被釋放那種「多條執行緒幾乎同時 fault」的相關性當機，
  正是這個功能存在的理由。所以這條路要 **`Sleep(5000)` 讓路**，等第一位寫完再收尾；
  `Sleep` 是核心呼叫，不配置不上鎖，仍然守得住第一階段的禁用清單。

**開檔失敗要有退路但不能卡住**：主路徑失敗（磁碟滿、權限、目錄被刪）就試一次 `%TEMP%`，
再失敗就放棄進結束流程。當機路徑上三次以上的重試沒有意義。

**不要讓 DbgHelp 去連網。** 使用者機器上若設過 `_NT_SYMBOL_PATH`（裝過 WinDbg 或 VS 的人很常見），
`SymInitialize` 會照那個路徑去連 Microsoft 符號伺服器下載——**在當機當下連網抓幾十 MB**，
使用者看到的就是「程式卡住幾分鐘然後才消失」。一定要 `SymSetSearchPathW` 明確覆寫成
「只有 exe 所在目錄」，把環境變數擋掉。搭配
`SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME | SYMOPT_LOAD_LINES)`，
且 `SymInitialize` 在**啟動時**呼叫（deferred loads 讓註冊很便宜，真正讀 pdb 延到需要時）。

**結束方式：寫完檔直接 `TerminateProcess`**，不走 `ExitProcess`——後者會跑 DLL detach 與 `atexit`，
在已損毀的狀態下可能再死一次或卡住。代價是跳過 `LoggingGuard` 解構的 flush，
但 `DailyRotatingFileSink` 每則都 flush（`logging.cpp:134`），所以**實際上零損失**。

**第二階段有 10 秒硬上限，逾時由看門狗執行緒代為 `TerminateProcess`。**
上面那句「第二階段失敗只損失好看的那半邊」只涵蓋「失敗」，不涵蓋「卡住」——
允許配置與上鎖就等於允許死鎖：崩在 CRT heap lock 上（也就是**崩在 `malloc`／`free` 裡**，
正是 §5 兩階段設計最想活下來的那一家當機）時，`what()`（`std::system_error` 的訊息是
第一次呼叫才配置出來的）、`UnDecorateSymbolName`、DbgHelp 任何一支都可能當場卡死。
那時 `TerminateProcess` 永遠走不到，§13 開頭那條「另一條執行緒讓路用的 `Sleep(5000)`」
也幫不上忙（同一條執行緒，那條路根本不會走到），使用者得到的是一隻**凍住卻還賴在畫面上、
只能去工作管理員砍掉**的桌寵——**這是整個功能唯一會讓當機比「完全沒有 handler」更糟的路徑**。

看門狗的執行緒與事件**在 `installCrashHandler()` 就建好**，不是進第二階段前才 `CreateThread`。
這是整段唯一不直覺的地方，理由：崩潰當下 `CreateThread` 會為每個已載入的 DLL 跑一次
`DllMain(DLL_THREAD_ATTACH)`，**那要取 loader lock**——而「崩在 loader lock 上」正是
§8 一路在迴避的情境（第一階段連 `GetModuleHandleEx` 都不准碰就是這個理由）。
在那種狀態下 `CreateThread` 自己就會卡住，看門狗連計時都沒開始，等於白裝。
所以改成：安裝時（行程還健康）備好執行緒與 manual-reset event，執行緒開頭
`WaitForSingleObject(INFINITE)` 睡著；崩潰路徑上只剩一次 `SetEvent`——核心呼叫，
不配置、不上鎖，守得住第一階段的禁用清單。**上膛點在第一階段 `FlushFileBuffers` 落地之後、
進第二階段之前**，所以看門狗開火只會損失「好看」的那半邊。第二階段正常跑完會照原路
`TerminateProcess`，看門狗自然來不及作用。代價是行程生命期多一條閒置執行緒
（一次 `WaitForSingleObject`，零 CPU）。

`crash_handler.h` 的區塊註解要明列**第一階段的禁用清單**（照專案文件慣例寫理由）：
不呼叫 Qt、不 `new`/`malloc`、不上任何鎖、不用 CRT stdio、不碰 `std::string`、
連 `GetModuleHandleEx` 都不碰（loader lock）。

## 14. 架構與檔案切法

照專案既有規則：可測的邏輯進 `l2m_core`，OS 互動進 `src/platform/`。

**`src/platform/crash_handler.h` + `crash_handler_win.cpp` + `crash_handler_mac.mm`（空殼）**
安裝四個進場點、擷取堆疊、寫檔。以 `window_effects_win.cpp`（88 行）作對照，
這支會是 `platform/` 裡最大的一支——**當初估「250 行上下」，實際 1131 行**
（`crash_handler_win.cpp`，2026-08-31 這一輪修完之後）。差距不在功能長出來，
而在本專案的註解密度：`ThrowInfoRec` 那一組結構、`captureFromContext` 的葉函式退路、
第二階段看門狗、`initSymbols` 的搜尋路徑坑，每一段的「為什麼」都比程式碼本身長。
往後估行數時要把這件事算進去。
macOS 留空殼是因為 `CMakeLists.txt:525` 起的 install 版面本來就只在 `WIN32` 生效。

**`src/core/crash_report.h` / `.cpp`（配測試）**
crash 檔名規則、過期清理、`位址 + 模組基底 → "live2d_mate.exe+0xRVA"` 字串、錯誤碼文字、
手刻的十六進位格式化。跟 `log_rotation.h` 完全同一個理由：RVA 減錯一個基底，
整份報告就是靜靜地全部指到錯的函式。

**`src/app/main.cpp`：安裝點在 `LoggingGuard`（第 99 行）之後、`--mcp-stdio` 分流（第 103 行）之前**
這樣三種行程都受保護。但這裡有時序衝突：**crash 檔目錄那時還算不出來**——
`attachLogFile` 要等到第 210 行、`setApplicationName` 跑完 `QStandardPaths` 才有路徑，
而 handler 裡不能呼叫 Qt、不能配置記憶體，路徑必須事先轉成固定大小的寬字元緩衝存好。

解法照 `main.cpp` 註解第 4 點那個既有先例辦：**手刻讀一次 `%APPDATA%`**
（`GetEnvironmentVariableW`），自己組出 `%APPDATA%\live2d_mate\logs`。
這跟 `app.disableHardwareAcceleration` 為了搶在 `QApplication` 之前而手刻 yyjson 讀一次
是同一種**刻意的重複**，理由要寫進標頭註解。

**`src/mcp/mcp_http_server.cpp`** — 補 `set_exception_handler`（§7.1 ①）。

**`src/windows/tray.cpp` / `src/windows/settings/about_page.*`** — §12 的提示與按鈕。

**`CMakeLists.txt` / `.github/workflows/release.yml`** — §15。

## 15. 建置與 CI

```cmake
# 僅 WIN32
target_link_libraries(live2d_mate PRIVATE ... Dbghelp)

target_link_options(live2d_mate PRIVATE
  "$<$<CONFIG:RelWithDebInfo,Release>:/PDBSTRIPPED:$<TARGET_FILE_DIR:live2d_mate>/live2d_mate.stripped.pdb>")

# RENAME 是必要的，不是美觀問題 —— 見 §9.1
install(FILES "$<TARGET_FILE_DIR:live2d_mate>/live2d_mate.stripped.pdb"
        DESTINATION . RENAME live2d_mate.pdb OPTIONAL)
```

`OPTIONAL` 是因為 Debug 建置不產這個檔。
`/PDBSTRIPPED` 與 `/INCREMENTAL` 併用 MSVC 會抱怨——本機那份 168 MB 的 pdb 正好是
incremental link 的特徵，所以 RelWithDebInfo 大概要補 `/INCREMENTAL:NO`。實作時當場就會看到警告。

其餘：三個新檔進 `live2d_mate` 來源清單、`l2m_add_test(crash_report)`、
`release.yml` 把完整 pdb 壓成獨立資產掛上去。
`THIRD_PARTY_NOTICES.md` 不用動，DbgHelp 是系統元件。

## 16. 測試

**能進 `l2m_core` 的部分**（`tests/test_crash_report.cpp`）。
`tests.yml` 走 `L2M_BUILD_APP=OFF`，剛好只建這半邊，所以這批測試自動進 CI：

- `formatFrame(addr, imageBase, imageSize)` → `"live2d_mate.exe+0x001A2B3C"` 或絕對位址。
  **最值得測的一支**：邊界要釘住 `addr == base`（RVA 0，屬於界內）與 `addr == base + size`（界外）。
- **手刻的 `writeHex()`** — 因為第一階段不能用 `snprintf`，這支得自己寫，
  而它正是最容易差一位、少補零、大小寫不一致的東西。
- `expiredCrashFiles(names, 20)` — 保留最近 20 份的規則，
  含「解析不出來的檔名一律不碰」（照抄 `parseLogFileName` 的既有行為）。
- `crashFileName` / `parseCrashFileName` 的往返。
- `crashReasonText(code)`，含未知錯誤碼的退路。

**不能單元測的部分**，照 `L2M_SAY` / `L2M_DUMP_FRAME` 的既有慣例加一個環境變數旗標：

```
L2M_CRASH_TEST=av | uncaught | systemerror | terminate | purecall | invalidparam | stackoverflow
```

`systemerror` 跟 `uncaught` 走同一條派送路徑，差別只在丟的型別——它釘住的是
**`--- exception ---` 的 `what()` 那一行有沒有真的內容**。挑 `std::system_error`
是因為它最貼近本專案的歷史現場（`McpHttpServer::setStatus()` 持鎖 emit 造成的
mutex 重入，見 CLAUDE.md「持鎖不得 emit」），而且**光有型別名不知道是哪個 errno**
——正是「只拿型別名不夠」的活證據。

啟動 3 秒後故意觸發對應的死法。**這是唯一能驗證「守衛空間夠不夠」「重入防護有沒有效」
「精簡 pdb 到底解不解得出函式名」的手段**，成本只是一個 switch。
release 版也留著——現場排查時「使用者那台機器上 handler 到底有沒有在運作」是個會真的遇到的問題。
觸發前印一行 `qWarning` 註明是刻意的，免得日後有人拿它當成真當機。

驗收：七種死法各跑一次，人工確認 crash 檔的兩個階段都寫出來、符號化解得出自己的函式名，
且 `--- exception ---` 只在 `uncaught` 與 `systemerror` 兩種（真的有 C++ 例外在飛）時出現。

## 17. 已知限制

- **只抓崩潰那一條執行緒的堆疊。** 要抓另外兩條得 `SuspendThread` + `StackWalk64`，
  在已經不健康的行程裡風險大過收穫。
- **x64 限定。** §6 的展開迴圈在 32 位元 x86 上不成立。
- **第二階段可能靜靜失敗。** 那時 crash 檔只有第一階段的內容，仍可離線還原，但要動手。
- **精簡 pdb 沒有行號。** 要行號就得抓 release 的完整 pdb 離線解。
- **`_set_invalid_parameter_handler` 在 release 建置拿不到 expression/function/file/line**
  （CRT 不帶那些 debug 字串），只會是一堆 `nullptr`。堆疊仍然完整。
- **直接 `__fastfail` 的死法四個進場點一個都攔不到，寫不出任何報告。**
  `abort()`（UCRT 走 `__fastfail(FAST_FAIL_FATAL_APP_EXIT)`）與 `/GS` cookie 檢查失敗都是。
  理由見 §7：`__fastfail` 依設計繞過 vectored handler 與 unhandled exception filter，
  也不經過 terminate handler。要接得另外裝 `SIGABRT` handler，是設計變更。
- **堆積損毀也多半走 `__fastfail`，所以多半一份報告都沒有。** 這一條要單獨講，因為它跟
  §1 的動機案例（記憶體損毀那一家）**正面衝突**，而且 `crashReasonText` 裡確實有一筆
  `0xC0000374 HEAP_CORRUPTION`，讀起來像是在承諾涵蓋——**不是**。Windows 10/11 的多項
  堆積完整性檢查（釋放時的 header／encoded pointer 檢驗、LFH bucket 驗證…）觸發時走的是
  `RtlFailFast(FAST_FAIL_HEAP_METADATA_CORRUPTION)`，跟 `abort()` 同一條路，
  四個進場點一個都碰不到。真的丟出 `STATUS_HEAP_CORRUPTION`（`0xC0000374`）這個 SEH 例外的
  路徑存在但少見，那一筆是留給它的。**實務上的結論：堆積被寫壞時多半是「什麼都沒留下」，
  而不是「報告寫不完整」。** 那一家當機真正要靠的是頁堆積（`gflags /p`）或 ASan 這種在
  損毀當下就攔住的工具，不是事後的報告。§1 的三個現場裡，前兩個（存取違規、
  未捕捉例外）這個功能涵蓋得很好，第三個若真的走到堆積 fail-fast 就涵蓋不到。
- **報告裡凡是帶 detail 括號的 reason 碼，都是我們自己合成的。** 目前有兩個：
  `0xC0000409 STACK_BUFFER_OVERRUN`（`onTerminate` 的 `(std::terminate)` 與
  `onInvalidParameter` 的 `(invalid CRT parameter)`）與 `0xC0000025 NONCONTINUABLE_EXCEPTION`
  （`onPureCall` 的 `(purecall)`）。兩個都不是真的發生了那個 SEH 例外——那三個進場點手上
  根本沒有 `EXCEPTION_RECORD`，碼是寫死的。所以看到 `STACK_BUFFER_OVERRUN` 卻找不到緩衝溢位、
  看到 `NONCONTINUABLE_EXCEPTION` 卻沒有任何不可繼續的例外，都是正常的。
  **判斷規則就一條：有括號 ＝ 我們合成的，沒括號 ＝ 作業系統真的給的。**
- **第二階段最多跑 10 秒**，之後由看門狗執行緒收掉行程（§13）。逾時的報告只有第一階段的
  內容，跟「第二階段靜靜失敗」看起來一樣——差別在 `--- exception ---`／`--- symbolized ---`
  兩段都不會出現，而失敗通常只掉其中一段。

## 18. 建議的實作順序

1. `core/crash_report.h/.cpp` + `tests/test_crash_report.cpp`（純邏輯，TDD，先進 CI）
2. `platform/crash_handler.*` 第一階段 + `L2M_CRASH_TEST` 旗標 → 七種死法各驗一次
3. 第二階段符號化 + `CMakeLists.txt` 的 `Dbghelp` / `/PDBSTRIPPED` / install
4. `main.cpp` 安裝點與 `%APPDATA%` 手刻路徑
5. `mcp_http_server.cpp` 的 `set_exception_handler`
6. §12 的啟動提示、系統匣通知、關於分頁按鈕、`crash.lastNotified`
7. `release.yml` 掛完整 pdb
