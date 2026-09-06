#include "tts_http.h"

#include <yyjson.h>

#include <cstdio>
#include <map>

#include "config_patch.h"
#include "json_doc.h"
#include "string_util.h"

namespace l2m {

namespace {

// 兩碼語言碼對應到系統匣看得懂的地區字串（VOICE_LOCALES 用連字號形式）
const std::map<std::string, std::string>& langToLocale() {
  static const std::map<std::string, std::string> table{{"zh", "zh-CN"}, {"yue", "zh-HK"}, {"ja", "ja-JP"}, {"ko", "ko-KR"}, {"en", "en-US"}};
  return table;
}

// 只有 unreserved 字元原樣保留，其餘一律百分比編碼（即 encodeURIComponent 的規則）
std::string urlEncode(const std::string& raw) {
  static const char* kHex = "0123456789ABCDEF";
  std::string out;
  out.reserve(raw.size() * 3);
  for (const unsigned char c : raw) {
    const bool unreserved =
      (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '!' || c == '~' || c == '*' || c == '\'' || c == '(' || c == ')';
    if (unreserved) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += kHex[c >> 4];
      out += kHex[c & 0x0F];
    }
  }
  return out;
}

// 自訂語音樣板裡代表「這裡放要唸的句子」的標記
constexpr const char* kTextToken = "${TEXT}";

void addStr(yyjson_mut_doc* doc, yyjson_mut_val* obj, const char* key, const std::string& value) { yyjson_mut_obj_add_strcpy(doc, obj, key, value.c_str()); }

}  // namespace

std::string normalizeBaseUrl(const std::string& url) {
  std::string trimmed = strutil::trim(url);
  while (!trimmed.empty() && trimmed.back() == '/') trimmed.pop_back();
  return trimmed;
}

std::string localeToLangCode(const std::string& locale) {
  const std::string lower = strutil::toLowerAscii(strutil::trim(locale));
  const auto dash = lower.find('-');
  return dash == std::string::npos ? lower : lower.substr(0, dash);
}

std::string langCodeToLocale(const std::string& code) {
  const std::string trimmed = strutil::trim(code);
  if (trimmed.empty() || trimmed.find('-') != std::string::npos) return trimmed;
  const auto it = langToLocale().find(strutil::toLowerAscii(trimmed));
  return it != langToLocale().end() ? it->second : trimmed;
}

std::string describeErrorResponse(int status, const std::string& body) {
  std::string message = strutil::trim(body);
  if (!message.empty() && message.front() == '{') {
    if (auto doc = jsonu::Doc::parse(message)) {
      const std::string detailed = jsonu::getString(doc->root(), "message", jsonu::getString(doc->root(), "detail", ""));
      if (!detailed.empty()) message = detailed;
    }
    // 不是合法 JSON 就原樣用
  }
  const std::string prefix = "HTTP " + std::to_string(status);
  return message.empty() ? prefix : prefix + " - " + message;
}

std::string gptSovitsMime(const std::string& mediaType) {
  if (mediaType == "ogg") return "audio/ogg";
  if (mediaType == "aac") return "audio/aac";
  return "audio/wav";
}

std::string buildGptSovitsBody(const GptSovitsConfig& config, const GptSovitsPreset& preset, const std::string& text, double rate) {
  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_mut_obj(d);
  doc.setRoot(root);

  addStr(d, root, "text", text);
  addStr(d, root, "text_lang", config.textLang);
  addStr(d, root, "ref_audio_path", preset.refAudioPath);
  addStr(d, root, "prompt_text", preset.promptText);
  addStr(d, root, "prompt_lang", preset.promptLang);
  addStr(d, root, "media_type", config.mediaType);
  // 一次拿完整音訊：口型同步要整段波形，串流回來反而要自己拼
  yyjson_mut_obj_add_bool(d, root, "streaming_mode", false);
  yyjson_mut_obj_add_bool(d, root, "parallel_infer", true);
  // 語速原生對應，不必自己變速
  yyjson_mut_obj_add_real(d, root, "speed_factor", rate);
  yyjson_mut_obj_add_int(d, root, "top_k", config.topK);
  yyjson_mut_obj_add_real(d, root, "top_p", config.topP);
  yyjson_mut_obj_add_real(d, root, "temperature", config.temperature);
  addStr(d, root, "text_split_method", config.textSplitMethod);
  yyjson_mut_obj_add_int(d, root, "batch_size", config.batchSize);

  return doc.write(false);
}

std::string gptSovitsWeightUrl(const std::string& base, const std::string& path, const std::string& weights) { return base + "/" + path + "?weights_path=" + urlEncode(weights); }

std::string buildVoiceboxBody(const VoiceboxConfig& config, const std::string& profileId, const std::string& text, const std::string& language) {
  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_mut_obj(d);
  doc.setRoot(root);

  addStr(d, root, "profile_id", profileId);
  addStr(d, root, "text", text);
  addStr(d, root, "language", language);
  // 留空時整個欄位不送，讓 profile 自己的預設生效
  if (config.engine.has_value() && !config.engine->empty()) {
    addStr(d, root, "engine", *config.engine);
  }
  if (config.modelSize.has_value() && !config.modelSize->empty()) {
    addStr(d, root, "model_size", *config.modelSize);
  }
  // Voicebox 的 GenerationRequest 沒有語速欄位，所以刻意不送 rate

  return doc.write(false);
}

std::vector<VoiceInfo> parseVoiceboxProfiles(const std::string& json, const std::string& engineId) {
  std::vector<VoiceInfo> voices;
  auto doc = jsonu::Doc::parse(json);
  if (!doc || !doc->root() || !yyjson_is_arr(doc->root())) return voices;

  yyjson_arr_iter iter;
  yyjson_arr_iter_init(doc->root(), &iter);
  yyjson_val* item = nullptr;
  while ((item = yyjson_arr_iter_next(&iter))) {
    if (!yyjson_is_obj(item)) continue;
    VoiceInfo voice;
    voice.id = jsonu::getString(item, "id");
    voice.name = jsonu::getString(item, "name");
    voice.locale = langCodeToLocale(jsonu::getString(item, "language"));
    voice.engine = engineId;
    if (voice.id.empty()) continue;
    voices.push_back(std::move(voice));
  }
  return voices;
}

std::vector<VoiceInfo> gptSovitsVoices(const GptSovitsConfig& config, const std::string& engineId) {
  std::vector<VoiceInfo> voices;
  voices.reserve(config.presets.size());
  for (const auto& preset : config.presets) {
    VoiceInfo voice;
    voice.id = preset.id;
    voice.name = preset.name;
    voice.locale = preset.locale;
    voice.engine = engineId;
    voices.push_back(std::move(voice));
  }
  return voices;
}

CustomTtsMethod customTtsMethod(const std::string& value) {
  if (value == "post") return CustomTtsMethod::PostForm;
  if (value == "post-json") return CustomTtsMethod::PostJson;
  return CustomTtsMethod::Get;
}

std::string expandTextPlaceholder(const std::string& tpl, const std::string& text, TemplateEncoding encoding) {
  // JSON 的逃逸借 config_patch 的 jsonEscape，不要在這裡再寫第二份
  const std::string encoded = encoding == TemplateEncoding::Percent ? urlEncode(text) : jsonEscape(text);
  const std::string token = kTextToken;
  std::string out;
  out.reserve(tpl.size() + encoded.size());
  size_t pos = 0;
  for (;;) {
    const size_t hit = tpl.find(token, pos);
    if (hit == std::string::npos) {
      out.append(tpl, pos, std::string::npos);
      return out;
    }
    out.append(tpl, pos, hit - pos);
    out += encoded;
    pos = hit + token.size();
  }
}

std::vector<std::pair<std::string, std::string>> parseHeaderLines(const std::string& text) {
  std::vector<std::pair<std::string, std::string>> headers;
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    const std::string line = strutil::trim(text.substr(pos, end - pos));
    pos = end + 1;
    if (line.empty() || line.front() == '#') continue;
    const size_t colon = line.find(':');
    if (colon == std::string::npos) continue;
    const std::string name = strutil::trim(line.substr(0, colon));
    if (name.empty()) continue;
    headers.emplace_back(name, strutil::trim(line.substr(colon + 1)));
  }
  return headers;
}

CustomTtsRequest buildCustomTtsRequest(const CustomTtsConfig& config, const std::string& text) {
  CustomTtsRequest out;
  const std::string url = strutil::trim(config.url);
  const std::string params = strutil::trim(config.params);
  const std::string token = kTextToken;

  if (url.empty()) {
    out.error = "Custom voice URL is not set - fill it in under Settings > Voice > Custom Voice";
    return out;
  }
  // 沒有 ${TEXT} 的話端點永遠只會唸到空字串，而且不會有任何錯誤 —— 一定要擋在送出前
  if (url.find(token) == std::string::npos && params.find(token) == std::string::npos) {
    out.error =
      "Custom voice params must contain ${TEXT} - that is where the sentence gets "
      "inserted. Example: text=${TEXT}&speaker=1";
    return out;
  }

  // URL 一律百分比編碼，**即使方法是 post-json** —— path/query 裡的 ${TEXT}
  // 是網址的一部分，跟 JSON 主體的逃逸規則無關
  out.url = expandTextPlaceholder(url, text, TemplateEncoding::Percent);
  out.headers = parseHeaderLines(config.headers);

  switch (customTtsMethod(config.method)) {
    case CustomTtsMethod::Get:
      if (!params.empty()) {
        // 樣板自己可能已經帶了 query（urlEncode 會把插入文字裡的 ? 編掉，不會誤判）
        out.url += out.url.find('?') == std::string::npos ? "?" : "&";
        out.url += expandTextPlaceholder(params, text, TemplateEncoding::Percent);
      }
      break;
    case CustomTtsMethod::PostForm:
      out.body = expandTextPlaceholder(params, text, TemplateEncoding::Percent);
      out.contentType = "application/x-www-form-urlencoded";
      break;
    case CustomTtsMethod::PostJson:
      out.body = expandTextPlaceholder(params, text, TemplateEncoding::JsonString);
      out.contentType = "application/json";
      break;
  }
  return out;
}

std::string customTtsAudioError(const std::string& contentType, size_t bodySize) {
  if (bodySize == 0) {
    return "Custom voice endpoint returned an empty body - it must return the audio bytes "
           "directly (wav, mp3 or flac)";
  }
  const std::string type = strutil::toLowerAscii(contentType);
  if (type.find("json") != std::string::npos || type.rfind("text/", 0) == 0 || type.rfind("application/xml", 0) == 0) {
    return "Custom voice endpoint returned text, not audio (Content-Type: " + contentType + ") - this app only accepts a raw wav, mp3 or flac body, not JSON wrapping it";
  }
  for (const char* bad : {"ogg", "opus", "aac", "webm", "mp4"}) {
    if (type.find(bad) != std::string::npos) {
      return "Custom voice endpoint returned " + std::string(bad) + " audio, which this app cannot decode - ask the endpoint for wav, mp3 or flac";
    }
  }
  return "";
}

}  // namespace l2m
