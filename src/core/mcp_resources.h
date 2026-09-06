#pragma once

// MCP Resources：把角色描述開放給 client 主動讀取。
//
// 為什麼要有這一條通道（三條通道的分工，改之前務必先看完）：
//
//   通道                                  AI 一定讀得到   換角色後即時生效
//   ─────────────────────────────────────────────────────────────────────
//   initialize 的 instructions（放全文）        是＊          否，要重連
//   speak / perform 的 description（放指向）    是           否，要重連
//   resources/list + read（放全文與清單）       否           **是**
//
// ＊ 這個「是」有長度條件：host 會截斷 instructions（Claude Code 實測砍在第 2048
//    個字元），所以「AI 一定讀得到」只到那個長度為止。角色描述因此排在互動協定
//    **之前**（core/mcp_tool_specs.cpp 的 mcpInstructions），被切掉的才會是協定尾巴
//    那些次要規則。這也是本模組的第二個存在理由：resources 沒有長度上限，
//    描述寫滿 2000 字元時，這裡是全文唯一到得了的路。
//
// 前兩條會進 host 的 system prompt，所以「AI 一定讀得到」；但它們只在連線
// 交握時送一次，而本專案的 MCP 傳輸是 POST-only 的 httplib（沒有 SSE），
// 伺服器端**推不了** notifications/*，所以換角色之後已經連線的 client
// 不會知道。resources 是 client 端發起的，永遠拿得到最新的那一份 ——
// 這是「換角色即時生效」唯一走得通的路。
//
// 這個模組只碰 PersonaSnapshot，不碰檔案系統，所以測試不必準備目錄。
// 伺服器端的方法分派在 src/mcp/mcp_http_server.cpp。

#include <optional>
#include <string>

#include "persona.h"

namespace l2m {

// 目前套用中的角色。永遠列得出來（沒有套用時讀到的是一句說明），
// 這樣 AI 不必先判斷有沒有才決定要不要讀。
inline constexpr const char* kPersonaActiveUri = "persona://active";

// 個別角色檔的 URI 前綴。刻意不是 "persona://<名稱>" —— 使用者真的可以把
// 角色取名叫 "active"，那樣就會和上面那一個撞在一起。
inline constexpr const char* kPersonaSavedPrefix = "persona://saved/";

// resources/list 的 result JSON
std::string mcpResourcesListJson(const PersonaSnapshot& persona);

// resources/read 的 result JSON；未知的 uri 回 nullopt
//（呼叫端要轉成 JSON-RPC 的 -32002 kJsonRpcResourceNotFound）
std::optional<std::string> mcpResourceReadJson(const PersonaSnapshot& persona, const std::string& uri);

}  // namespace l2m
