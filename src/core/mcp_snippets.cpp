#include "mcp_snippets.h"

#include <yyjson.h>

#include "json_doc.h"
#include "mcp_host.h"

namespace l2m {

namespace {

// MCP on Windows 目前是 ODR 註冊制，說明列直接指向官方文件
constexpr const char* kWindowsMcpDoc = "https://learn.microsoft.com/en-us/windows/ai/mcp/overview";

// 有 token 才加 headers；沒有的話整個欄位不出現，避免使用者以為要填
void addAuthHeaders(yyjson_mut_doc* doc, yyjson_mut_val* target, const std::optional<std::string>& token) {
  if (!token.has_value() || token->empty()) return;
  yyjson_mut_val* headers = yyjson_mut_obj(doc);
  yyjson_mut_obj_add_strcpy(doc, headers, "Authorization", ("Bearer " + *token).c_str());
  yyjson_mut_obj_add_val(doc, target, "headers", headers);
}

std::string jsonText(jsonu::MutDoc& doc) { return doc.write(true); }

}  // namespace

std::vector<McpSnippet> buildMcpSnippets(const SnippetInput& input) {
  const std::string url = mcpUrl(input.host, input.port);
  const std::string origin = mcpOrigin(input.host, input.port);
  const bool hasToken = input.token.has_value() && !input.token->empty();

  std::vector<McpSnippet> snippets;

  // Claude Code：CLI 指令，不是設定檔
  {
    std::string command = std::string("claude mcp add ") + kMcpServerName + " --transport http " + url + " --scope user";
    if (hasToken) command += " --header \"Authorization: Bearer " + *input.token + "\"";
    snippets.push_back({"claude-code", McpSnippetGroup::Direct, "mcp.snippet.claudeCode", "", command, std::nullopt});
  }

  // VS Code：根鍵是 servers，跟其他家的 mcpServers 不一樣，而且要 "type": "http"
  {
    jsonu::MutDoc doc;
    yyjson_mut_doc* d = doc.get();
    yyjson_mut_val* root = yyjson_mut_obj(d);
    doc.setRoot(root);
    yyjson_mut_val* servers = yyjson_mut_obj(d);
    yyjson_mut_val* entry = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, entry, "type", "http");
    yyjson_mut_obj_add_strcpy(d, entry, "url", url.c_str());
    addAuthHeaders(d, entry, input.token);
    yyjson_mut_obj_add_val(d, servers, kMcpServerName, entry);
    yyjson_mut_obj_add_val(d, root, "servers", servers);
    snippets.push_back({"vscode", McpSnippetGroup::Direct, "mcp.snippet.vscode", "mcp.snippet.vscodeHint", jsonText(doc), std::nullopt});
  }

  // Cursor / Windsurf / Cline
  {
    jsonu::MutDoc doc;
    yyjson_mut_doc* d = doc.get();
    yyjson_mut_val* root = yyjson_mut_obj(d);
    doc.setRoot(root);
    yyjson_mut_val* servers = yyjson_mut_obj(d);
    yyjson_mut_val* entry = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, entry, "url", url.c_str());
    addAuthHeaders(d, entry, input.token);
    yyjson_mut_obj_add_val(d, servers, kMcpServerName, entry);
    yyjson_mut_obj_add_val(d, root, "mcpServers", servers);
    snippets.push_back({"cursor", McpSnippetGroup::Direct, "mcp.snippet.cursor", "mcp.snippet.cursorHint", jsonText(doc), std::nullopt});
  }

  // Gemini CLI：用 httpUrl；填成 url 會被當成 SSE
  {
    jsonu::MutDoc doc;
    yyjson_mut_doc* d = doc.get();
    yyjson_mut_val* root = yyjson_mut_obj(d);
    doc.setRoot(root);
    yyjson_mut_val* servers = yyjson_mut_obj(d);
    yyjson_mut_val* entry = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, entry, "httpUrl", url.c_str());
    addAuthHeaders(d, entry, input.token);
    yyjson_mut_obj_add_val(d, servers, kMcpServerName, entry);
    yyjson_mut_obj_add_val(d, root, "mcpServers", servers);
    snippets.push_back({"gemini-cli", McpSnippetGroup::Direct, "mcp.snippet.geminiCli", "mcp.snippet.geminiCliHint", jsonText(doc), std::nullopt});
  }

  // Claude Desktop：只吃 stdio，指向本執行檔的 --mcp-stdio 模式
  {
    jsonu::MutDoc doc;
    yyjson_mut_doc* d = doc.get();
    yyjson_mut_val* root = yyjson_mut_obj(d);
    doc.setRoot(root);
    yyjson_mut_val* servers = yyjson_mut_obj(d);
    yyjson_mut_val* entry = yyjson_mut_obj(d);
    yyjson_mut_obj_add_strcpy(d, entry, "command", input.exePath.c_str());
    yyjson_mut_val* args = yyjson_mut_arr(d);
    yyjson_mut_arr_add_str(d, args, "--mcp-stdio");
    yyjson_mut_arr_add_strcpy(d, args, url.c_str());
    if (hasToken) yyjson_mut_arr_add_strcpy(d, args, input.token->c_str());
    yyjson_mut_obj_add_val(d, entry, "args", args);
    yyjson_mut_obj_add_val(d, servers, kMcpServerName, entry);
    yyjson_mut_obj_add_val(d, root, "mcpServers", servers);
    snippets.push_back({"claude-desktop", McpSnippetGroup::Stdio, "mcp.snippet.claudeDesktop", "mcp.snippet.claudeDesktopHint", jsonText(doc), std::nullopt});
  }

  // 隧道指向來源，不是 /mcp；路徑由 client 自己接上去
  snippets.push_back({"cloudflared", McpSnippetGroup::Tunnel, "mcp.snippet.cloudflared", "mcp.snippet.tunnelHint", "cloudflared tunnel --url " + origin, std::nullopt});
  snippets.push_back({"ngrok", McpSnippetGroup::Tunnel, "mcp.snippet.ngrok", "", "ngrok http " + std::to_string(input.port), std::nullopt});

  snippets.push_back({"windows-copilot", McpSnippetGroup::Note, "mcp.snippet.windowsCopilot", "mcp.snippet.windowsCopilotHint", std::nullopt, kWindowsMcpDoc});

  return snippets;
}

}  // namespace l2m
