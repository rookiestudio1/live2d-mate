#include "config_patch.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <locale>
#include <sstream>

namespace l2m {

namespace {

// 先用 15 位試，能原樣讀回來就用它 —— 直接上 17 位會把 0.1 印成
// 0.10000000000000001，設定檔變得沒法讀。
constexpr int kShortPrecision = 15;
constexpr int kExactPrecision = 17;

std::string formatDouble(double value, int precision) {
  std::ostringstream out;
  // 不吃全域 locale：小數點被換成逗號就直接產出無效 JSON
  out.imbue(std::locale::classic());
  out << std::setprecision(precision) << value;
  return out.str();
}

}  // namespace

std::string jsonEscape(const std::string& value) {
  std::string out;
  out.reserve(value.size());
  for (const char ch : value) {
    const unsigned char c = static_cast<unsigned char>(ch);
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\b':
        out += "\\b";
        break;
      case '\f':
        out += "\\f";
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
          // 其餘控制字元走 \u00XX；0x80 以上是 UTF-8 續位元組，原樣保留
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out.push_back(ch);
        }
        break;
    }
  }
  return out;
}

std::string jsonString(const std::string& value) { return '"' + jsonEscape(value) + '"'; }

std::string jsonNumber(double value) {
  // 整數值不印小數點，讓設定檔好讀
  //（與 config_schema.cpp 的 putNum 同一條規則）
  if (value == std::floor(value) && std::abs(value) < 9.0e15) {
    return std::to_string(static_cast<long long>(value));
  }
  const std::string shortForm = formatDouble(value, kShortPrecision);
  if (std::strtod(shortForm.c_str(), nullptr) == value) return shortForm;
  return formatDouble(value, kExactPrecision);
}

std::string jsonBool(bool value) { return value ? "true" : "false"; }

JsonObject& JsonObject::boolean(const std::string& field, bool value) {
  fields_.emplace_back(field, jsonBool(value));
  return *this;
}

JsonObject& JsonObject::number(const std::string& field, double value) {
  fields_.emplace_back(field, jsonNumber(value));
  return *this;
}

JsonObject& JsonObject::text(const std::string& field, const std::string& value) {
  fields_.emplace_back(field, jsonString(value));
  return *this;
}

JsonObject& JsonObject::nullValue(const std::string& field) {
  fields_.emplace_back(field, "null");
  return *this;
}

JsonObject& JsonObject::nullableText(const std::string& field, const std::optional<std::string>& value) { return value ? text(field, *value) : nullValue(field); }

JsonObject& JsonObject::object(const std::string& field, const JsonObject& value) {
  fields_.emplace_back(field, value.json());
  return *this;
}

JsonObject& JsonObject::array(const std::string& field, const std::vector<JsonObject>& items) {
  std::string out = "[";
  bool first = true;
  for (const auto& item : items) {
    if (!first) out.push_back(',');
    first = false;
    out += item.json();
  }
  out.push_back(']');
  fields_.emplace_back(field, out);
  return *this;
}

std::string JsonObject::json() const {
  std::string out = "{";
  bool first = true;
  for (const auto& entry : fields_) {
    if (!first) out.push_back(',');
    first = false;
    out += jsonString(entry.first);
    out.push_back(':');
    out += entry.second;
  }
  out.push_back('}');
  return out;
}

ConfigPatch& ConfigPatch::boolean(const std::string& field, bool value) {
  fields_.boolean(field, value);
  return *this;
}

ConfigPatch& ConfigPatch::number(const std::string& field, double value) {
  fields_.number(field, value);
  return *this;
}

ConfigPatch& ConfigPatch::text(const std::string& field, const std::string& value) {
  fields_.text(field, value);
  return *this;
}

ConfigPatch& ConfigPatch::nullValue(const std::string& field) {
  fields_.nullValue(field);
  return *this;
}

ConfigPatch& ConfigPatch::nullableText(const std::string& field, const std::optional<std::string>& value) {
  fields_.nullableText(field, value);
  return *this;
}

ConfigPatch& ConfigPatch::object(const std::string& field, const JsonObject& value) {
  fields_.object(field, value);
  return *this;
}

std::string ConfigPatch::json() const { return "{" + jsonString(section_) + ":" + fields_.json() + "}"; }

std::string boolPatch(const std::string& section, const std::string& field, bool value) { return ConfigPatch(section).boolean(field, value).json(); }

std::string numberPatch(const std::string& section, const std::string& field, double value) { return ConfigPatch(section).number(field, value).json(); }

std::string stringPatch(const std::string& section, const std::string& field, const std::string& value) { return ConfigPatch(section).text(field, value).json(); }

std::string nullPatch(const std::string& section, const std::string& field) { return ConfigPatch(section).nullValue(field).json(); }

std::string ttsEnginePatch(const std::string& engineId) { return ConfigPatch("tts").text("engine", engineId).nullValue("voice").json(); }

std::string ttsCustomPatch(const CustomTtsConfig& config) {
  // 欄位順序與 config_schema.cpp 的 serialize 一致，比對 patch 字串的測試才好讀
  JsonObject custom;
  custom.text("url", config.url).text("method", config.method).text("params", config.params).text("headers", config.headers).number("timeoutMs", config.timeoutMs);
  return ConfigPatch("tts").object("custom", custom).json();
}

bool customTtsFieldsEqual(const CustomTtsConfig& a, const CustomTtsConfig& b) {
  // 刻意逐欄列出而不是比整個結構：timeoutMs 沒有 UI，算進來的話「套用」按鈕
  // 會被一個使用者永遠改不到的欄位卡住
  return a.url == b.url && a.method == b.method && a.params == b.params && a.headers == b.headers;
}

std::string ttsLocalServerPatch(const TtsConfig& config) {
  // 欄位順序與 config_schema.cpp 的 serialize 一致，比對 patch 字串的測試才好讀
  const GptSovitsConfig& g = config.gptsovits;
  std::vector<JsonObject> presets;
  presets.reserve(g.presets.size());
  for (const auto& p : g.presets) {
    JsonObject preset;
    preset.text("id", p.id)
      .text("name", p.name)
      .text("refAudioPath", p.refAudioPath)
      .text("promptText", p.promptText)
      .text("promptLang", p.promptLang)
      .text("locale", p.locale)
      .nullableText("gptWeights", p.gptWeights)
      .nullableText("sovitsWeights", p.sovitsWeights);
    presets.push_back(std::move(preset));
  }

  JsonObject gptsovits;
  gptsovits.text("baseUrl", g.baseUrl)
    .text("textLang", g.textLang)
    .text("mediaType", g.mediaType)
    .number("timeoutMs", g.timeoutMs)
    .array("presets", presets)
    .number("topK", g.topK)
    .number("topP", g.topP)
    .number("temperature", g.temperature)
    .text("textSplitMethod", g.textSplitMethod)
    .number("batchSize", g.batchSize);

  const VoiceboxConfig& v = config.voicebox;
  JsonObject voicebox;
  voicebox.text("baseUrl", v.baseUrl).nullableText("language", v.language).nullableText("engine", v.engine).nullableText("modelSize", v.modelSize).number("timeoutMs", v.timeoutMs);

  return ConfigPatch("tts").object("gptsovits", gptsovits).object("voicebox", voicebox).json();
}

bool ttsLocalServerFieldsEqual(const TtsConfig& a, const TtsConfig& b) {
  // 只比兩個有輸入框的欄位。其餘（presets、timeoutMs、取樣參數…）沒有 UI，
  // 算進來只會讓「套用」按鈕被使用者永遠改不到的欄位卡住
  return a.gptsovits.baseUrl == b.gptsovits.baseUrl && a.voicebox.baseUrl == b.voicebox.baseUrl;
}

std::string mcpPatch(bool enabled, const std::string& host, int port, const std::optional<std::string>& token) {
  return ConfigPatch("mcp").boolean("enabled", enabled).text("host", host).number("port", port).nullableText("token", token).json();
}

std::string llmPatch(const LlmConfig& config) {
  // 欄位順序與 config_schema.cpp 的 serialize 一致，比對 patch 字串的測試才好讀
  return ConfigPatch("llm")
    .boolean("enabled", config.enabled)
    .text("provider", config.provider)
    .text("baseUrl", config.baseUrl)
    .text("apiKey", config.apiKey)
    .text("model", config.model)
    .text("anthropicApiKey", config.anthropicApiKey)
    .text("anthropicModel", config.anthropicModel)
    .number("temperature", config.temperature)
    .number("maxTokens", config.maxTokens)
    .number("timeoutMs", config.timeoutMs)
    .boolean("driveIdle", config.driveIdle)
    .number("idleLlmCooldownMs", config.idleLlmCooldownMs)
    .json();
}

std::string weatherLocationPatch(double latitude, double longitude, const std::string& name) {
  // 欄位順序與 config_schema.cpp 的 serialize 一致，比對 patch 字串的測試才好讀
  return ConfigPatch("weather").number("latitude", latitude).number("longitude", longitude).text("locationName", name).json();
}

bool llmFieldsEqual(const LlmConfig& a, const LlmConfig& b) {
  // 只比套用按鈕管的欄位（見標頭）；timeoutMs 沒有 UI，同 customTtsFieldsEqual 的取捨
  return a.provider == b.provider && a.baseUrl == b.baseUrl && a.apiKey == b.apiKey && a.model == b.model && a.anthropicApiKey == b.anthropicApiKey && a.anthropicModel == b.anthropicModel;
}

}  // namespace l2m
