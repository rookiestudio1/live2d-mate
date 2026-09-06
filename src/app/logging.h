#pragma once

// 應用程式日誌：把 Qt 的 qDebug／qWarning 導進 spdlog，同時輸出到 console 與檔案。
//
// **全專案只有 logging.cpp 這一個 TU 認識 spdlog。** 其餘 101 個既有呼叫點
// 與之後新加的埋點一律繼續寫 `qDebug() << "[tts] …"` —— qInstallMessageHandler
// 是行程層級的攔截，l2m_core 與 l2m_media 裡的 qDebug 同樣會流進來，
// 卻不必讓那兩個函式庫（以及只連 l2m_core 的 58 支測試）背上 spdlog 的相依。
// 附帶好處：測試行程沒裝 handler，行為與導入前完全一致。
//
// 訊息開頭的 `[live2d]`、`[tts]` 之類的子系統前綴會被切出來當成 spdlog 的
// named logger（core/log_category.h），Qt 自己的內部訊息則用它的 category
//（"qt.qpa.window" 之類）。所以平台外掛、GL、DirectWrite 字型 fallback 那些
// 以前只會閃過 stderr 的警告，現在也一起落進 log 檔了。
//
// **三個等級怎麼挑。** release 建置的預設過濾在 info（CMakeLists.txt 的
// L2M_DEFAULT_LOG_LEVEL），所以「隨手寫哪一個」直接決定了現場回報看不看得到：
//   qWarning —— 出事了或降級了，使用者可能得處理。
//   qInfo    —— 兩種。① 每次啟動最多印一次、而且是拿到 log 檔第一眼要找的骨幹
//                （載入了哪個模型、MCP 綁在哪、TTS 探測走了哪條路、做過什麼一次性
//                遷移）；② 被 L2M_PROFILE／L2M_SAY／L2M_DUMP_FRAME 這類環境變數旗標
//                守衛住的診斷輸出 —— 執行到就代表使用者主動要看，被預設等級默默
//                吃掉的症狀是「設了旗標卻什麼都沒印」，完全沒有線索指向日誌等級。
//   qDebug   —— 其餘全部：逐幀、逐句、逐次網路往返的細節。設一次
//                L2M_LOG_LEVEL=debug 就全部回來，不必為了「release 也看得到」而升級。
//
// **初始化刻意分兩階段**，因為三個約束互相打架：
//   ① spdlog 出廠的 default logger 寫的是 **stdout**，而 `--mcp-stdio` 模式的
//      stdout 就是 MCP 協定本身的傳輸通道 —— 印一個位元組進去 JSON-RPC 訊框就髒了，
//      Claude Desktop 那端直接解析失敗。所以第一階段必須排在 main() 的
//      `--mcp-stdio` 分流**之前**把 default logger 換掉，讓這件事由結構保證，
//      而不是靠「記得不要在橋接模式裡寫日誌」。
//   ② main.cpp 有三個 qWarning 發生在 QApplication 建立**之前**（建立
//      models／personas／memory 目錄那幾行），handler 要攔得到就得裝在更前面。
//      第一階段不碰 QStandardPaths，所以不受「setApplicationName 要先跑」的限制。
//   ③ 檔案 sink 卻**不能**在第一階段開：logs/ 的路徑要等 setApplicationName
//      之後才查得到，而且多行程會搶同一個檔（見 attachLogFile）。
// 第一階段到第二階段之間的訊息暫存在 ringbuffer 裡，開檔時整批回放，
// 時間戳保留原值 —— 開機早期的錯誤正是最需要留下來的那一批。

#include <filesystem>

namespace l2m {

// 第一階段。建構＝接上 console、換掉 default logger、裝上 Qt handler；
// 解構＝卸下 handler 並 flush。
//
// **宣告成 main() 的第一個 local**：本專案的物件圖全是 stack local、
// 解構順序即建構的反序，第一個宣告就是最後一個解構，日誌因此活得比
// 所有子系統的 teardown 都久。
class LoggingGuard {
public:
  LoggingGuard();
  ~LoggingGuard();

  LoggingGuard(const LoggingGuard&) = delete;
  LoggingGuard& operator=(const LoggingGuard&) = delete;
};

// 第二階段：補上檔案 sink（%APPDATA%/live2d_mate/logs/），並回放暫存的訊息。
//
// **只有搶到 single-instance 鎖的主行程可以呼叫。** 另外三種行程一律不開檔案：
//   `--mcp-stdio` 橋接每個 client 一個、可以同時好幾個長命行程；
//   `--splash` 子行程與主行程並存；
//   搶鎖失敗的第二實例在 return 之前已經跑過四行 qWarning。
// 多行程同時持有同一個 log 檔時，輪替的改名會失敗。
// 橋接模式的 stderr 本來就會被 Claude Desktop 收進它自己的 MCP server log。
void attachLogFile(const std::filesystem::path& logsDir);

}  // namespace l2m
