#pragma once

// MCP 的 24 個工具的實作。
//
// 規格表（名稱、說明、JSON Schema）與輸出格式在 core/mcp_tool_specs ——
// 那些是純資料，抽出去才測得到；這裡只負責「把參數解出來、打 AppController、
// 把 CommandResult 包成工具輸出」。
//
// 執行緒：invoke() 一律在 GUI 執行緒跑（由 McpHttpServer 以 QueuedConnection
// 投遞進來），所以可以安全地碰 AppController、模型與視窗。

#include <QObject>

#include <string>

#include "core/mcp_tool_specs.h"
#include "pending_call.h"

namespace l2m {

class AppController;

class McpTools : public QObject {
  Q_OBJECT

public:
  explicit McpTools(AppController& controller, QObject* parent = nullptr);

  // 執行一個工具並把輸出填回 call（可能是非同步完成）
  void invoke(PendingCallPtr call);
  // 射後不理：schedule 到期時回頭呼叫其他工具走這條，沒有等待端
  void invokeDetached(const std::string& tool, const std::string& argsJson);

private:
  AppController& controller_;
};

}  // namespace l2m
