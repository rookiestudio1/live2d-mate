#pragma once

// MCP 監聽位址的安全規則與網址組裝。
// isAllowedOrigin 也放在這裡而不是伺服器那側 —— 它是純函式，
// 放這裡才測得到（不必先起一台 HTTP 伺服器）。
//
// 核心規則只有一條：綁到 loopback 以外的位址，等於讓同網段的任何人都能
// 操控桌面上的角色，所以一定要有 token。UI 會擋一次，這裡再擋一次，
// 因為 config.json 是使用者可以手改的。

#include <optional>
#include <string>
#include <vector>

namespace l2m {

// 監聽位址的兩個特殊值：只有本機，以及所有網路介面
inline constexpr const char* kLoopbackHost = "127.0.0.1";
inline constexpr const char* kAllInterfacesHost = "0.0.0.0";

// 設定檔裡固定用這個名字，換掉的話 AI 那邊的工具前綴也會跟著變
inline constexpr const char* kMcpServerName = "live2d_mate";

// 網路介面的最小結構，宣告成這樣才餵得進單元測試
struct NetworkAddress {
  std::string address;
  bool ipv4 = true;
  bool internal = false;
  // 介面名稱（下拉選單的標籤）
  std::string iface;
};

enum class McpHostKind { Loopback, All, Interface };

struct McpHostOption {
  std::string value;
  McpHostKind kind = McpHostKind::Interface;
  // kind == Interface 時是網卡名稱，其餘為空
  std::string label;
};

// 只有這幾個位址是「其他機器連不進來」
bool isLoopbackHost(const std::string& host);

// 綁到 loopback 以外就必須有 token
bool requiresToken(const std::string& host);

// 這組設定能不能安全地啟動；沒問題回 nullopt，否則回英文的原因字串
//（會顯示在狀態列，與伺服器的其他錯誤訊息同一風格）
std::optional<std::string> validateMcpBinding(const std::string& host, const std::optional<std::string>& token);

// 位址下拉選單：loopback、所有介面，再加上掃到的實體網卡 IPv4
std::vector<McpHostOption> listHostOptions(const std::vector<NetworkAddress>& interfaces);

// 連線指令不能寫 0.0.0.0 —— 那是「綁在哪」不是「連到哪」，貼過去會連不到。
// 綁所有介面時挑一個真的連得到的區網位址。
std::string resolveAdvertisedHost(const std::string& host, const std::vector<NetworkAddress>& interfaces);

// MCP 端點網址（含 /mcp 路徑）
std::string mcpUrl(const std::string& host, int port);
// 服務的來源（不含路徑）；隧道指令指向的是這個，不是 /mcp
std::string mcpOrigin(const std::string& host, int port);

// 產生一段夠長的隨機 token（24 bytes → base64url），讓使用者不必自己想
std::string generateToken();

// 哪些工具呼叫要吃「長期限」（185 秒而不是 20 秒），並佔用長呼叫的併發名額。
//
// **wait 刻意不影響答案**，即使直覺上 speak(wait=false) 應該很快回。
// 兩個理由：
//  1. 說話是佇列的（SpeechController），wait=false 的回覆掛在「這一句開始出聲」，
//     而那要等前面排隊的句子全部播完 —— 連下五句，第五句可能卡上一分鐘。
//  2. 還沒串流的引擎上，「開始出聲」本來就等於整段合成完；本機推論跑一分鐘是常態。
// 降成 20 秒會讓長句直接逾時，不佔名額則可能把 httplib 的 worker pool 佔滿。
// 參數留著是為了讓這條規則在測試裡講得出口，而不是被誤讀成「還沒想到」。
bool isLongToolCall(const std::string& tool, bool wait);

// 跨站請求的防線（DNS rebinding）。
// 沒有 Origin 標頭一律放行 —— 非瀏覽器的 MCP 用戶端本來就不送這個。
bool isAllowedOrigin(const std::optional<std::string>& origin, const std::string& boundHost);

// 實機的網路介面（QNetworkInterface）。測試改餵自己的清單。
std::vector<NetworkAddress> systemNetworkAddresses();

}  // namespace l2m
