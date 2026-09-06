#include "llm_http.h"

#include <cstdlib>

#include "json_doc.h"
#include "string_util.h"
#include "tts_http.h"  // normalizeBaseUrl

namespace l2m {

namespace {

// 訊息陣列（OpenAI 形狀）。assistant 的 tool_calls 與 tool 結果的 tool_call_id
// 第一期還沒有呼叫端會填，但組裝規則先定死並被測試釘住 —— 聊天迴圈階段
// 接上時不會再動這裡。
void putOpenAiMessages(jsonu::MutDoc& doc, yyjson_mut_val* root, const std::vector<LlmMessage>& messages) {
  yyjson_mut_val* arr = yyjson_mut_arr(doc.get());
  yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "messages"), arr);
  for (const auto& message : messages) {
    yyjson_mut_val* obj = yyjson_mut_arr_add_obj(doc.get(), arr);
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "role"), yyjson_mut_strcpy(doc.get(), message.role.c_str()));
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "content"), yyjson_mut_strcpy(doc.get(), message.text.c_str()));
    if (message.role == "tool" && !message.toolCallId.empty()) {
      yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "tool_call_id"), yyjson_mut_strcpy(doc.get(), message.toolCallId.c_str()));
    }
    if (message.role == "assistant" && !message.toolCalls.empty()) {
      yyjson_mut_val* calls = yyjson_mut_arr(doc.get());
      yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "tool_calls"), calls);
      for (const auto& call : message.toolCalls) {
        yyjson_mut_val* callObj = yyjson_mut_arr_add_obj(doc.get(), calls);
        yyjson_mut_obj_put(callObj, yyjson_mut_strcpy(doc.get(), "id"), yyjson_mut_strcpy(doc.get(), call.id.c_str()));
        yyjson_mut_obj_put(callObj, yyjson_mut_strcpy(doc.get(), "type"), yyjson_mut_strcpy(doc.get(), "function"));
        yyjson_mut_val* fn = yyjson_mut_obj(doc.get());
        yyjson_mut_obj_put(callObj, yyjson_mut_strcpy(doc.get(), "function"), fn);
        yyjson_mut_obj_put(fn, yyjson_mut_strcpy(doc.get(), "name"), yyjson_mut_strcpy(doc.get(), call.name.c_str()));
        yyjson_mut_obj_put(fn, yyjson_mut_strcpy(doc.get(), "arguments"), yyjson_mut_strcpy(doc.get(), call.argumentsJson.c_str()));
      }
    }
  }
}

// toolsJson（OpenAI tools 陣列原文）解析後深拷貝進請求。
// 解不開就整個不帶 —— 寧可少工具也不要送出壞 JSON 讓整個請求 400。
void putRawTools(jsonu::MutDoc& doc, yyjson_mut_val* root, const std::string& toolsJson) {
  if (toolsJson.empty()) return;
  const auto parsed = jsonu::Doc::parse(toolsJson);
  if (!parsed || !parsed->root() || !yyjson_is_arr(parsed->root())) return;
  yyjson_mut_val* copy = doc.copyOf(parsed->root());
  if (copy) yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "tools"), copy);
}

}  // namespace

std::string llmConfigIssue(const LlmConfig& config) {
  if (config.provider == "anthropic") {
    if (strutil::trim(config.anthropicApiKey).empty()) {
      return "Anthropic API key is not set. Enter your API key (starts with sk-ant-) in "
             "Settings > LLM.";
    }
    if (strutil::trim(config.anthropicModel).empty()) {
      return "Anthropic model is not set. Enter a model id such as claude-opus-5.";
    }
    return {};
  }
  if (normalizeBaseUrl(config.baseUrl).empty()) {
    return "LLM base URL is not set. Enter an OpenAI-compatible endpoint such as "
           "http://127.0.0.1:11434/v1 (Ollama).";
  }
  if (strutil::trim(config.model).empty()) {
    return "LLM model is not set. Use Test connection to list available models, then pick "
           "one.";
  }
  return {};
}

LlmHttpRequest buildChatCompletionsRequest(const LlmConfig& config, const std::vector<LlmMessage>& messages, const LlmChatOptions& options, bool stream) {
  LlmHttpRequest request;
  request.error = llmConfigIssue(config);
  if (!request.error.empty()) return request;

  request.url = normalizeBaseUrl(config.baseUrl) + "/chat/completions";
  request.contentType = "application/json";
  // local 服務（Ollama / LM Studio）不需要金鑰；沒填就不送 Authorization
  if (!strutil::trim(config.apiKey).empty()) {
    request.headers.emplace_back("Authorization", "Bearer " + strutil::trim(config.apiKey));
  }

  jsonu::MutDoc doc;
  yyjson_mut_val* root = yyjson_mut_obj(doc.get());
  doc.setRoot(root);
  yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "model"), yyjson_mut_strcpy(doc.get(), strutil::trim(config.model).c_str()));
  putOpenAiMessages(doc, root, messages);
  yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "temperature"), yyjson_mut_real(doc.get(), options.temperature ? *options.temperature : config.temperature));
  yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "max_tokens"), yyjson_mut_sint(doc.get(), options.maxTokens ? *options.maxTokens : config.maxTokens));
  if (stream) {
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "stream"), yyjson_mut_bool(doc.get(), true));
  }
  putRawTools(doc, root, options.toolsJson);

  request.body = doc.write();
  return request;
}

LlmHttpRequest buildAnthropicMessagesRequest(const LlmConfig& config, const std::vector<LlmMessage>& messages, const LlmChatOptions& options, bool stream) {
  LlmHttpRequest request;
  request.error = llmConfigIssue(config);
  if (!request.error.empty()) return request;

  request.url = kAnthropicMessagesUrl;
  request.contentType = "application/json";
  request.headers.emplace_back("x-api-key", strutil::trim(config.anthropicApiKey));
  request.headers.emplace_back("anthropic-version", kAnthropicVersion);

  jsonu::MutDoc doc;
  yyjson_mut_val* root = yyjson_mut_obj(doc.get());
  doc.setRoot(root);
  yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "model"), yyjson_mut_strcpy(doc.get(), strutil::trim(config.anthropicModel).c_str()));
  yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "max_tokens"), yyjson_mut_sint(doc.get(), options.maxTokens ? *options.maxTokens : config.maxTokens));

  // system 抽成頂層欄位（多則 system 以空行串接）
  std::string system;
  for (const auto& message : messages) {
    if (message.role != "system") continue;
    if (!system.empty()) system += "\n\n";
    system += message.text;
  }
  if (!system.empty()) {
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "system"), yyjson_mut_strcpy(doc.get(), system.c_str()));
  }

  yyjson_mut_val* arr = yyjson_mut_arr(doc.get());
  yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "messages"), arr);
  for (const auto& message : messages) {
    if (message.role == "system") continue;
    yyjson_mut_val* obj = yyjson_mut_arr_add_obj(doc.get(), arr);
    if (message.role == "tool") {
      // 工具結果在 Anthropic 是 user 訊息裡的 tool_result 區塊
      yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "role"), yyjson_mut_strcpy(doc.get(), "user"));
      yyjson_mut_val* content = yyjson_mut_arr(doc.get());
      yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "content"), content);
      yyjson_mut_val* block = yyjson_mut_arr_add_obj(doc.get(), content);
      yyjson_mut_obj_put(block, yyjson_mut_strcpy(doc.get(), "type"), yyjson_mut_strcpy(doc.get(), "tool_result"));
      yyjson_mut_obj_put(block, yyjson_mut_strcpy(doc.get(), "tool_use_id"), yyjson_mut_strcpy(doc.get(), message.toolCallId.c_str()));
      yyjson_mut_obj_put(block, yyjson_mut_strcpy(doc.get(), "content"), yyjson_mut_strcpy(doc.get(), message.text.c_str()));
      continue;
    }
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "role"), yyjson_mut_strcpy(doc.get(), message.role.c_str()));
    if (message.role == "assistant" && !message.toolCalls.empty()) {
      yyjson_mut_val* content = yyjson_mut_arr(doc.get());
      yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "content"), content);
      if (!message.text.empty()) {
        yyjson_mut_val* textBlock = yyjson_mut_arr_add_obj(doc.get(), content);
        yyjson_mut_obj_put(textBlock, yyjson_mut_strcpy(doc.get(), "type"), yyjson_mut_strcpy(doc.get(), "text"));
        yyjson_mut_obj_put(textBlock, yyjson_mut_strcpy(doc.get(), "text"), yyjson_mut_strcpy(doc.get(), message.text.c_str()));
      }
      for (const auto& call : message.toolCalls) {
        yyjson_mut_val* block = yyjson_mut_arr_add_obj(doc.get(), content);
        yyjson_mut_obj_put(block, yyjson_mut_strcpy(doc.get(), "type"), yyjson_mut_strcpy(doc.get(), "tool_use"));
        yyjson_mut_obj_put(block, yyjson_mut_strcpy(doc.get(), "id"), yyjson_mut_strcpy(doc.get(), call.id.c_str()));
        yyjson_mut_obj_put(block, yyjson_mut_strcpy(doc.get(), "name"), yyjson_mut_strcpy(doc.get(), call.name.c_str()));
        const auto args = jsonu::Doc::parse(call.argumentsJson);
        yyjson_mut_val* input = args && args->root() ? doc.copyOf(args->root()) : yyjson_mut_obj(doc.get());
        yyjson_mut_obj_put(block, yyjson_mut_strcpy(doc.get(), "input"), input);
      }
      continue;
    }
    yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "content"), yyjson_mut_strcpy(doc.get(), message.text.c_str()));
  }

  yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "temperature"), yyjson_mut_real(doc.get(), options.temperature ? *options.temperature : config.temperature));
  if (stream) {
    yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "stream"), yyjson_mut_bool(doc.get(), true));
  }

  // OpenAI tools 形狀 → Anthropic：function.{name,description,parameters}
  // 攤平成 {name,description,input_schema}
  if (!options.toolsJson.empty()) {
    const auto parsed = jsonu::Doc::parse(options.toolsJson);
    if (parsed && parsed->root() && yyjson_is_arr(parsed->root())) {
      yyjson_mut_val* tools = yyjson_mut_arr(doc.get());
      yyjson_mut_obj_put(root, yyjson_mut_strcpy(doc.get(), "tools"), tools);
      size_t idx, max;
      yyjson_val* tool;
      yyjson_arr_foreach(parsed->root(), idx, max, tool) {
        yyjson_val* fn = jsonu::get(tool, "function");
        if (!fn) fn = tool;  // 已是攤平形狀就原樣用
        yyjson_mut_val* obj = yyjson_mut_arr_add_obj(doc.get(), tools);
        yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "name"), yyjson_mut_strcpy(doc.get(), jsonu::getString(fn, "name").c_str()));
        const std::string description = jsonu::getString(fn, "description");
        if (!description.empty()) {
          yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "description"), yyjson_mut_strcpy(doc.get(), description.c_str()));
        }
        yyjson_val* schema = jsonu::get(fn, "parameters");
        if (!schema) schema = jsonu::get(fn, "input_schema");
        yyjson_mut_val* input = schema ? doc.copyOf(schema) : yyjson_mut_obj(doc.get());
        yyjson_mut_obj_put(obj, yyjson_mut_strcpy(doc.get(), "input_schema"), input);
      }
    }
  }

  request.body = doc.write();
  return request;
}

LlmHttpRequest buildLlmChatRequest(const LlmConfig& config, const std::vector<LlmMessage>& messages, const LlmChatOptions& options, bool stream) {
  if (config.provider == "anthropic") {
    return buildAnthropicMessagesRequest(config, messages, options, stream);
  }
  return buildChatCompletionsRequest(config, messages, options, stream);
}

LlmHttpRequest buildListModelsRequest(const LlmConfig& config) {
  LlmHttpRequest request;
  if (config.provider == "anthropic") {
    if (strutil::trim(config.anthropicApiKey).empty()) {
      // 與 llmConfigIssue 的第一句相同 —— 清單與對話的前置條件一致
      request.error = llmConfigIssue(config);
      return request;
    }
    request.url = kAnthropicModelsUrl;
    request.headers.emplace_back("x-api-key", strutil::trim(config.anthropicApiKey));
    request.headers.emplace_back("anthropic-version", kAnthropicVersion);
    return request;
  }
  const std::string base = normalizeBaseUrl(config.baseUrl);
  if (base.empty()) {
    request.error = llmConfigIssue(config);
    return request;
  }
  request.url = base + "/models";
  if (!strutil::trim(config.apiKey).empty()) {
    request.headers.emplace_back("Authorization", "Bearer " + strutil::trim(config.apiKey));
  }
  return request;
}

std::vector<std::string> parseModelListJson(const std::string& json) {
  std::vector<std::string> models;
  const auto doc = jsonu::Doc::parse(json);
  if (!doc || !doc->root()) return models;
  yyjson_val* data = jsonu::get(doc->root(), "data");
  if (!data || !yyjson_is_arr(data)) return models;
  size_t idx, max;
  yyjson_val* entry;
  yyjson_arr_foreach(data, idx, max, entry) {
    const std::string id = jsonu::getString(entry, "id");
    if (!id.empty()) models.push_back(id);
  }
  return models;
}

std::string describeLlmErrorResponse(int status, const std::string& body) {
  std::string message;
  const auto doc = jsonu::Doc::parse(body);
  if (doc && doc->root()) {
    yyjson_val* error = jsonu::get(doc->root(), "error");
    if (error) {
      message = jsonu::getString(error, "message");
      if (message.empty()) message = jsonu::getString(error, "type");
    }
    if (message.empty()) message = jsonu::getString(doc->root(), "message");
  }
  if (message.empty() && !body.empty()) {
    // 純文字 body 截短附上，至少留個線索
    message = body.substr(0, 200);
  }
  std::string out = "LLM request failed (HTTP " + std::to_string(status) + ")";
  if (!message.empty()) out += ": " + message;
  return out;
}

bool llmErrorLooksTransient(const std::string& error) {
  // describeLlmErrorResponse 的格式固定是 "(HTTP <status>)"，從那裡撈狀態碼
  const size_t at = error.find("(HTTP ");
  if (at != std::string::npos) {
    const int status = std::atoi(error.c_str() + at + 6);
    if (status >= 500 && status <= 599) return true;  // 伺服器端暫時掛掉／載入中
    if (status == 429) return true;                   // rate limit，等等就好
    return false;                                     // 其他 4xx：設定錯了，重試一百次也一樣
  }
  // 沒有狀態碼＝連線層失敗（"LLM request failed: <transportError>"）：
  // 服務正在啟動、網路抖一下都是常態，值得再試
  return true;
}

}  // namespace l2m
