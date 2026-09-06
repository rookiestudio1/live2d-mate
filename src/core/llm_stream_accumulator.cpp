#include "llm_stream_accumulator.h"

#include "json_doc.h"

namespace l2m {

namespace {

// 伺服器塞在串流裡的錯誤物件（兩家形狀恰好相同：{"error":{"message":…}}）
std::string errorMessageOf(yyjson_val* root) {
  yyjson_val* error = jsonu::get(root, "error");
  if (!error) return {};
  const std::string message = jsonu::getString(error, "message");
  if (!message.empty()) return message;
  return jsonu::getString(error, "type", "unknown error");
}

}  // namespace

// ── OpenAI ─────────────────────────────────────────────────

void OpenAiStreamAccumulator::flushTools(LlmStreamUpdate& update) {
  if (toolsFlushed_) return;
  toolsFlushed_ = true;
  for (auto& tool : tools_) {
    if (tool.id.empty() && tool.name.empty()) continue;  // index 缺項的填充位
    update.toolCalls.push_back({tool.id, tool.name, tool.arguments});
  }
  tools_.clear();
}

std::vector<LlmStreamUpdate> OpenAiStreamAccumulator::feed(const SseEvent& event) {
  std::vector<LlmStreamUpdate> out;

  // 應用層哨兵。有些相容服務不送 finish_reason 只送 [DONE]，
  // 工具呼叫在這裡也要補 flush
  if (event.data == "[DONE]") {
    LlmStreamUpdate update;
    flushTools(update);
    update.done = true;
    out.push_back(std::move(update));
    return out;
  }

  const auto doc = jsonu::Doc::parse(event.data);
  if (!doc || !doc->root()) return out;  // 解不開的 chunk 直接略過，不讓整條串流失敗
  yyjson_val* root = doc->root();

  if (const std::string error = errorMessageOf(root); !error.empty()) {
    LlmStreamUpdate update;
    update.error = error;
    out.push_back(std::move(update));
    return out;
  }

  LlmStreamUpdate update;
  bool meaningful = false;

  if (const std::string model = jsonu::getString(root, "model"); !model.empty()) {
    update.model = model;
    meaningful = true;
  }

  yyjson_val* choices = jsonu::get(root, "choices");
  yyjson_val* choice = choices ? yyjson_arr_get(choices, 0) : nullptr;
  if (choice) {
    if (yyjson_val* delta = jsonu::get(choice, "delta")) {
      const std::string content = jsonu::getString(delta, "content");
      if (!content.empty()) {
        update.textDelta = content;
        meaningful = true;
      }
      if (yyjson_val* toolCalls = jsonu::get(delta, "tool_calls")) {
        size_t idx, max;
        yyjson_val* call;
        yyjson_arr_foreach(toolCalls, idx, max, call) {
          // index 對齊：同一個工具呼叫的碎片都帶同一個 index
          int slot = static_cast<int>(idx);
          if (yyjson_val* indexVal = jsonu::get(call, "index"); indexVal && yyjson_is_int(indexVal)) {
            slot = static_cast<int>(yyjson_get_int(indexVal));
          }
          if (slot < 0) continue;
          if (static_cast<size_t>(slot) >= tools_.size()) {
            tools_.resize(static_cast<size_t>(slot) + 1);
          }
          PendingTool& tool = tools_[static_cast<size_t>(slot)];
          const std::string id = jsonu::getString(call, "id");
          if (!id.empty()) tool.id = id;
          if (yyjson_val* function = jsonu::get(call, "function")) {
            const std::string name = jsonu::getString(function, "name");
            if (!name.empty()) tool.name = name;
            tool.arguments += jsonu::getString(function, "arguments");
          }
        }
      }
    }
    yyjson_val* finish = jsonu::get(choice, "finish_reason");
    if (finish && yyjson_is_str(finish)) {
      update.finishReason = yyjson_get_str(finish);
      flushTools(update);
      meaningful = true;
    }
  }

  if (meaningful || !update.toolCalls.empty()) out.push_back(std::move(update));
  return out;
}

// ── Anthropic ──────────────────────────────────────────────

std::vector<LlmStreamUpdate> AnthropicStreamAccumulator::feed(const SseEvent& event) {
  std::vector<LlmStreamUpdate> out;
  if (event.event == "ping") return out;

  const auto doc = jsonu::Doc::parse(event.data);
  if (!doc || !doc->root()) return out;
  yyjson_val* root = doc->root();
  // 事件名以 data 裡的 type 為準（event: 欄位理論上相同，但只信一處）
  const std::string type = jsonu::getString(root, "type", event.event);

  if (type == "error") {
    LlmStreamUpdate update;
    update.error = errorMessageOf(root);
    if (update.error.empty()) update.error = "unknown stream error";
    out.push_back(std::move(update));
    return out;
  }

  if (type == "message_start") {
    yyjson_val* message = jsonu::get(root, "message");
    const std::string model = jsonu::getString(message, "model");
    if (!model.empty()) {
      LlmStreamUpdate update;
      update.model = model;
      out.push_back(std::move(update));
    }
    return out;
  }

  const auto indexOf = [root]() -> int {
    yyjson_val* index = jsonu::get(root, "index");
    return index && yyjson_is_int(index) ? static_cast<int>(yyjson_get_int(index)) : -1;
  };

  if (type == "content_block_start") {
    yyjson_val* block = jsonu::get(root, "content_block");
    if (jsonu::getString(block, "type") == "tool_use") {
      PendingTool tool;
      tool.index = indexOf();
      tool.id = jsonu::getString(block, "id");
      tool.name = jsonu::getString(block, "name");
      tools_.push_back(std::move(tool));
    }
    return out;
  }

  if (type == "content_block_delta") {
    yyjson_val* delta = jsonu::get(root, "delta");
    const std::string deltaType = jsonu::getString(delta, "type");
    if (deltaType == "text_delta") {
      const std::string text = jsonu::getString(delta, "text");
      if (!text.empty()) {
        LlmStreamUpdate update;
        update.textDelta = text;
        out.push_back(std::move(update));
      }
    } else if (deltaType == "input_json_delta") {
      const int index = indexOf();
      for (auto& tool : tools_) {
        if (tool.index == index) {
          tool.arguments += jsonu::getString(delta, "partial_json");
          break;
        }
      }
    }
    return out;
  }

  if (type == "content_block_stop") {
    const int index = indexOf();
    for (auto it = tools_.begin(); it != tools_.end(); ++it) {
      if (it->index == index) {
        LlmStreamUpdate update;
        // Anthropic 的空參數工具呼叫 input_json_delta 一次都不來，補上空物件
        update.toolCalls.push_back({it->id, it->name, it->arguments.empty() ? "{}" : it->arguments});
        tools_.erase(it);
        out.push_back(std::move(update));
        break;
      }
    }
    return out;
  }

  if (type == "message_delta") {
    yyjson_val* delta = jsonu::get(root, "delta");
    const std::string stop = jsonu::getString(delta, "stop_reason");
    if (!stop.empty()) {
      LlmStreamUpdate update;
      // 對映到 OpenAI 字彙，呼叫端只需要認一套
      if (stop == "end_turn")
        update.finishReason = "stop";
      else if (stop == "tool_use")
        update.finishReason = "tool_calls";
      else if (stop == "max_tokens")
        update.finishReason = "length";
      else
        update.finishReason = stop;
      out.push_back(std::move(update));
    }
    return out;
  }

  if (type == "message_stop") {
    LlmStreamUpdate update;
    update.done = true;
    out.push_back(std::move(update));
    return out;
  }

  return out;
}

}  // namespace l2m
