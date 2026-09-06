#include "jsonrpc.h"

#include <cstdlib>

#include "json_doc.h"

namespace l2m {

namespace {

// JSON 字串逸出（回覆裡的 message 由我們自己產生，不會有控制字元以外的怪東西）
std::string escapeJson(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() + 8);
  for (const unsigned char c : raw) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          static const char* kHex = "0123456789abcdef";
          out += "\\u00";
          out += kHex[c >> 4];
          out += kHex[c & 0x0F];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

std::optional<RpcMessage> parseOne(yyjson_val* item) {
  if (!item || !yyjson_is_obj(item)) return std::nullopt;

  yyjson_val* method = jsonu::get(item, "method");
  const char* name = method ? yyjson_get_str(method) : nullptr;
  if (!name) return std::nullopt;

  RpcMessage message;
  message.method = name;
  message.params = jsonu::get(item, "params");

  yyjson_val* id = jsonu::get(item, "id");
  // null 的 id 依規格等同「沒有 id」，當成通知處理
  if (id && !yyjson_is_null(id)) message.idJson = valueToJson(id);
  return message;
}

}  // namespace

std::string valueToJson(yyjson_val* value) {
  if (!value) return "null";
  size_t length = 0;
  char* text = yyjson_val_write(value, 0, &length);
  if (!text) return "null";
  std::string out(text, length);
  free(text);
  return out;
}

std::optional<std::vector<RpcMessage>> parseRequest(yyjson_val* root) {
  if (!root) return std::nullopt;

  std::vector<RpcMessage> messages;
  if (yyjson_is_arr(root)) {
    // 空批次依規格是 Invalid Request
    if (yyjson_arr_size(root) == 0) return std::nullopt;
    yyjson_arr_iter iter;
    yyjson_arr_iter_init(root, &iter);
    yyjson_val* item = nullptr;
    while ((item = yyjson_arr_iter_next(&iter))) {
      auto parsed = parseOne(item);
      if (!parsed) return std::nullopt;
      messages.push_back(*parsed);
    }
    return messages;
  }

  auto parsed = parseOne(root);
  if (!parsed) return std::nullopt;
  messages.push_back(*parsed);
  return messages;
}

std::string resultResponse(const std::string& idJson, const std::string& resultJson) {
  return "{\"jsonrpc\":\"2.0\",\"id\":" + (idJson.empty() ? "null" : idJson) + ",\"result\":" + (resultJson.empty() ? "{}" : resultJson) + "}";
}

std::string errorResponse(const std::string& idJson, int code, const std::string& message) {
  return "{\"jsonrpc\":\"2.0\",\"id\":" + (idJson.empty() ? "null" : idJson) + ",\"error\":{\"code\":" + std::to_string(code) + ",\"message\":\"" + escapeJson(message) + "\"}}";
}

std::string batchResponse(const std::vector<std::string>& responses) {
  if (responses.empty()) return {};
  if (responses.size() == 1) return responses.front();

  std::string out = "[";
  for (size_t i = 0; i < responses.size(); ++i) {
    if (i > 0) out += ",";
    out += responses[i];
  }
  out += "]";
  return out;
}

}  // namespace l2m
