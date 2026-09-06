#pragma once

// JSON-RPC 2.0 的最小 wire 層。
//
// C++ 沒有官方的 MCP SDK，
// 所以 initialize / tools/list / tools/call 這三個方法與底下的 JSON-RPC
// 訊框都得自己刻。範圍刻意只做 MCP 真的會用到的部分：
// 單筆與批次請求、通知（沒有 id）、標準錯誤碼。
//
// id 一律以「原始 JSON 文字」保存而不是解析成型別：規格允許數字或字串，
// 回覆時必須原封不動照抄，轉成型別再轉回去只會製造出 1 與 "1" 的差異。

#include <yyjson.h>

#include <optional>
#include <string>
#include <vector>

namespace l2m {

// 標準錯誤碼
inline constexpr int kJsonRpcParseError = -32700;
inline constexpr int kJsonRpcInvalidRequest = -32600;
inline constexpr int kJsonRpcMethodNotFound = -32601;
inline constexpr int kJsonRpcInvalidParams = -32602;
inline constexpr int kJsonRpcInternalError = -32603;
// MCP 規範：resources/read 指到不存在的資源
inline constexpr int kJsonRpcResourceNotFound = -32002;
// 伺服器自訂：工具執行期間的失敗
inline constexpr int kJsonRpcServerError = -32000;

struct RpcMessage {
  std::string method;
  // id 的原始 JSON 文字；通知沒有 id，這裡是空字串
  std::string idJson;
  // 指向原始文件裡的 params 節點（文件必須活得比它久）
  yyjson_val* params = nullptr;

  bool isNotification() const { return idJson.empty(); }
};

// 解析請求（單筆或批次）。整份不合法時回 nullopt。
std::optional<std::vector<RpcMessage>> parseRequest(yyjson_val* root);

// 把某個值序列化回 JSON 文字（用來保存 id）
std::string valueToJson(yyjson_val* value);

// 回覆：result 與 error 都吃「已經序列化好的 JSON 文字」
std::string resultResponse(const std::string& idJson, const std::string& resultJson);
std::string errorResponse(const std::string& idJson, int code, const std::string& message);

// 把多筆回覆包成批次；只有一筆時直接回那一筆（規格要求）
std::string batchResponse(const std::vector<std::string>& responses);

}  // namespace l2m
