#pragma once

// stdio ↔ Streamable HTTP 的 MCP 橋接模式。
//
// 給只支援 stdio 的 MCP 用戶端（Claude Desktop）使用：
//   live2d_mate.exe --mcp-stdio [url] [token]
// 預設 url 是 http://127.0.0.1:3777/mcp，也吃環境變數 L2D_MCP_URL / L2D_MCP_TOKEN。
//
// 橋接做進同一支執行檔，而不是另外附一支腳本：
// 使用者不必為了 Claude Desktop 再裝別的執行環境。
//
// **這條路沒有替代方案，所以它的品質就是 Claude Desktop 的體驗上限**：
// claude_desktop_config.json 只驗證 stdio，寫 url／type 不但不會生效，
// 較新版本還會靜默丟掉整個 mcpServers；而 Custom Connectors 要求伺服器
// 從 Anthropic 的公網 IP 連得到，localhost 是明確不支援。
// （Claude Code 反而可以直接 {"type":"http","url":"http://127.0.0.1:3777/mcp"}，
// 不經過這裡 —— 對照兩邊的行為差異時要記得這件事。）
//
// 這個模式**不建立 QApplication、不開視窗、不搶單一實例鎖** ——
// 它只是一個管線轉發器，會有好幾個實例同時存在（每個 client 一個）。
// 所以 main() 必須在做任何 Qt 初始化之前就先分流到這裡。
//
// 直接轉發 JSON-RPC 訊息而不解讀內容，橋接才不會因為協定細節改變而壞掉。
//
// **併發是必要的，不是最佳化**：主執行緒只負責把 stdin 讀進佇列，
// 另外幾條 worker 各自持有 httplib::Client 去送 HTTP。
// 舊版是單執行緒逐行處理的，一個 speak／perform（伺服器端期限 185 秒）
// 會把後面**所有**請求鎖在 stdin 裡沒人去讀，client 那邊就成批 timeout。
// JSON-RPC 靠 id 配對，回應亂序寫回 client 是合法的。
// 實測數字、worker 數的取法、以及收尾的寬限期都寫在 .cpp 的常數註解裡。

namespace l2m {

int runMcpStdioBridge(int argc, char* argv[]);

}  // namespace l2m
