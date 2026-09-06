#include <httplib.h>

#include "mcp_stdio.h"

#include <yyjson.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include <QDebug>

#include "core/json_doc.h"

namespace l2m {

namespace {

constexpr const char* kDefaultUrl = "http://127.0.0.1:3777/mcp";

// 錯誤訊息裡最多帶多少回應內容，太長會把 client 的日誌洗爆
constexpr size_t kErrorBodyLimit = 300;

// speak wait=true 最長 185 秒，橋接的逾時要留得比它寬
constexpr time_t kReadTimeoutSec = 200;
constexpr time_t kWriteTimeoutSec = 30;

// 幾條 worker 同時在跟伺服器說話。
//
// 舊版是單執行緒的「讀一行 → POST → 等回應 → 讀下一行」，於是一個 speak／perform
// （伺服器端期限 185 秒）會把後面**所有**請求鎖在 stdin 裡沒人去讀。
// 實測：長呼叫進行中送一個 get_state，經舊橋接要 7.95 秒才回，直連 HTTP 只要 0.09 秒 ——
// 伺服器端的併發一直是好的，瓶頸完全在這支橋接。而 Claude Desktop 會同時發多個請求
// （日誌裡 tools/list 與 resources/list 就是同一毫秒送出的），排在長呼叫後面的那些
// 光排隊就會撞上 client 自己的請求期限，症狀就是「用一用就 timeout」。
//
// 8 條的理由：伺服器端同時最多 4 個長呼叫（McpHttpServer 的 kMaxConcurrentLongCalls），
// 長呼叫全部佔滿之後仍要留餘裕給 get_state 這類瞬時查詢。
constexpr int kWorkerCount = 8;

// stdin 收到 EOF（client 走了）之後，最多再等飛行中的請求多久。
// 不設上限的話，卡在 Post() 裡的 worker 最長要等滿 kReadTimeoutSec 才會醒 ——
// Claude Desktop 被強制關閉時，橋接就會在背景賴著好幾分鐘。
constexpr int kShutdownGraceMs = 3000;

struct Endpoint {
  std::string host;  // 含 scheme 的 host:port，給 httplib::Client
  std::string path;  // /mcp
};

// 把完整 URL 拆成 httplib 要的 (host, path)
Endpoint splitUrl(const std::string& url) {
  Endpoint endpoint;
  const auto schemeEnd = url.find("://");
  if (schemeEnd == std::string::npos) {
    endpoint.host = url;
    endpoint.path = "/";
    return endpoint;
  }
  const auto pathStart = url.find('/', schemeEnd + 3);
  if (pathStart == std::string::npos) {
    endpoint.host = url;
    endpoint.path = "/";
    return endpoint;
  }
  endpoint.host = url.substr(0, pathStart);
  endpoint.path = url.substr(pathStart);
  return endpoint;
}

std::string envOr(const char* name, const std::string& fallback) {
  const char* value = std::getenv(name);
  return (value && *value) ? std::string(value) : fallback;
}

// 所有 worker 共用的狀態。橋接不解讀轉發的內容，所以只有三樣東西要保護：
// 待處理佇列、stdout、以及 initialize 協商到的協定版本。
struct BridgeContext {
  std::string url;
  Endpoint endpoint;
  std::string token;

  std::mutex queueMutex;
  std::condition_variable queueCv;
  std::deque<std::string> queue;
  bool stopping = false;

  // 一則 JSON-RPC 訊息必須整行原子寫出，否則兩個 worker 的回應會交錯成廢話
  std::mutex outMutex;

  // initialize 協商到的版本，之後每個請求都要帶。併發之下仍然安全：
  // MCP 規範要求 client 等到 initialize 的回覆才發後續請求，
  // 所以真正需要這個值的請求一定排在它後面。
  std::mutex versionMutex;
  std::string protocolVersion;

  // 還沒結束的 worker 數，收尾時用
  std::mutex doneMutex;
  std::condition_variable doneCv;
  int running = 0;
};

void writeLine(BridgeContext& ctx, const std::string& text) {
  std::lock_guard<std::mutex> lock(ctx.outMutex);
  std::fwrite(text.data(), 1, text.size(), stdout);
  std::fputc('\n', stdout);
  std::fflush(stdout);
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
    } else if (c == '\r') {
      out += "\\r";
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

// 伺服器可能以 SSE 或純 JSON 回覆，兩種都要拆成一則則 JSON-RPC 訊息
std::vector<std::string> parseBody(const std::string& contentType, const std::string& body) {
  std::vector<std::string> messages;
  if (body.find_first_not_of(" \t\r\n") == std::string::npos) return messages;

  if (contentType.find("text/event-stream") != std::string::npos) {
    std::istringstream stream(body);
    std::string line;
    while (std::getline(stream, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.rfind("data:", 0) != 0) continue;
      std::string payload = line.substr(5);
      const auto begin = payload.find_first_not_of(" \t");
      if (begin == std::string::npos) continue;
      payload = payload.substr(begin);
      // 不是完整 JSON 的 data 行直接略過
      if (jsonu::Doc::parse(payload)) messages.push_back(payload);
    }
    return messages;
  }

  auto doc = jsonu::Doc::parse(body);
  if (!doc || !doc->root()) return messages;
  if (yyjson_is_arr(doc->root())) {
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(doc->root(), &iter);
    yyjson_val* item = nullptr;
    while ((item = yyjson_arr_iter_next(&iter))) {
      size_t length = 0;
      char* text = yyjson_val_write(item, 0, &length);
      if (!text) continue;
      messages.emplace_back(text, length);
      free(text);
    }
  } else {
    messages.push_back(body);
  }
  return messages;
}

// 從一則請求裡取出 id 的原始 JSON（回錯誤時要原封不動照抄）
std::string extractId(const std::string& message, bool* isRequest) {
  *isRequest = false;
  auto doc = jsonu::Doc::parse(message);
  if (!doc || !doc->root()) return "null";

  yyjson_val* method = jsonu::get(doc->root(), "method");
  yyjson_val* id = jsonu::get(doc->root(), "id");
  if (!method || !id || yyjson_is_null(id)) return "null";

  *isRequest = true;
  size_t length = 0;
  char* text = yyjson_val_write(id, 0, &length);
  if (!text) return "null";
  std::string out(text, length);
  free(text);
  return out;
}

// 轉發一則訊息：POST 給伺服器，把回來的內容逐則寫回 stdout。
// 內容一律原封不動，只有連不上或非 2xx 時才由橋接自己生一則 JSON-RPC error。
void handleMessage(BridgeContext& ctx, httplib::Client& client, const std::string& line) {
  bool isRequest = false;
  const std::string id = extractId(line, &isRequest);

  httplib::Headers headers{{"Content-Type", "application/json"},
                           // 一定要同時宣告這兩種 Accept，Streamable HTTP 規範才會接受 POST
                           {"Accept", "application/json, text/event-stream"}};
  if (!ctx.token.empty()) headers.emplace("Authorization", "Bearer " + ctx.token);
  {
    std::lock_guard<std::mutex> lock(ctx.versionMutex);
    if (!ctx.protocolVersion.empty()) {
      headers.emplace("MCP-Protocol-Version", ctx.protocolVersion);
    }
  }

  auto res = client.Post(ctx.endpoint.path, headers, line, "application/json");
  if (!res) {
    const std::string detail = httplib::to_string(res.error());
    if (isRequest) {
      writeLine(ctx, "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":-32000,\"message\":\"Cannot reach Live2D Mate (" + jsonEscape(ctx.url) + "): " + jsonEscape(detail) + "\"}}");
    } else {
      qWarning() << "[bridge]" << QString::fromStdString(detail);
    }
    return;
  }

  if (res->status < 200 || res->status >= 300) {
    const std::string body = res->body.substr(0, kErrorBodyLimit);
    if (isRequest) {
      writeLine(ctx, "{\"jsonrpc\":\"2.0\",\"id\":" + id + ",\"error\":{\"code\":-32000,\"message\":\"HTTP " + std::to_string(res->status) + ": " + jsonEscape(body) + "\"}}");
    } else {
      qWarning() << "[bridge] HTTP" << res->status << ":" << QString::fromStdString(body);
    }
    return;
  }

  const std::string contentType = res->has_header("Content-Type") ? res->get_header_value("Content-Type") : std::string();
  for (const std::string& out : parseBody(contentType, res->body)) {
    if (auto doc = jsonu::Doc::parse(out)) {
      yyjson_val* result = jsonu::get(doc->root(), "result");
      const std::string negotiated = jsonu::getString(result, "protocolVersion");
      if (!negotiated.empty()) {
        std::lock_guard<std::mutex> lock(ctx.versionMutex);
        ctx.protocolVersion = negotiated;
      }
    }
    writeLine(ctx, out);
  }
}

// 一條 worker：自己持有一個 httplib::Client（Client 不是執行緒安全的），
// 從佇列拿一則就轉發一則。stopping 之後仍會把佇列裡剩下的做完才收工。
void workerLoop(BridgeContext& ctx) {
  httplib::Client client(ctx.endpoint.host);
  client.set_read_timeout(kReadTimeoutSec, 0);
  client.set_write_timeout(kWriteTimeoutSec, 0);

  for (;;) {
    std::string line;
    {
      std::unique_lock<std::mutex> lock(ctx.queueMutex);
      ctx.queueCv.wait(lock, [&ctx] { return ctx.stopping || !ctx.queue.empty(); });
      if (ctx.queue.empty()) break;
      line = std::move(ctx.queue.front());
      ctx.queue.pop_front();
    }
    handleMessage(ctx, client, line);
  }

  {
    std::lock_guard<std::mutex> lock(ctx.doneMutex);
    --ctx.running;
  }
  ctx.doneCv.notify_all();
}

}  // namespace

int runMcpStdioBridge(int argc, char* argv[]) {
#ifdef _WIN32
  // 二進位模式：不要讓 CRT 把 \n 翻成 \r\n，JSON-RPC 是逐行協定
  _setmode(_fileno(stdin), _O_BINARY);
  _setmode(_fileno(stdout), _O_BINARY);
#endif

  BridgeContext ctx;
  ctx.url = argc >= 3 && argv[2][0] != '\0' ? std::string(argv[2]) : envOr("L2D_MCP_URL", kDefaultUrl);
  ctx.token = argc >= 4 && argv[3][0] != '\0' ? std::string(argv[3]) : envOr("L2D_MCP_TOKEN", "");
  ctx.endpoint = splitUrl(ctx.url);
  ctx.running = kWorkerCount;

  std::vector<std::thread> workers;
  workers.reserve(kWorkerCount);
  for (int i = 0; i < kWorkerCount; ++i) workers.emplace_back([&ctx] { workerLoop(ctx); });

  // 主執行緒只做一件事：把 stdin 讀進佇列。**永遠不阻塞在 HTTP 上**，
  // 長呼叫進行中後面的請求照樣進得來，client 關掉 stdin 也能立刻發現。
  std::string line;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.find_first_not_of(" \t") == std::string::npos) continue;
    {
      std::lock_guard<std::mutex> lock(ctx.queueMutex);
      ctx.queue.push_back(std::move(line));
    }
    ctx.queueCv.notify_one();
  }

  {
    std::lock_guard<std::mutex> lock(ctx.queueMutex);
    ctx.stopping = true;
  }
  ctx.queueCv.notify_all();

  {
    std::unique_lock<std::mutex> lock(ctx.doneMutex);
    if (ctx.doneCv.wait_for(lock, std::chrono::milliseconds(kShutdownGraceMs), [&ctx] { return ctx.running == 0; })) {
      lock.unlock();
      for (auto& worker : workers) worker.join();
      return 0;
    }
  }

  // 還有 worker 卡在 Post() 裡。client 早就不在了，回應沒人收，
  // 不能為了它們把橋接留在背景 —— 直接離開，跳過所有解構。
  std::_Exit(0);
}

}  // namespace l2m
