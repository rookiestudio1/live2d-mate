// 各家 MCP client 的設定片段（core/mcp_snippets.h）。
//
// Claude Desktop 那組的 stdio 橋接做在同一支執行檔裡，
// 所以是 command: <exePath>, args: ["--mcp-stdio", url] ——
// 使用者不必為了 Claude Desktop 另外裝 Node。
#include <QtTest>

#include <set>
#include <string>
#include <vector>

#include "core/json_doc.h"
#include "core/mcp_snippets.h"

using namespace l2m;

namespace {

constexpr const char* kExePath = "C:/app/live2d_mate.exe";

std::vector<McpSnippet> build(const std::string& host = "127.0.0.1", const std::optional<std::string>& token = std::nullopt) {
  SnippetInput input;
  input.host = host;
  input.port = 3777;
  input.token = token;
  input.exePath = kExePath;
  return buildMcpSnippets(input);
}

const McpSnippet& find(const std::vector<McpSnippet>& snippets, const std::string& id) {
  for (const auto& snippet : snippets) {
    if (snippet.id == id) return snippet;
  }
  static const McpSnippet missing;
  return missing;
}

// 片段是 JSON 的都要能 parse 回來，不然使用者貼過去只會拿到語法錯誤
std::optional<jsonu::Doc> json(const std::vector<McpSnippet>& snippets, const std::string& id) {
  const McpSnippet& snippet = find(snippets, id);
  if (!snippet.text.has_value()) return std::nullopt;
  return jsonu::Doc::parse(*snippet.text);
}

// mcpServers/servers → live2d_mate 的那層物件
yyjson_val* serverEntry(const jsonu::Doc& doc, const char* rootKey) {
  yyjson_val* servers = jsonu::get(doc.root(), rootKey);
  return jsonu::get(servers, "live2d_mate");
}

}  // namespace

class TestMcpSnippets : public QObject {
  Q_OBJECT

private slots:
  // id 不重複，且每個非說明片段都有內容
  void idsAreUniqueAndTextPresent() {
    const auto snippets = build();
    std::set<std::string> ids;
    for (const auto& snippet : snippets) {
      QVERIFY2(ids.insert(snippet.id).second, snippet.id.c_str());
      if (snippet.group == McpSnippetGroup::Note) {
        QVERIFY(!snippet.text.has_value());
      } else {
        QVERIFY(snippet.text.has_value() && !snippet.text->empty());
      }
    }
  }

  // 分組涵蓋直連、stdio、隧道與說明
  void groupsCoverAllKinds() {
    std::set<int> groups;
    for (const auto& snippet : build()) groups.insert(static_cast<int>(snippet.group));
    QCOMPARE(groups.size(), size_t(4));
  }

  // 所有 JSON 片段都是合法 JSON
  void jsonSnippetsParse() {
    const auto snippets = build("127.0.0.1", std::string("abc"));
    for (const char* id : {"vscode", "cursor", "gemini-cli", "claude-desktop"}) {
      QVERIFY2(json(snippets, id).has_value(), id);
    }
  }

  // === Claude Code ===

  // 沒有 token 時就是一行 CLI 指令
  void claudeCodeWithoutToken() {
    QCOMPARE(*find(build(), "claude-code").text, std::string("claude mcp add live2d_mate --transport http "
                                                             "http://127.0.0.1:3777/mcp --scope user"));
  }

  // 有 token 時補上 Authorization 標頭
  void claudeCodeWithToken() {
    QCOMPARE(*find(build("127.0.0.1", std::string("s3cret")), "claude-code").text, std::string("claude mcp add live2d_mate --transport http "
                                                                                               "http://127.0.0.1:3777/mcp --scope user "
                                                                                               "--header \"Authorization: Bearer s3cret\""));
  }

  // === 可直接連本機的 JSON 片段 ===

  // VS Code / Copilot 的根鍵是 servers，且要 type: http
  void vscodeUsesServersKey() {
    auto doc = json(build(), "vscode");
    QVERIFY(doc.has_value());
    QCOMPARE(yyjson_obj_size(doc->root()), size_t(1));
    yyjson_val* entry = serverEntry(*doc, "servers");
    QVERIFY(entry != nullptr);
    QCOMPARE(jsonu::getString(entry, "type"), std::string("http"));
    QCOMPARE(jsonu::getString(entry, "url"), std::string("http://127.0.0.1:3777/mcp"));
  }

  // Cursor / Windsurf / Cline 的根鍵是 mcpServers，用 url
  void cursorUsesMcpServersKey() {
    auto doc = json(build(), "cursor");
    QVERIFY(doc.has_value());
    QCOMPARE(yyjson_obj_size(doc->root()), size_t(1));
    yyjson_val* entry = serverEntry(*doc, "mcpServers");
    QVERIFY(entry != nullptr);
    QCOMPARE(jsonu::getString(entry, "url"), std::string("http://127.0.0.1:3777/mcp"));
  }

  // Gemini CLI 用的是 httpUrl 而不是 url —— 填錯它會當成 SSE
  void geminiUsesHttpUrl() {
    auto doc = json(build(), "gemini-cli");
    QVERIFY(doc.has_value());
    yyjson_val* entry = serverEntry(*doc, "mcpServers");
    QCOMPARE(jsonu::getString(entry, "httpUrl"), std::string("http://127.0.0.1:3777/mcp"));
    QVERIFY(jsonu::get(entry, "url") == nullptr);
  }

  // 有 token 時三家都帶 Authorization，沒 token 時完全不出現 headers
  void headersOnlyWithToken() {
    const auto withToken = build("127.0.0.1", std::string("s3cret"));
    for (const char* id : {"vscode", "cursor", "gemini-cli"}) {
      auto doc = json(withToken, id);
      QVERIFY(doc.has_value());
      const char* rootKey = std::string(id) == "vscode" ? "servers" : "mcpServers";
      yyjson_val* headers = jsonu::get(serverEntry(*doc, rootKey), "headers");
      QVERIFY2(headers != nullptr, id);
      QCOMPARE(jsonu::getString(headers, "Authorization"), std::string("Bearer s3cret"));
    }

    const auto without = build();
    for (const char* id : {"vscode", "cursor", "gemini-cli"}) {
      QVERIFY2(find(without, id).text->find("headers") == std::string::npos, id);
    }
  }

  // === Claude Desktop（只吃 stdio）===

  // 橋接做在同一支執行檔裡，所以 command 指向 exe，args 以 --mcp-stdio 開頭
  void claudeDesktopUsesOwnExecutable() {
    auto doc = json(build(), "claude-desktop");
    QVERIFY(doc.has_value());
    yyjson_val* entry = serverEntry(*doc, "mcpServers");
    QCOMPARE(jsonu::getString(entry, "command"), std::string(kExePath));

    yyjson_val* args = jsonu::get(entry, "args");
    QVERIFY(args && yyjson_is_arr(args));
    QCOMPARE(yyjson_arr_size(args), size_t(2));
    QCOMPARE(jsonu::asString(yyjson_arr_get(args, 0)), std::string("--mcp-stdio"));
    QCOMPARE(jsonu::asString(yyjson_arr_get(args, 1)), std::string("http://127.0.0.1:3777/mcp"));
  }

  // 有 token 時當成第三個參數接在後面
  void claudeDesktopAppendsToken() {
    auto doc = json(build("127.0.0.1", std::string("s3cret")), "claude-desktop");
    QVERIFY(doc.has_value());
    yyjson_val* args = jsonu::get(serverEntry(*doc, "mcpServers"), "args");
    QCOMPARE(yyjson_arr_size(args), size_t(3));
    QCOMPARE(jsonu::asString(yyjson_arr_get(args, 2)), std::string("s3cret"));
  }

  // === 隧道 ===

  // 隧道指向來源而不是 /mcp 路徑
  void tunnelsPointAtOrigin() {
    const auto snippets = build("192.168.1.5");
    QCOMPARE(*find(snippets, "cloudflared").text, std::string("cloudflared tunnel --url http://192.168.1.5:3777"));
    QCOMPARE(*find(snippets, "ngrok").text, std::string("ngrok http 3777"));
  }

  // === Windows 11 內建 Copilot ===

  // 是一則說明而不是可貼上的片段，並附官方文件連結
  void windowsCopilotIsANote() {
    // build() 回傳的是 vector 值，先留在區域變數裡 ——
    // 直接對暫時物件取 find() 的參考會懸空
    const auto snippets = build();
    const McpSnippet& note = find(snippets, "windows-copilot");
    QVERIFY(note.group == McpSnippetGroup::Note);
    QVERIFY(!note.text.has_value());
    QVERIFY(note.docUrl.has_value());
    QVERIFY(note.docUrl->rfind("https://learn.microsoft.com/", 0) == 0);
  }

  // === 對外位址 ===

  // 換成區網位址時所有片段都跟著換，不會殘留 127.0.0.1
  void lanHostLeavesNoLoopback() {
    for (const auto& snippet : build("192.168.1.5", std::string("t"))) {
      if (!snippet.text.has_value()) continue;
      QVERIFY2(snippet.text->find("127.0.0.1") == std::string::npos, snippet.id.c_str());
    }
  }
};

QTEST_APPLESS_MAIN(TestMcpSnippets)
#include "test_mcp_snippets.moc"
