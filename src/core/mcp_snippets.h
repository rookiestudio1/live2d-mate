#pragma once

// 各家 AI 應用的 MCP 連線設定片段。
//
// 集中在一處產生，系統匣與設定視窗才不會各自拼出不一樣的指令。
//
// 只吃 stdio 的 Claude Desktop 指向同一支執行檔內建的橋接
//（command: <exePath>, args: ["--mcp-stdio", url]），使用者不必另外裝任何執行環境。

#include <optional>
#include <string>
#include <vector>

namespace l2m {

enum class McpSnippetGroup { Direct, Stdio, Tunnel, Note };

struct McpSnippet {
  std::string id;
  McpSnippetGroup group = McpSnippetGroup::Direct;
  // i18n key（標籤與說明），由 UI 自己翻譯
  std::string labelKey;
  std::string hintKey;
  // 可複製的內容；note 類型沒有內容，只有說明與文件連結
  std::optional<std::string> text;
  std::optional<std::string> docUrl;
};

struct SnippetInput {
  // 已解析過的對外主機，不會是 0.0.0.0
  std::string host;
  int port = 0;
  std::optional<std::string> token;
  // 本執行檔的絕對路徑（Claude Desktop 的 stdio 橋接指向它自己）
  std::string exePath;
};

std::vector<McpSnippet> buildMcpSnippets(const SnippetInput& input);

}  // namespace l2m
