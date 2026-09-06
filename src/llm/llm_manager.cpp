#include "llm_manager.h"

#include <memory>

#include "core/config_patch.h"
#include "core/llm_http.h"

namespace l2m {

LlmManager::LlmManager(HttpJson& http, std::function<LlmConfig()> getConfig) : getConfig_(std::move(getConfig)), engine_(http, getConfig_) {}

bool LlmManager::ready() const {
  const LlmConfig config = getConfig_ ? getConfig_() : LlmConfig{};
  return config.enabled && llmConfigIssue(config).empty();
}

std::unique_ptr<LlmRequestHandle> LlmManager::chat(const std::vector<LlmMessage>& messages, const LlmChatOptions& options, LlmStreamSink sink) {
  // 包一層 onError 記下最近的失敗原因（get_state 的診斷欄位），其餘直通
  auto original = std::move(sink.onError);
  sink.onError = [this, original](std::string error) {
    lastError_ = error;
    if (original) original(std::move(error));
  };
  return engine_.chat(messages, options, std::move(sink));
}

std::unique_ptr<LlmRequestHandle> LlmManager::chatBuffered(const std::vector<LlmMessage>& messages, const LlmChatOptions& options, std::function<void(std::string, std::string)> done) {
  auto text = std::make_shared<std::string>();
  LlmStreamSink sink;
  sink.onTextDelta = [text](const std::string& delta) { *text += delta; };
  sink.onDone = [text, done](std::string) {
    if (done) done(std::move(*text), "");
  };
  sink.onError = [done](std::string error) {
    if (done) done("", std::move(error));
  };
  return chat(messages, options, std::move(sink));
}

void LlmManager::listModels(std::function<void(std::vector<std::string>, std::string)> done) { engine_.listModels(std::move(done)); }

std::string LlmManager::stateJson() const {
  const LlmConfig config = getConfig_ ? getConfig_() : LlmConfig{};
  JsonObject state;
  state.boolean("enabled", config.enabled).text("provider", config.provider);
  state.text("model", config.provider == "anthropic" ? config.anthropicModel : config.model);
  state.boolean("driveIdle", config.driveIdle);
  const std::string issue = llmConfigIssue(config);
  if (issue.empty()) {
    state.nullValue("configIssue");
  } else {
    state.text("configIssue", issue);
  }
  if (lastError_.empty()) {
    state.nullValue("lastError");
  } else {
    state.text("lastError", lastError_);
  }
  return state.json();
}

}  // namespace l2m
