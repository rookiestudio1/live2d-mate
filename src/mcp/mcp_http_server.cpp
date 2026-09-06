// httplib.h 必須排在所有 Qt 標頭之前：它會拉進 winsock2.h，
// 而 Qt 的標頭會先拉 windows.h，順序反了就會撞上 winsock 1.x 的定義。
#include <httplib.h>

#include "mcp_http_server.h"

#include <QDebug>
#include <QMetaObject>

#include <algorithm>
#include <chrono>
#include <exception>

#include "core/json_doc.h"
#include "core/jsonrpc.h"
#include "core/mcp_host.h"
#include "core/mcp_resources.h"
#include "core/mcp_tool_specs.h"
#include "mcp_tools.h"
#include "platform/crash_handler.h"

namespace l2m {

namespace {

// 請求主體上限 1 MiB。合成的動作關鍵影格是最大的輸入，離這個上限還很遠。
constexpr size_t kMaxBodyBytes = 1024 * 1024;

// 一般工具的期限 20 秒；speak wait=true 與 perform 另計 180 秒，
// 兩者都再加 5 秒給跨執行緒投遞本身
constexpr int kDefaultDeadlineMs = 20000;
constexpr int kLongDeadlineMs = 185000;
// 同時最多幾個長呼叫，超過就直接請 AI 稍後再試，不要把 worker pool 佔滿
constexpr int kMaxConcurrentLongCalls = 4;

// MCP 協定版本：帶回用戶端要求的版本（若我們支援），否則回我們的預設
constexpr const char* kProtocolVersion = "2025-06-18";
const std::vector<std::string>& supportedProtocolVersions() {
  static const std::vector<std::string> versions{"2025-06-18", "2025-03-26", "2024-11-05"};
  return versions;
}

// 從工具參數裡撈 wait（只有 speak 有），決定要不要走長期限。
bool wantsWait(const std::string& argsJson) {
  auto doc = jsonu::Doc::parse(argsJson);
  if (!doc || !doc->root()) return false;
  yyjson_val* value = yyjson_obj_get(doc->root(), "wait");
  return value && yyjson_is_bool(value) && yyjson_get_bool(value);
}

std::string jsonEscape(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() + 8);
  for (const unsigned char c : raw) {
    if (c == '"' || c == '\\') {
      out += '\\';
      out += static_cast<char>(c);
    } else if (c == '\n') {
      out += "\\n";
    } else if (c < 0x20) {
      static const char* kHex = "0123456789abcdef";
      out += "\\u00";
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    } else {
      out += static_cast<char>(c);
    }
  }
  return out;
}

}  // namespace

McpHttpServer::McpHttpServer(McpTools& tools, QString version, QObject* parent) : QObject(parent), tools_(tools), version_(std::move(version)) {}

McpHttpServer::~McpHttpServer() { stop(); }

McpHttpServer::Status McpHttpServer::status() const {
  std::lock_guard<std::mutex> lock(statusMutex_);
  return status_;
}

void McpHttpServer::setStatus(Status next) {
  {
    std::lock_guard<std::mutex> lock(statusMutex_);
    status_ = std::move(next);
  }
  // 鎖已釋放才發訊號 —— 接收端會回頭呼叫 status()
  emit statusChanged();
}

void McpHttpServer::setPersona(PersonaSnapshot snapshot) {
  std::lock_guard<std::mutex> lock(personaMutex_);
  persona_ = std::move(snapshot);
}

PersonaSnapshot McpHttpServer::personaSnapshot() const {
  std::lock_guard<std::mutex> lock(personaMutex_);
  return persona_;
}

void McpHttpServer::setSpeechProtocol(SpeechProtocol protocol) {
  std::lock_guard<std::mutex> lock(speechMutex_);
  speech_ = protocol;
}

SpeechProtocol McpHttpServer::speechProtocol() const {
  std::lock_guard<std::mutex> lock(speechMutex_);
  return speech_;
}

void McpHttpServer::start(const std::string& host, int port, const std::optional<std::string>& token) {
  stop();

  // config.json 是使用者可以手改的，UI 擋過一次這裡還是要再擋一次
  if (const auto refusal = validateMcpBinding(host, token)) {
    setStatus({false, "", *refusal});
    return;
  }

  boundHost_ = host;
  token_ = token;
  server_ = std::make_unique<httplib::Server>();
  server_->set_payload_max_length(kMaxBodyBytes);

  server_->Get("/health", [this](const httplib::Request&, httplib::Response& res) {
    res.set_content(std::string("{\"ok\":true,\"name\":\"") + kMcpServerName + "\",\"version\":\"" + version_.toStdString() + "\",\"path\":\"/mcp\"}", "application/json");
  });

  server_->Post("/mcp", [this](const httplib::Request& req, httplib::Response& res) {
    // **只在當前執行緒的第一個請求時裝一次。**
    // terminate handler 與堆疊守衛空間都是 per-thread 的，httplib worker 各執行緒
    // 需要各自安裝，該執行緒的例外與堆疊溢位才走得到 crash 報告
    //（見 platform/crash_handler.h）。
    static thread_local bool installedCrashSupport = false;
    if (!installedCrashSupport) {
      installedCrashSupport = true;
      l2m::platform::installThreadCrashSupport();
    }

    // DNS rebinding 防線：沒有 Origin 一律放行（非瀏覽器用戶端本來就不送）
    std::optional<std::string> origin;
    if (req.has_header("Origin")) origin = req.get_header_value("Origin");
    if (!isAllowedOrigin(origin, boundHost_)) {
      res.status = 403;
      res.set_content("{\"error\":\"Origin not allowed\"}", "application/json");
      return;
    }

    if (token_.has_value() && !token_->empty()) {
      const std::string expected = "Bearer " + *token_;
      if (!req.has_header("Authorization") || req.get_header_value("Authorization") != expected) {
        res.status = 401;
        res.set_content("{\"error\":\"Invalid or missing access token\"}", "application/json");
        return;
      }
    }

    const std::string reply = handleRpc(req.body);
    // 通知（沒有 id）不需要回應，依規格回 202
    if (reply.empty()) {
      res.status = 202;
      return;
    }
    res.set_header("MCP-Protocol-Version", kProtocolVersion);
    res.set_content(reply, "application/json");
  });

  server_->set_error_handler([](const httplib::Request& req, httplib::Response& res) {
    if (res.status != 404) return;
    res.set_content("{\"error\":\"Unknown path " + jsonEscape(req.path) + "; the MCP endpoint is /mcp\"}", "application/json");
  });

  // **`set_exception_handler` 只攔我們的 route handler 執行期間的例外。**
  // 「POST /mcp handler 開頭」已用 `thread_local` 旗標在 worker 執行緒裡裝一次
  // `installThreadCrashSupport()`，讓該執行緒「任何位置」（含 httplib 自己的
  // 解析與檔案輸出）逸出的例外都能走到 crash 報告，堆疊溢位也一併蓋到。
  // **殘留缺口**：一條全新的 worker 執行緒在「服務第一個請求的解析階段」丟例外仍然攔不到
  // ——那時我們的 handler 還沒裝上去。實務上 httplib 的 pool 執行緒都會服務請求，
  // 暖機之後就都覆蓋到了。
  // 這一段攔的例外會回 500 給 client，所以 client 不會看到斷線
  //（見 platform/crash_handler.h）。
  server_->set_exception_handler([](const httplib::Request& req, httplib::Response& res, std::exception_ptr ep) {
    std::string what = "unknown exception";
    try {
      std::rethrow_exception(ep);
    } catch (const std::exception& e) {
      what = e.what();
    } catch (...) {
    }
    qWarning() << "[mcp] worker 執行緒逸出例外:" << QString::fromStdString(what) << "path:" << QString::fromStdString(req.path);
    res.status = 500;
    res.set_content("{\"error\":\"Internal error: " + jsonEscape(what) + "\"}", "application/json");
  });

  // 先在 GUI 執行緒 bind，才拿得到同步的失敗原因（埠被佔用、位址不存在）
  const int bound = server_->bind_to_port(host, port);
  if (bound == 0) {
    server_.reset();
    setStatus({false, "", "Could not listen on " + host + ":" + std::to_string(port) + " - the port may already be in use, or the address does not exist on this machine"});
    return;
  }

  thread_ = std::thread([this] {
    if (server_) server_->listen_after_bind();
  });

  qInfo() << "[mcp] 監聽中:" << QString::fromStdString(mcpUrl(host, port));
  setStatus({true, mcpUrl(host, port), ""});
}

void McpHttpServer::stop() {
  if (!server_) {
    bool wasRunning = false;
    {
      std::lock_guard<std::mutex> lock(statusMutex_);
      wasRunning = status_.running;
    }
    if (wasRunning) setStatus({false, "", ""});
    return;
  }

  server_->stop();
  if (thread_.joinable()) thread_.join();
  server_.reset();

  // 還在等的呼叫要立刻放生：GUI 執行緒即將離開事件迴圈，
  // 不放的話 worker 會對著永遠不會來的答案等滿 185 秒
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    for (auto& call : pending_) {
      call->complete(
        "{\"content\":[{\"type\":\"text\",\"text\":\"The character app is shutting "
        "down\"}],\"isError\":true}");
    }
    pending_.clear();
  }

  setStatus({false, "", ""});
}

std::string McpHttpServer::dispatchTool(const std::string& tool, const std::string& argsJson) {
  const bool longCall = isLongToolCall(tool, wantsWait(argsJson));
  if (longCall) {
    if (longCalls_.fetch_add(1) >= kMaxConcurrentLongCalls) {
      longCalls_.fetch_sub(1);
      // **硬拒絕，不是逾時**：這一句根本沒有進佇列，AI 拿到的是一則錯誤。
      // wait=false 的 speak 要等「輪到它出聲」才放掉額度，所以連送第五句就會撞上
      qWarning() << "[mcp] 長呼叫額度已滿（" << kMaxConcurrentLongCalls << "），拒絕" << QString::fromStdString(tool);
      return "{\"content\":[{\"type\":\"text\",\"text\":\"Too many long-running calls in "
             "flight\\nRetry in a moment, or call speak with wait=false.\"}],\"isError\":true}";
    }
  }

  auto call = std::make_shared<PendingCall>(tool, argsJson);
  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.push_back(call);
  }

  // functor 版的 invokeMethod：不必註冊 metatype，捕獲的 shared_ptr
  // 保證物件活到 GUI 執行緒真的執行完
  McpTools* tools = &tools_;
  QMetaObject::invokeMethod(tools, [tools, call] { tools->invoke(call); }, Qt::QueuedConnection);

  const int deadline = longCall ? kLongDeadlineMs : kDefaultDeadlineMs;
  const bool answered = call->wait(std::chrono::milliseconds(deadline));

  {
    std::lock_guard<std::mutex> lock(pendingMutex_);
    pending_.erase(std::remove(pending_.begin(), pending_.end(), call), pending_.end());
  }
  if (longCall) longCalls_.fetch_sub(1);

  if (!answered) {
    // 逾時只是「不再等回覆」，語音其實還在播（PendingCall 逾時不取消播放）。
    // AI 這時常會改送 stop_speaking，那才真的把剩下的句子砍掉
    qWarning() << "[mcp]" << QString::fromStdString(tool) << "逾時" << deadline / 1000 << "秒，回報 timeout（語音仍在播放）";
    return "{\"content\":[{\"type\":\"text\",\"text\":\"" + jsonEscape(tool) + " timed out after " + std::to_string(deadline / 1000) +
           "s\\nThe character app may be busy; try again.\"}],\"isError\":true}";
  }
  return call->output();
}

std::string McpHttpServer::handleRpc(const std::string& body) {
  auto doc = jsonu::Doc::parse(body);
  if (!doc || !doc->root()) {
    return errorResponse("", kJsonRpcParseError, "Parse error");
  }

  auto messages = parseRequest(doc->root());
  if (!messages) {
    return errorResponse("", kJsonRpcInvalidRequest, "Invalid Request");
  }

  std::vector<std::string> responses;
  for (const RpcMessage& message : *messages) {
    // 通知不需要回應
    if (message.isNotification()) continue;

    if (message.method == "initialize") {
      std::string version = kProtocolVersion;
      if (message.params) {
        const std::string requested = jsonu::getString(message.params, "protocolVersion");
        const auto& supported = supportedProtocolVersions();
        if (std::find(supported.begin(), supported.end(), requested) != supported.end()) {
          version = requested;
        }
      }

      // instructions 現在帶著角色描述全文 —— 使用者貼進去的散文一定有換行，
      // 很可能有 tab 與 CRLF，而本檔的 jsonEscape() 只逸出引號、反斜線與換行，
      // 歸位字元與 tab 都沒處理。整段改用 yyjson 組，逸出交給它 ——
      // 這也是專案的慣例（CLAUDE.md：JSON 一律 yyjson）。
      const PersonaSnapshot persona = personaSnapshot();
      jsonu::MutDoc doc;
      yyjson_mut_doc* d = doc.get();
      yyjson_mut_val* root = yyjson_mut_obj(d);
      doc.setRoot(root);
      yyjson_mut_obj_add_strcpy(d, root, "protocolVersion", version.c_str());

      yyjson_mut_val* capabilities = yyjson_mut_obj(d);
      yyjson_mut_obj_add_val(d, capabilities, "tools", yyjson_mut_obj(d));
      // resources 是角色描述唯一「換角色後不必重連就拿得到新版」的通道
      yyjson_mut_obj_add_val(d, capabilities, "resources", yyjson_mut_obj(d));
      yyjson_mut_obj_add_val(d, root, "capabilities", capabilities);

      yyjson_mut_val* serverInfo = yyjson_mut_obj(d);
      yyjson_mut_obj_add_str(d, serverInfo, "name", kMcpServerName);
      yyjson_mut_obj_add_strcpy(d, serverInfo, "version", version_.toStdString().c_str());
      yyjson_mut_obj_add_val(d, root, "serverInfo", serverInfo);

      const std::string instructions = mcpInstructions(persona.activeName, persona.activeText(), speechProtocol());
      yyjson_mut_obj_add_strcpy(d, root, "instructions", instructions.c_str());

      responses.push_back(resultResponse(message.idJson, doc.write(false)));
      continue;
    }

    if (message.method == "ping") {
      responses.push_back(resultResponse(message.idJson, "{}"));
      continue;
    }

    if (message.method == "tools/list") {
      // 工具表是靜態的，唯一動的是 speak / perform 說明後面接的角色名 ——
      // 那是 GUI 執行緒推過來的快照複本，一樣不必上 GUI 執行緒
      responses.push_back(resultResponse(message.idJson, mcpToolsListJson(personaSnapshot().activeName)));
      continue;
    }

    if (message.method == "resources/list") {
      responses.push_back(resultResponse(message.idJson, mcpResourcesListJson(personaSnapshot())));
      continue;
    }

    if (message.method == "resources/read") {
      const std::string uri = jsonu::getString(message.params, "uri");
      if (uri.empty()) {
        responses.push_back(errorResponse(message.idJson, kJsonRpcInvalidParams, "Missing resource uri"));
        continue;
      }
      const auto contents = mcpResourceReadJson(personaSnapshot(), uri);
      if (!contents) {
        responses.push_back(errorResponse(message.idJson, kJsonRpcResourceNotFound, "Resource not found: " + uri));
        continue;
      }
      responses.push_back(resultResponse(message.idJson, *contents));
      continue;
    }

    if (message.method == "tools/call") {
      const std::string name = jsonu::getString(message.params, "name");
      if (name.empty()) {
        responses.push_back(errorResponse(message.idJson, kJsonRpcInvalidParams, "Missing tool name"));
        continue;
      }
      if (!mcpToolExists(name)) {
        responses.push_back(errorResponse(message.idJson, kJsonRpcMethodNotFound, "Unknown tool \"" + name + "\""));
        continue;
      }

      yyjson_val* arguments = jsonu::get(message.params, "arguments");
      const std::string argsJson = arguments ? valueToJson(arguments) : "{}";
      responses.push_back(resultResponse(message.idJson, dispatchTool(name, argsJson)));
      continue;
    }

    responses.push_back(errorResponse(message.idJson, kJsonRpcMethodNotFound, "Method not found: " + message.method));
  }

  return batchResponse(responses);
}

}  // namespace l2m
