// LLM 請求組裝：兩家的欄位、header、system 抽取、tools 轉換、
// 設定不完整的同一句錯誤、模型清單與錯誤回應解析。
#include <QtTest>

#include <string>

#include "core/json_doc.h"
#include "core/llm_http.h"

using namespace l2m;

namespace {

LlmConfig openaiConfig() {
  LlmConfig config;
  config.enabled = true;
  config.provider = "openai";
  config.baseUrl = "http://127.0.0.1:11434/v1/";  // 帶尾斜線，驗 normalize
  config.apiKey = "sk-test";
  config.model = "llama3";
  config.temperature = 0.7;
  config.maxTokens = 256;
  return config;
}

LlmConfig anthropicConfig() {
  LlmConfig config;
  config.enabled = true;
  config.provider = "anthropic";
  config.anthropicApiKey = "sk-ant-test";
  config.anthropicModel = "claude-opus-5";
  return config;
}

std::string headerValue(const LlmHttpRequest& request, const std::string& name) {
  for (const auto& [key, value] : request.headers) {
    if (key == name) return value;
  }
  return {};
}

}  // namespace

class TestLlmHttp : public QObject {
  Q_OBJECT

private slots:
  void chatCompletionsShape() {
    const std::vector<LlmMessage> messages{{"system", "You are a mascot."}, {"user", "hi"}};
    const LlmHttpRequest request = buildChatCompletionsRequest(openaiConfig(), messages, {}, /*stream=*/true);
    QVERIFY(request.error.empty());
    QCOMPARE(request.url, std::string("http://127.0.0.1:11434/v1/chat/completions"));
    QCOMPARE(request.contentType, std::string("application/json"));
    QCOMPARE(headerValue(request, "Authorization"), std::string("Bearer sk-test"));

    const auto doc = jsonu::Doc::parse(request.body);
    QVERIFY(doc.has_value());
    yyjson_val* root = doc->root();
    QCOMPARE(jsonu::getString(root, "model"), std::string("llama3"));
    QCOMPARE(yyjson_get_real(jsonu::get(root, "temperature")), 0.7);
    QCOMPARE(yyjson_get_int(jsonu::get(root, "max_tokens")), 256);
    QCOMPARE(yyjson_get_bool(jsonu::get(root, "stream")), true);
    yyjson_val* msgs = jsonu::get(root, "messages");
    QCOMPARE(yyjson_arr_size(msgs), size_t(2));
    QCOMPARE(jsonu::getString(yyjson_arr_get(msgs, 0), "role"), std::string("system"));
    QCOMPARE(jsonu::getString(yyjson_arr_get(msgs, 1), "content"), std::string("hi"));
    // 沒帶工具就不該有 tools 欄位
    QVERIFY(jsonu::get(root, "tools") == nullptr);
  }

  // local 服務沒填金鑰：不送 Authorization；非串流：不送 stream
  void chatCompletionsWithoutKeyOrStream() {
    LlmConfig config = openaiConfig();
    config.apiKey = "";
    const LlmHttpRequest request = buildChatCompletionsRequest(config, {{"user", "hi"}}, {}, /*stream=*/false);
    QVERIFY(request.error.empty());
    QCOMPARE(headerValue(request, "Authorization"), std::string());
    const auto doc = jsonu::Doc::parse(request.body);
    QVERIFY(jsonu::get(doc->root(), "stream") == nullptr);
  }

  // options 蓋過 config
  void chatOptionsOverrideConfig() {
    LlmChatOptions options;
    options.temperature = 0.1;
    options.maxTokens = 64;
    const LlmHttpRequest request = buildChatCompletionsRequest(openaiConfig(), {{"user", "hi"}}, options, true);
    const auto doc = jsonu::Doc::parse(request.body);
    QCOMPARE(yyjson_get_real(jsonu::get(doc->root(), "temperature")), 0.1);
    QCOMPARE(yyjson_get_int(jsonu::get(doc->root(), "max_tokens")), 64);
  }

  void anthropicShape() {
    const std::vector<LlmMessage> messages{{"system", "Persona A."}, {"system", "Rule B."}, {"user", "hi"}};
    const LlmHttpRequest request = buildAnthropicMessagesRequest(anthropicConfig(), messages, {}, /*stream=*/true);
    QVERIFY(request.error.empty());
    QCOMPARE(request.url, std::string(kAnthropicMessagesUrl));
    QCOMPARE(headerValue(request, "x-api-key"), std::string("sk-ant-test"));
    QCOMPARE(headerValue(request, "anthropic-version"), std::string(kAnthropicVersion));

    const auto doc = jsonu::Doc::parse(request.body);
    yyjson_val* root = doc->root();
    QCOMPARE(jsonu::getString(root, "model"), std::string("claude-opus-5"));
    // 兩則 system 抽成頂層欄位、以空行串接；messages 裡不再有 system
    QCOMPARE(jsonu::getString(root, "system"), std::string("Persona A.\n\nRule B."));
    yyjson_val* msgs = jsonu::get(root, "messages");
    QCOMPARE(yyjson_arr_size(msgs), size_t(1));
    QCOMPARE(jsonu::getString(yyjson_arr_get(msgs, 0), "role"), std::string("user"));
  }

  // OpenAI tools 形狀 → Anthropic input_schema 形狀
  void anthropicToolConversion() {
    LlmChatOptions options;
    options.toolsJson = R"([{"type":"function","function":{"name":"speak","description":"Say a line",)"
                        R"("parameters":{"type":"object","properties":{"text":{"type":"string"}}}}}])";
    const LlmHttpRequest request = buildAnthropicMessagesRequest(anthropicConfig(), {{"user", "hi"}}, options, true);
    const auto doc = jsonu::Doc::parse(request.body);
    yyjson_val* tools = jsonu::get(doc->root(), "tools");
    QCOMPARE(yyjson_arr_size(tools), size_t(1));
    yyjson_val* tool = yyjson_arr_get(tools, 0);
    QCOMPARE(jsonu::getString(tool, "name"), std::string("speak"));
    QCOMPARE(jsonu::getString(tool, "description"), std::string("Say a line"));
    QVERIFY(jsonu::get(tool, "input_schema") != nullptr);
    QVERIFY(jsonu::get(tool, "function") == nullptr);
  }

  // 設定不完整：紅字與執行失敗共用同一句（llmConfigIssue）
  void incompleteConfigSharesOneMessage() {
    LlmConfig config = openaiConfig();
    config.model = "";
    const std::string issue = llmConfigIssue(config);
    QVERIFY(!issue.empty());
    const LlmHttpRequest request = buildChatCompletionsRequest(config, {{"user", "hi"}}, {}, true);
    QCOMPARE(request.error, issue);

    LlmConfig anthropic = anthropicConfig();
    anthropic.anthropicApiKey = "  ";
    QVERIFY(!llmConfigIssue(anthropic).empty());
    QCOMPARE(buildAnthropicMessagesRequest(anthropic, {}, {}, true).error, llmConfigIssue(anthropic));
  }

  void dispatchByProvider() {
    QVERIFY(buildLlmChatRequest(openaiConfig(), {{"user", "x"}}, {}, true).url.find("/chat/completions") != std::string::npos);
    QCOMPARE(buildLlmChatRequest(anthropicConfig(), {{"user", "x"}}, {}, true).url, std::string(kAnthropicMessagesUrl));
  }

  void listModelsRequests() {
    const LlmHttpRequest openai = buildListModelsRequest(openaiConfig());
    QCOMPARE(openai.url, std::string("http://127.0.0.1:11434/v1/models"));
    QCOMPARE(headerValue(openai, "Authorization"), std::string("Bearer sk-test"));

    const LlmHttpRequest anthropic = buildListModelsRequest(anthropicConfig());
    QCOMPARE(anthropic.url, std::string(kAnthropicModelsUrl));
    QCOMPARE(headerValue(anthropic, "x-api-key"), std::string("sk-ant-test"));

    LlmConfig broken = openaiConfig();
    broken.baseUrl = "  ";
    QVERIFY(!buildListModelsRequest(broken).error.empty());
  }

  // 兩家的清單恰好同形：{"data":[{"id":…}]}
  void parsesModelList() {
    const auto models = parseModelListJson(R"({"object":"list","data":[{"id":"llama3"},{"id":"qwen2"},{"name":"no-id"}]})");
    QCOMPARE(models.size(), size_t(2));
    QCOMPARE(models[0], std::string("llama3"));
    QCOMPARE(models[1], std::string("qwen2"));
    QCOMPARE(parseModelListJson("garbage").size(), size_t(0));
  }

  // 暫時性判定：5xx／429／連線層算，其他 4xx 不算 —— Ollama 冷載入模型
  // 回的 500 "llm server loading model" 是這條規則存在的理由
  void transientErrorsAreRecognized() {
    QVERIFY(llmErrorLooksTransient(describeLlmErrorResponse(500, R"({"error":{"message":"unexpected server status: llm server loading model"}})")));
    QVERIFY(llmErrorLooksTransient(describeLlmErrorResponse(503, "overloaded")));
    QVERIFY(llmErrorLooksTransient(describeLlmErrorResponse(429, "slow down")));
    QVERIFY(llmErrorLooksTransient("LLM request failed: Connection refused"));
    QVERIFY(!llmErrorLooksTransient(describeLlmErrorResponse(401, R"({"error":{"message":"bad key"}})")));
    QVERIFY(!llmErrorLooksTransient(describeLlmErrorResponse(404, "no such model")));
  }

  void describesErrorResponses() {
    const std::string json = describeLlmErrorResponse(401, R"({"error":{"message":"bad key"}})");
    QVERIFY(json.find("401") != std::string::npos);
    QVERIFY(json.find("bad key") != std::string::npos);
    // 純文字 body 截短附上
    const std::string text = describeLlmErrorResponse(502, "Bad Gateway");
    QVERIFY(text.find("Bad Gateway") != std::string::npos);
  }
};

QTEST_GUILESS_MAIN(TestLlmHttp)
#include "test_llm_http.moc"
