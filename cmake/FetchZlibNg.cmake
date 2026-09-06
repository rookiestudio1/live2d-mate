# zlib-ng（zlib License）—— 貼圖 PNG 解碼用的 inflate。
#
# 提供 target：`zlib-ng::zlibstatic`（ZLIB_COMPAT 模式，介面就是標準 zlib API，
# 所以 third_party/spng 的 `#include <zlib.h>` 原封不動就能用）。
# 用帶命名空間的那個名字而不是裸的 `zlibstatic` —— 後者是 zlib-ng 的
# `option(ZLIB_ALIASES)` 底下才建立的別名，使用者在 cache 裡關掉就會炸；
# `zlib-ng::zlibstatic` 是無條件建立的。BUILD_SHARED_LIBS 關掉時它們都指向
# 同一個實體 target `zlib-ng`（所以下面設定編譯選項時要對那個名字設，
# ALIAS 吃不了 target_compile_options）。
#
# ── 為什麼要多這一份 inflate（專案裡已經有 miniz 了）──
#
# PNG 解碼的時間**九成在 inflate**，反濾波與格式轉換加起來不到兩成。
# 實測 LiveroiD_A-Y01 的單張 8192×16384 貼圖（30 MB → 512 MiB，i5-13500）：
#
#   只做 inflate（miniz，也就是原版 zlib 的水準）  約 500 ms
#   只做 inflate（zlib-ng，SSE2）                  約 163 ms   ← 3.1x
#   Qt 內建 libpng + zlib，整張解完 + 轉格式       約 671 ms
#   libspng + miniz，整張解完 + 轉格式             約 675 ms   ← 沒有差別
#   libspng + zlib-ng，整張解完 + 轉格式           約 309 ms   ← 2.2x
#
# 中間那一行是重點：**換解碼器但不換 inflate 完全是白做的**。libspng 的 SSE2
# 反濾波在這個尺寸下省不到 10 ms，跟 Qt 的差距落在誤差裡。會誤導人的是
# 「libpng 沒開 SIMD」這件事本身為真（qtbase 的 libpng CMakeLists 明寫
# PNG_ARM_NEON_OPT=0，x86 那條要 PNG_INTEL_SSE 才會開而 Qt 沒定義），
# 但那塊根本不是瓶頸。
#
# 也試過 libdeflate：inflate 約 300 ms，比 miniz 快但輸給 zlib-ng，而且它
# **只有 one-shot API 沒有 streaming**，塞不進逐列解碼器 —— 要先把 IDAT 串成
# 一塊、再配一份完整的 537 MiB raw 緩衝，等於每個並行解碼的記憶體預算翻倍
# （見 model_controller.cpp 的 kDecodeBudgetBytes）。zlib-ng 是串流的，
# 記憶體用量跟從前一模一樣。
#
# miniz 留著不動：那邊用的是 zip 容器（mz_zip_reader_*），libdeflate 與
# zlib-ng 都沒有那組 API，不是二選一的關係。
#
# ── 指令集刻意壓到 SSE2 ──
#
# x86-64 保證有 SSE2，所以這個組合在任何 64 位元 PC 上都跑得動。開到 AVX2
# 實測是 145 ms / 277 ms（再快 11%），**刻意不要** —— 舊機器的相容性比那 11%
# 值錢，何況下面這個坑就是活生生的例子：
#
# 用 zlib-ng 的預設選項（AVX2 + AVX2VNNI + PCLMULQDQ + VPCLMULQDQ + AVX512）
# 建出來的版本，在開發機（i5-13500，Raptor Lake，E-core 讓整顆 CPU 沒有
# AVX-512）上一呼叫 inflate() 就 **0xC000001D（illegal instruction）當場死，
# 一句訊息都沒有**。逐項關掉之後確認：AVX2 / SSE4.2 / PCLMULQDQ 都正常，
# 問題出在 VPCLMULQDQ 或 AVX2VNNI 那兩個較新的指令集 —— 它的執行期偵測在
# MSVC 建置上判斷有誤。既然使用者的 CPU 五花八門而症狀是「開起來就閃退、
# 沒有任何線索」，這裡一律關到只剩 SSE2。
#
# WITH_RUNTIME_CPU_DETECTION 維持預設的 ON：那正是「舊 CPU 也安全」的機制
# （靠 cpuid 決定走哪條），關掉反而變成編譯期寫死。

include(FetchContent)

# 釘住的版本。升版時只改這一行，改完務必在真的機器上跑一次貼圖載入 ——
# 上面那個 illegal instruction 是連結得過、建置得過、執行才死的那種。
set(L2M_ZLIBNG_VERSION "2.3.3")

FetchContent_Declare(zlib-ng
  GIT_REPOSITORY https://github.com/zlib-ng/zlib-ng.git
  GIT_TAG ${L2M_ZLIBNG_VERSION}
  GIT_SHALLOW TRUE
)

# zlib 相容模式：spng 吃的是標準 zlib API（z_stream / inflateInit2 / inflate）
set(ZLIB_COMPAT ON CACHE BOOL "" FORCE)
# 它自己的測試與 benchmark 一個都不要（會多拉 gtest）
set(ZLIB_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(ZLIBNG_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(WITH_GTEST OFF CACHE BOOL "" FORCE)
set(WITH_BENCHMARKS OFF CACHE BOOL "" FORCE)
# 指令集：只留 SSE2（x86-64 的基準線）。理由見檔頭。
set(WITH_AVX512 OFF CACHE BOOL "" FORCE)
set(WITH_AVX512VNNI OFF CACHE BOOL "" FORCE)
set(WITH_AVX2 OFF CACHE BOOL "" FORCE)
set(WITH_AVX2VNNI OFF CACHE BOOL "" FORCE)
set(WITH_VPCLMULQDQ OFF CACHE BOOL "" FORCE)
set(WITH_PCLMULQDQ OFF CACHE BOOL "" FORCE)
set(WITH_SSE42 OFF CACHE BOOL "" FORCE)

# BUILD_SHARED_LIBS 是 zlib-ng 決定要不要另外產一份 DLL 的依據。本專案全靜態，
# 但這是全域變數，用完要還原 —— 同一個 scope 後面還有別人的 CMake 會讀它。
#
# **還原要分「本來就沒定義」與「定義成別的值」兩種**：直接 set 回存下來的空字串
# 會讓 `if(DEFINED BUILD_SHARED_LIBS)` 從 false 變成 true，而那正是 zlib-ng 自己
# 都在用的判斷（它的 CMakeLists 有三處 `if(NOT DEFINED BUILD_SHARED_LIBS)`）。
if(DEFINED BUILD_SHARED_LIBS)
  set(l2m_saved_build_shared "${BUILD_SHARED_LIBS}")
  set(l2m_had_build_shared TRUE)
else()
  set(l2m_had_build_shared FALSE)
endif()
set(BUILD_SHARED_LIBS OFF)
FetchContent_MakeAvailable(zlib-ng)
if(l2m_had_build_shared)
  set(BUILD_SHARED_LIBS "${l2m_saved_build_shared}")
else()
  unset(BUILD_SHARED_LIBS)
endif()

# 它的警告不是我們該修的，跟 Framework 一樣別讓它洗版
if(MSVC)
  target_compile_options(zlib-ng PRIVATE /W0)
else()
  target_compile_options(zlib-ng PRIVATE -w)
endif()
