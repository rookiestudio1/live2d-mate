#pragma once

// MCP 的 Streamable HTTP 伺服器。
// 不依賴任何 MCP SDK，JSON-RPC 那層自己刻在 core/jsonrpc。
//
// 無狀態：每個請求各自完成，不維護 session，也不用 SSE。
// 只回 application/json，但仍然接受請求帶
// Accept: text/event-stream（Claude Desktop 之類的用戶端一定會送）。
//
// 執行緒：cpp-httplib 的 listen 是阻塞式的，所以跑在自己的執行緒上；
// 工具呼叫透過 PendingCall + QueuedConnection 投遞回 GUI 執行緒。
// GUI 執行緒永遠不阻塞，被擋住的是 httplib 的 worker。

#include <QObject>
#include <QString>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/mcp_tool_specs.h"
#include "core/persona.h"
#include "pending_call.h"

namespace httplib {
class Server;
}

namespace l2m {

class McpTools;

class McpHttpServer : public QObject {
  Q_OBJECT

public:
  struct Status {
    bool running = false;
    // 實際監聽中的網址（含 /mcp）
    std::string url;
    // 啟動失敗的原因（顯示在設定視窗的狀態列）
    std::string error;
  };

  McpHttpServer(McpTools& tools, QString version, QObject* parent = nullptr);
  ~McpHttpServer() override;

  Status status() const;

  // 啟動；失敗時 status().error 會有原因。已經在跑會先停掉。
  void start(const std::string& host, int port, const std::optional<std::string>& token);
  void stop();

  // 角色描述的快照。**由 GUI 執行緒呼叫**（main() 接在
  // AppController::personaChanged 上），httplib 的 worker 執行緒在
  // handleRpc 裡讀複本。
  //
  // 這裡不會踩到 setStatus() 那個坑（見下面的註解）：setPersona 純寫狀態、
  // 不發任何訊號，personaSnapshot() 在鎖內複製、鎖外回傳。
  // 慣例照舊 —— 鎖的作用域只包住存取本身。
  void setPersona(PersonaSnapshot snapshot);

  // 發話節奏的快照，來源是 config 的 mcp.talkative / notifyOnComplete /
  // speakNoWait / announceSteps。**同樣由 GUI 執行緒呼叫**（main() 接在
  // ConfigStore::changed 上），worker 執行緒在 initialize 裡讀複本。
  //
  // 為什麼要推快照而不是在 worker 裡現讀 ConfigStore：ConfigStore 是 QObject，
  // 住在 GUI 執行緒，worker 直接讀就是跨執行緒讀一份會被改寫的結構。
  // 這條路徑本身純寫狀態、不發訊號，理由同 setPersona。
  //
  // 這四個開關不影響伺服器怎麼跑，只影響 initialize 送出的 instructions 文字，
  // 所以改了不必重啟伺服器 —— 但已連線的 AI 要重新連線才讀得到
  //（POST-only 的 httplib 沒有 SSE，推不了 notifications/*）。
  void setSpeechProtocol(SpeechProtocol protocol);

signals:
  void statusChanged();

private:
  // 更新狀態並通知。
  //
  // **一定要在解鎖之後才發訊號**：接收端（系統匣）會在 slot 裡回頭呼叫
  // status()，而 statusMutex_ 是非遞迴的 std::mutex。在鎖裡 emit 等於同一條
  // 執行緒重入自己持有的鎖，MSVC 會丟 std::system_error，而訊號的接收端
  // 沒有人接這個例外，整個 app 會直接 __fastfail。
  void setStatus(Status next);

  // 由 httplib 的工作執行緒呼叫：把一次 JSON-RPC 請求跑完並回傳回應文字
  std::string handleRpc(const std::string& body);
  // 把工具呼叫投遞給 GUI 執行緒並等結果
  std::string dispatchTool(const std::string& tool, const std::string& argsJson);
  PersonaSnapshot personaSnapshot() const;
  SpeechProtocol speechProtocol() const;

  McpTools& tools_;
  QString version_;

  std::unique_ptr<httplib::Server> server_;
  std::thread thread_;

  mutable std::mutex personaMutex_;
  PersonaSnapshot persona_;

  mutable std::mutex speechMutex_;
  SpeechProtocol speech_;

  mutable std::mutex statusMutex_;
  Status status_;
  std::string boundHost_;
  std::optional<std::string> token_;

  // 還在等結果的呼叫；關閉時要逐一放生，不然 worker 會對著死掉的
  // GUI 執行緒等滿整個期限
  std::mutex pendingMutex_;
  std::vector<PendingCallPtr> pending_;
  // 長呼叫（speak wait=true、perform）的併發上限，避免佔滿 worker pool
  std::atomic<int> longCalls_{0};
};

}  // namespace l2m
