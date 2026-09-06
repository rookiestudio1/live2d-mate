#include "config_schema.h"

#include <algorithm>
#include <cmath>

#include "bubble_shape.h"

namespace l2m {

namespace {

// ── 驗證輔助 ───────────────────────────────────────────────
// 驗證語意：欄位缺席 → 保留預設；存在但型別錯／超出範圍 → 記 issue。

struct Ctx {
  std::vector<std::string>* issues;
  bool ok = true;

  void fail(const std::string& path, const std::string& message) {
    ok = false;
    if (issues) issues->push_back(path + ": " + message);
  }
};

bool isNum(yyjson_val* v) { return v && (yyjson_is_int(v) || yyjson_is_real(v)); }
double numOf(yyjson_val* v) { return yyjson_get_num(v); }

void readBool(Ctx& ctx, yyjson_val* obj, const char* key, bool& out, const std::string& path) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!v) return;
  if (!yyjson_is_bool(v)) return ctx.fail(path, "Expected boolean");
  out = yyjson_get_bool(v);
}

void readNum(Ctx& ctx, yyjson_val* obj, const char* key, double& out, double lo, double hi, const std::string& path) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!v) return;
  if (!isNum(v)) return ctx.fail(path, "Expected number");
  const double n = numOf(v);
  if (n < lo || n > hi) return ctx.fail(path, "Out of range");
  out = n;
}

void readInt(Ctx& ctx, yyjson_val* obj, const char* key, int& out, double lo, double hi, const std::string& path) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!v) return;
  if (!isNum(v)) return ctx.fail(path, "Expected number");
  const double n = numOf(v);
  if (n != std::floor(n)) return ctx.fail(path, "Expected integer");
  if (n < lo || n > hi) return ctx.fail(path, "Out of range");
  out = static_cast<int>(n);
}

void readStr(Ctx& ctx, yyjson_val* obj, const char* key, std::string& out, size_t minLen, const std::string& path) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!v) return;
  if (!yyjson_is_str(v)) return ctx.fail(path, "Expected string");
  const std::string s = yyjson_get_str(v);
  if (s.size() < minLen) return ctx.fail(path, "Too short");
  out = s;
}

void readNullableStr(Ctx& ctx, yyjson_val* obj, const char* key, std::optional<std::string>& out, const std::string& path) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!v) return;
  if (yyjson_is_null(v)) {
    out = std::nullopt;
    return;
  }
  if (!yyjson_is_str(v)) return ctx.fail(path, "Expected string or null");
  out = std::string(yyjson_get_str(v));
}

void readNullableNum(Ctx& ctx, yyjson_val* obj, const char* key, std::optional<double>& out, const std::string& path) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!v) return;
  if (yyjson_is_null(v)) {
    out = std::nullopt;
    return;
  }
  if (!isNum(v)) return ctx.fail(path, "Expected number or null");
  out = numOf(v);
}

void readEnum(Ctx& ctx, yyjson_val* obj, const char* key, std::string& out, const std::vector<std::string>& allowed, const std::string& path) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!v) return;
  if (!yyjson_is_str(v)) return ctx.fail(path, "Expected string");
  const std::string s = yyjson_get_str(v);
  if (std::find(allowed.begin(), allowed.end(), s) == allowed.end()) return ctx.fail(path, "Invalid enum value");
  out = s;
}

// 區塊：缺席 → 用預設；存在但不是物件 → 失敗
yyjson_val* section(Ctx& ctx, yyjson_val* root, const char* key) {
  yyjson_val* v = jsonu::get(root, key);
  if (!v) return nullptr;
  if (!yyjson_is_obj(v)) {
    ctx.fail(key, "Expected object");
    return nullptr;
  }
  return v;
}

// ── 序列化輔助 ──────────────────────────────────────────────

yyjson_mut_val* keyOf(jsonu::MutDoc& doc, const char* text) { return yyjson_mut_strcpy(doc.get(), text); }

// 整數值不印小數點，讓設定檔好讀（config_patch.cpp 的 jsonNumber 是同一條規則）
void putNum(jsonu::MutDoc& doc, yyjson_mut_val* obj, const char* key, double value) {
  if (value == std::floor(value) && std::abs(value) < 9.0e15) {
    yyjson_mut_obj_put(obj, keyOf(doc, key), yyjson_mut_sint(doc.get(), static_cast<int64_t>(value)));
  } else {
    yyjson_mut_obj_put(obj, keyOf(doc, key), yyjson_mut_real(doc.get(), value));
  }
}

void putBool(jsonu::MutDoc& doc, yyjson_mut_val* obj, const char* key, bool value) { yyjson_mut_obj_put(obj, keyOf(doc, key), yyjson_mut_bool(doc.get(), value)); }

void putStr(jsonu::MutDoc& doc, yyjson_mut_val* obj, const char* key, const std::string& value) { yyjson_mut_obj_put(obj, keyOf(doc, key), yyjson_mut_strcpy(doc.get(), value.c_str())); }

void putNullableStr(jsonu::MutDoc& doc, yyjson_mut_val* obj, const char* key, const std::optional<std::string>& value) {
  if (value)
    putStr(doc, obj, key, *value);
  else
    yyjson_mut_obj_put(obj, keyOf(doc, key), yyjson_mut_null(doc.get()));
}

void putNullableNum(jsonu::MutDoc& doc, yyjson_mut_val* obj, const char* key, const std::optional<double>& value) {
  if (value)
    putNum(doc, obj, key, *value);
  else
    yyjson_mut_obj_put(obj, keyOf(doc, key), yyjson_mut_null(doc.get()));
}

yyjson_mut_val* addSection(jsonu::MutDoc& doc, yyjson_mut_val* root, const char* key) {
  yyjson_mut_val* obj = yyjson_mut_obj(doc.get());
  yyjson_mut_obj_put(root, keyOf(doc, key), obj);
  return obj;
}

std::vector<std::string> mediaTypes() { return {"wav", "ogg", "aac"}; }

std::vector<std::string> customTtsMethods() { return {"get", "post", "post-json"}; }

std::vector<std::string> windDirections() { return {"left", "right"}; }

std::vector<std::string> llmProviders() { return {"openai", "anthropic"}; }

std::vector<std::string> speechModes() { return {"off", "bubble", "voice"}; }

std::vector<std::string> temperatureUnits() { return {"c", "f"}; }

std::vector<std::string> localeSettings() {
  std::vector<std::string> values{"auto"};
  for (const auto& l : supportedLocales()) values.push_back(l);
  return values;
}

}  // namespace

std::optional<AppConfig> parseConfig(yyjson_val* root, std::vector<std::string>* issues) {
  Ctx ctx{issues};
  AppConfig config;

  // 整份是 null／非物件都算不合法（只有 root 根本不存在才走預設）
  if (root && !yyjson_is_obj(root)) {
    ctx.fail("", "Expected object");
    return std::nullopt;
  }
  if (!root) return config;

  if (yyjson_val* model = section(ctx, root, "model")) {
    readNullableStr(ctx, model, "current", config.model.current, "model.current");
    readNum(ctx, model, "scale", config.model.scale, kMinScale, kMaxScale, "model.scale");
    readNum(ctx, model, "opacity", config.model.opacity, 0.1, 1, "model.opacity");
    readNullableNum(ctx, model, "x", config.model.x, "model.x");
    readNullableNum(ctx, model, "y", config.model.y, "model.y");
    readEnum(ctx, model, "preset", config.model.preset, positionPresets(), "model.preset");
  }

  if (yyjson_val* interaction = section(ctx, root, "interaction")) {
    readBool(ctx, interaction, "lookAt", config.interaction.lookAt, "interaction.lookAt");
    readBool(ctx, interaction, "clickThrough", config.interaction.clickThrough, "interaction.clickThrough");
    readBool(ctx, interaction, "dragMove", config.interaction.dragMove, "interaction.dragMove");
    readBool(ctx, interaction, "wheelZoom", config.interaction.wheelZoom, "interaction.wheelZoom");
    readBool(ctx, interaction, "lockPosition", config.interaction.lockPosition, "interaction.lockPosition");
    readBool(ctx, interaction, "tapMotion", config.interaction.tapMotion, "interaction.tapMotion");
    // 後來新增的鍵，追加在區塊最後（見 serializeConfig 的鍵順序合約）
    readBool(ctx, interaction, "dragSwing", config.interaction.dragSwing, "interaction.dragSwing");
    readBool(ctx, interaction, "speakFacingFront", config.interaction.speakFacingFront, "interaction.speakFacingFront");
  }

  if (yyjson_val* tts = section(ctx, root, "tts")) {
    readStr(ctx, tts, "engine", config.tts.engine, 0, "tts.engine");
    readNullableStr(ctx, tts, "voice", config.tts.voice, "tts.voice");
    readNum(ctx, tts, "rate", config.tts.rate, 0.5, 2, "tts.rate");
    // 上限 2：>1 為播放端軟體增益（見 TtsConfig::volume）
    readNum(ctx, tts, "volume", config.tts.volume, 0, 2, "tts.volume");
    readBool(ctx, tts, "showBubble", config.tts.showBubble, "tts.showBubble");
    readNum(ctx, tts, "bubbleMinDuration", config.tts.bubbleMinDuration, 500, 30000, "tts.bubbleMinDuration");
    readInt(ctx, tts, "bubbleOffsetY", config.tts.bubbleOffsetY, kMinBubbleOffsetY, kMaxBubbleOffsetY, "tts.bubbleOffsetY");
    readNum(ctx, tts, "bubbleMaxDelay", config.tts.bubbleMaxDelay, 0, 30000, "tts.bubbleMaxDelay");
    readEnum(ctx, tts, "bubbleShadow", config.tts.bubbleShadow, bubbleShadowIds(), "tts.bubbleShadow");

    if (yyjson_val* gpts = section(ctx, tts, "gptsovits")) {
      auto& g = config.tts.gptsovits;
      readStr(ctx, gpts, "baseUrl", g.baseUrl, 0, "tts.gptsovits.baseUrl");
      readStr(ctx, gpts, "textLang", g.textLang, 0, "tts.gptsovits.textLang");
      readEnum(ctx, gpts, "mediaType", g.mediaType, mediaTypes(), "tts.gptsovits.mediaType");
      readInt(ctx, gpts, "timeoutMs", g.timeoutMs, 5000, 600000, "tts.gptsovits.timeoutMs");
      readInt(ctx, gpts, "topK", g.topK, -1.0e18, 1.0e18, "tts.gptsovits.topK");
      readNum(ctx, gpts, "topP", g.topP, -1.0e18, 1.0e18, "tts.gptsovits.topP");
      readNum(ctx, gpts, "temperature", g.temperature, -1.0e18, 1.0e18, "tts.gptsovits.temperature");
      readStr(ctx, gpts, "textSplitMethod", g.textSplitMethod, 0, "tts.gptsovits.textSplitMethod");
      readInt(ctx, gpts, "batchSize", g.batchSize, -1.0e18, 1.0e18, "tts.gptsovits.batchSize");

      yyjson_val* presets = jsonu::get(gpts, "presets");
      if (presets) {
        if (!yyjson_is_arr(presets)) {
          ctx.fail("tts.gptsovits.presets", "Expected array");
        } else {
          size_t idx, max;
          yyjson_val* p;
          yyjson_arr_foreach(presets, idx, max, p) {
            const std::string path = "tts.gptsovits.presets." + std::to_string(idx);
            if (!yyjson_is_obj(p)) {
              ctx.fail(path, "Expected object");
              continue;
            }
            GptSovitsPreset preset;
            readStr(ctx, p, "id", preset.id, 1, path + ".id");
            if (preset.id.empty()) ctx.fail(path + ".id", "Required");
            readStr(ctx, p, "name", preset.name, 1, path + ".name");
            if (preset.name.empty()) ctx.fail(path + ".name", "Required");
            readStr(ctx, p, "refAudioPath", preset.refAudioPath, 1, path + ".refAudioPath");
            if (preset.refAudioPath.empty()) ctx.fail(path + ".refAudioPath", "Required");
            readStr(ctx, p, "promptText", preset.promptText, 0, path + ".promptText");
            readStr(ctx, p, "promptLang", preset.promptLang, 0, path + ".promptLang");
            readStr(ctx, p, "locale", preset.locale, 0, path + ".locale");
            readNullableStr(ctx, p, "gptWeights", preset.gptWeights, path + ".gptWeights");
            readNullableStr(ctx, p, "sovitsWeights", preset.sovitsWeights, path + ".sovitsWeights");
            g.presets.push_back(std::move(preset));
          }
        }
      }
    }

    if (yyjson_val* vb = section(ctx, tts, "voicebox")) {
      auto& v = config.tts.voicebox;
      readStr(ctx, vb, "baseUrl", v.baseUrl, 0, "tts.voicebox.baseUrl");
      readNullableStr(ctx, vb, "language", v.language, "tts.voicebox.language");
      readNullableStr(ctx, vb, "engine", v.engine, "tts.voicebox.engine");
      readNullableStr(ctx, vb, "modelSize", v.modelSize, "tts.voicebox.modelSize");
      readInt(ctx, vb, "timeoutMs", v.timeoutMs, 5000, 600000, "tts.voicebox.timeoutMs");
    }

    if (yyjson_val* cu = section(ctx, tts, "custom")) {
      auto& c = config.tts.custom;
      // url／params／headers 都不驗格式：端點長什麼樣只有使用者知道，
      // 真正的檢查在 buildCustomTtsRequest（設定頁與合成路徑共用同一句錯誤訊息）。
      readStr(ctx, cu, "url", c.url, 0, "tts.custom.url");
      readEnum(ctx, cu, "method", c.method, customTtsMethods(), "tts.custom.method");
      readStr(ctx, cu, "params", c.params, 0, "tts.custom.params");
      readStr(ctx, cu, "headers", c.headers, 0, "tts.custom.headers");
      readInt(ctx, cu, "timeoutMs", c.timeoutMs, 5000, 600000, "tts.custom.timeoutMs");
    }
  }

  if (yyjson_val* mcp = section(ctx, root, "mcp")) {
    readBool(ctx, mcp, "enabled", config.mcp.enabled, "mcp.enabled");
    readStr(ctx, mcp, "host", config.mcp.host, 1, "mcp.host");
    readInt(ctx, mcp, "port", config.mcp.port, 1024, 65535, "mcp.port");
    readNullableStr(ctx, mcp, "token", config.mcp.token, "mcp.token");
    readBool(ctx, mcp, "talkative", config.mcp.talkative, "mcp.talkative");
    readBool(ctx, mcp, "notifyOnComplete", config.mcp.notifyOnComplete, "mcp.notifyOnComplete");
    readBool(ctx, mcp, "speakNoWait", config.mcp.speakNoWait, "mcp.speakNoWait");
    readBool(ctx, mcp, "announceSteps", config.mcp.announceSteps, "mcp.announceSteps");
  }

  if (yyjson_val* idle = section(ctx, root, "idle")) {
    readBool(ctx, idle, "autoReset", config.idle.autoReset, "idle.autoReset");
    readInt(ctx, idle, "resetMs", config.idle.resetMs, kMinIdleResetMs, kMaxIdleResetMs, "idle.resetMs");
    readBool(ctx, idle, "perform", config.idle.perform, "idle.perform");
    readInt(ctx, idle, "performAfterMs", config.idle.performAfterMs, kMinIdlePerformAfterMs, kMaxIdlePerformAfterMs, "idle.performAfterMs");
    readInt(ctx, idle, "performIntervalMs", config.idle.performIntervalMs, kMinIdlePerformIntervalMs, kMaxIdlePerformIntervalMs, "idle.performIntervalMs");
  }

  if (yyjson_val* app = section(ctx, root, "app")) {
    readBool(ctx, app, "openAtLogin", config.app.openAtLogin, "app.openAtLogin");
    readBool(ctx, app, "alwaysOnTop", config.app.alwaysOnTop, "app.alwaysOnTop");
    readBool(ctx, app, "disableHardwareAcceleration", config.app.disableHardwareAcceleration, "app.disableHardwareAcceleration");
    readEnum(ctx, app, "locale", config.app.locale, localeSettings(), "app.locale");
    readBool(ctx, app, "sampleModelsPrompted", config.app.sampleModelsPrompted, "app.sampleModelsPrompted");
  }

  if (yyjson_val* persona = section(ctx, root, "persona")) {
    readNullableStr(ctx, persona, "current", config.persona.current, "persona.current");
  }

  if (yyjson_val* autonomy = section(ctx, root, "autonomy")) {
    readBool(ctx, autonomy, "enabled", config.autonomy.enabled, "autonomy.enabled");
    readBool(ctx, autonomy, "expression", config.autonomy.expression, "autonomy.expression");
    readEnum(ctx, autonomy, "speech", config.autonomy.speech, speechModes(), "autonomy.speech");
    readBool(ctx, autonomy, "greet", config.autonomy.greet, "autonomy.greet");
    readBool(ctx, autonomy, "pauseWhenAway", config.autonomy.pauseWhenAway, "autonomy.pauseWhenAway");
    readInt(ctx, autonomy, "awayAfterMs", config.autonomy.awayAfterMs, 60000, 7200000, "autonomy.awayAfterMs");
    readInt(ctx, autonomy, "familiarity", config.autonomy.familiarity, 0, 1000000000, "autonomy.familiarity");
    readBool(ctx, autonomy, "breakReminder", config.autonomy.breakReminder, "autonomy.breakReminder");
    readInt(ctx, autonomy, "breakAfterMs", config.autonomy.breakAfterMs, 600000, 14400000, "autonomy.breakAfterMs");
    readBool(ctx, autonomy, "move", config.autonomy.move, "autonomy.move");
  }

  if (yyjson_val* wind = section(ctx, root, "wind")) {
    readBool(ctx, wind, "enabled", config.wind.enabled, "wind.enabled");
    readEnum(ctx, wind, "direction", config.wind.direction, windDirections(), "wind.direction");
    readNum(ctx, wind, "strength", config.wind.strength, 0, 1, "wind.strength");
  }

  if (yyjson_val* llm = section(ctx, root, "llm")) {
    auto& l = config.llm;
    readBool(ctx, llm, "enabled", l.enabled, "llm.enabled");
    readEnum(ctx, llm, "provider", l.provider, llmProviders(), "llm.provider");
    // baseUrl / apiKey / model 都不驗格式：端點長什麼樣只有使用者知道，
    // 真正的檢查在 buildChatCompletionsRequest（設定頁與呼叫路徑共用同一句錯誤）。
    readStr(ctx, llm, "baseUrl", l.baseUrl, 0, "llm.baseUrl");
    readStr(ctx, llm, "apiKey", l.apiKey, 0, "llm.apiKey");
    readStr(ctx, llm, "model", l.model, 0, "llm.model");
    readStr(ctx, llm, "anthropicApiKey", l.anthropicApiKey, 0, "llm.anthropicApiKey");
    readStr(ctx, llm, "anthropicModel", l.anthropicModel, 0, "llm.anthropicModel");
    readNum(ctx, llm, "temperature", l.temperature, 0, 2, "llm.temperature");
    readInt(ctx, llm, "maxTokens", l.maxTokens, 16, 8192, "llm.maxTokens");
    readInt(ctx, llm, "timeoutMs", l.timeoutMs, 5000, 600000, "llm.timeoutMs");
    readBool(ctx, llm, "driveIdle", l.driveIdle, "llm.driveIdle");
    readInt(ctx, llm, "idleLlmCooldownMs", l.idleLlmCooldownMs, 0, 3600000, "llm.idleLlmCooldownMs");
  }

  if (yyjson_val* crash = section(ctx, root, "crash")) {
    readStr(ctx, crash, "lastNotified", config.crash.lastNotified, 0, "crash.lastNotified");
  }

  if (yyjson_val* weather = section(ctx, root, "weather")) {
    auto& w = config.weather;
    readBool(ctx, weather, "enabled", w.enabled, "weather.enabled");
    readBool(ctx, weather, "autoLocate", w.autoLocate, "weather.autoLocate");
    readNum(ctx, weather, "latitude", w.latitude, -90, 90, "weather.latitude");
    readNum(ctx, weather, "longitude", w.longitude, -180, 180, "weather.longitude");
    readStr(ctx, weather, "locationName", w.locationName, 0, "weather.locationName");
    readEnum(ctx, weather, "unit", w.unit, temperatureUnits(), "weather.unit");
    readInt(ctx, weather, "pollIntervalMs", w.pollIntervalMs, 300000, 21600000, "weather.pollIntervalMs");
    readBool(ctx, weather, "alertEnabled", w.alertEnabled, "weather.alertEnabled");
    readInt(ctx, weather, "rainProbability", w.rainProbability, kMinWeatherRainProbability, kMaxWeatherRainProbability, "weather.rainProbability");
    readInt(ctx, weather, "lookaheadMinutes", w.lookaheadMinutes, kMinWeatherLookaheadMinutes, kMaxWeatherLookaheadMinutes, "weather.lookaheadMinutes");
    readStr(ctx, weather, "lastAlertKey", w.lastAlertKey, 0, "weather.lastAlertKey");
  }

  if (!ctx.ok) return std::nullopt;
  return config;
}

std::string serializeConfig(const AppConfig& config, bool pretty) {
  jsonu::MutDoc doc;
  yyjson_mut_val* root = yyjson_mut_obj(doc.get());
  doc.setRoot(root);

  {
    yyjson_mut_val* model = addSection(doc, root, "model");
    putNullableStr(doc, model, "current", config.model.current);
    putNum(doc, model, "scale", config.model.scale);
    putNum(doc, model, "opacity", config.model.opacity);
    putNullableNum(doc, model, "x", config.model.x);
    putNullableNum(doc, model, "y", config.model.y);
    putStr(doc, model, "preset", config.model.preset);
  }
  {
    yyjson_mut_val* interaction = addSection(doc, root, "interaction");
    putBool(doc, interaction, "lookAt", config.interaction.lookAt);
    putBool(doc, interaction, "clickThrough", config.interaction.clickThrough);
    putBool(doc, interaction, "dragMove", config.interaction.dragMove);
    putBool(doc, interaction, "wheelZoom", config.interaction.wheelZoom);
    putBool(doc, interaction, "lockPosition", config.interaction.lockPosition);
    putBool(doc, interaction, "tapMotion", config.interaction.tapMotion);
    putBool(doc, interaction, "dragSwing", config.interaction.dragSwing);
    putBool(doc, interaction, "speakFacingFront", config.interaction.speakFacingFront);
  }
  {
    yyjson_mut_val* tts = addSection(doc, root, "tts");
    putStr(doc, tts, "engine", config.tts.engine);
    putNullableStr(doc, tts, "voice", config.tts.voice);
    putNum(doc, tts, "rate", config.tts.rate);
    putNum(doc, tts, "volume", config.tts.volume);
    putBool(doc, tts, "showBubble", config.tts.showBubble);
    putNum(doc, tts, "bubbleMinDuration", config.tts.bubbleMinDuration);
    putNum(doc, tts, "bubbleOffsetY", config.tts.bubbleOffsetY);
    putNum(doc, tts, "bubbleMaxDelay", config.tts.bubbleMaxDelay);
    putStr(doc, tts, "bubbleShadow", config.tts.bubbleShadow);

    yyjson_mut_val* gpts = addSection(doc, tts, "gptsovits");
    putStr(doc, gpts, "baseUrl", config.tts.gptsovits.baseUrl);
    putStr(doc, gpts, "textLang", config.tts.gptsovits.textLang);
    putStr(doc, gpts, "mediaType", config.tts.gptsovits.mediaType);
    putNum(doc, gpts, "timeoutMs", config.tts.gptsovits.timeoutMs);
    {
      yyjson_mut_val* presets = yyjson_mut_arr(doc.get());
      yyjson_mut_obj_put(gpts, keyOf(doc, "presets"), presets);
      for (const auto& p : config.tts.gptsovits.presets) {
        yyjson_mut_val* obj = yyjson_mut_arr_add_obj(doc.get(), presets);
        putStr(doc, obj, "id", p.id);
        putStr(doc, obj, "name", p.name);
        putStr(doc, obj, "refAudioPath", p.refAudioPath);
        putStr(doc, obj, "promptText", p.promptText);
        putStr(doc, obj, "promptLang", p.promptLang);
        putStr(doc, obj, "locale", p.locale);
        putNullableStr(doc, obj, "gptWeights", p.gptWeights);
        putNullableStr(doc, obj, "sovitsWeights", p.sovitsWeights);
      }
    }
    putNum(doc, gpts, "topK", config.tts.gptsovits.topK);
    putNum(doc, gpts, "topP", config.tts.gptsovits.topP);
    putNum(doc, gpts, "temperature", config.tts.gptsovits.temperature);
    putStr(doc, gpts, "textSplitMethod", config.tts.gptsovits.textSplitMethod);
    putNum(doc, gpts, "batchSize", config.tts.gptsovits.batchSize);

    yyjson_mut_val* vb = addSection(doc, tts, "voicebox");
    putStr(doc, vb, "baseUrl", config.tts.voicebox.baseUrl);
    putNullableStr(doc, vb, "language", config.tts.voicebox.language);
    putNullableStr(doc, vb, "engine", config.tts.voicebox.engine);
    putNullableStr(doc, vb, "modelSize", config.tts.voicebox.modelSize);
    putNum(doc, vb, "timeoutMs", config.tts.voicebox.timeoutMs);

    yyjson_mut_val* cu = addSection(doc, tts, "custom");
    putStr(doc, cu, "url", config.tts.custom.url);
    putStr(doc, cu, "method", config.tts.custom.method);
    putStr(doc, cu, "params", config.tts.custom.params);
    putStr(doc, cu, "headers", config.tts.custom.headers);
    putNum(doc, cu, "timeoutMs", config.tts.custom.timeoutMs);
  }
  {
    yyjson_mut_val* mcp = addSection(doc, root, "mcp");
    putBool(doc, mcp, "enabled", config.mcp.enabled);
    putStr(doc, mcp, "host", config.mcp.host);
    putNum(doc, mcp, "port", config.mcp.port);
    putNullableStr(doc, mcp, "token", config.mcp.token);
    // 新欄位一律接在既有鍵的後面：serializeConfig 的鍵順序是既有設定檔的合約，
    // 插在中間會讓既有 config.json 每次重寫都整份 diff
    putBool(doc, mcp, "talkative", config.mcp.talkative);
    putBool(doc, mcp, "notifyOnComplete", config.mcp.notifyOnComplete);
    putBool(doc, mcp, "speakNoWait", config.mcp.speakNoWait);
    putBool(doc, mcp, "announceSteps", config.mcp.announceSteps);
  }
  {
    yyjson_mut_val* idle = addSection(doc, root, "idle");
    putBool(doc, idle, "autoReset", config.idle.autoReset);
    putNum(doc, idle, "resetMs", config.idle.resetMs);
    putBool(doc, idle, "perform", config.idle.perform);
    putNum(doc, idle, "performAfterMs", config.idle.performAfterMs);
    putNum(doc, idle, "performIntervalMs", config.idle.performIntervalMs);
  }
  {
    yyjson_mut_val* app = addSection(doc, root, "app");
    putBool(doc, app, "openAtLogin", config.app.openAtLogin);
    putBool(doc, app, "alwaysOnTop", config.app.alwaysOnTop);
    putBool(doc, app, "disableHardwareAcceleration", config.app.disableHardwareAcceleration);
    putStr(doc, app, "locale", config.app.locale);
    // 新欄位一律接在既有鍵後面（見 CLAUDE.md 的 serializeConfig 合約）
    putBool(doc, app, "sampleModelsPrompted", config.app.sampleModelsPrompted);
  }
  {
    // 新增的 section 一律接在最後（見 config_schema.h 的說明）；
    // persona → autonomy → wind 的順序出貨後即為合約，不能對調
    yyjson_mut_val* persona = addSection(doc, root, "persona");
    putNullableStr(doc, persona, "current", config.persona.current);
  }
  {
    yyjson_mut_val* autonomy = addSection(doc, root, "autonomy");
    putBool(doc, autonomy, "enabled", config.autonomy.enabled);
    putBool(doc, autonomy, "expression", config.autonomy.expression);
    putStr(doc, autonomy, "speech", config.autonomy.speech);
    putBool(doc, autonomy, "greet", config.autonomy.greet);
    putBool(doc, autonomy, "pauseWhenAway", config.autonomy.pauseWhenAway);
    putNum(doc, autonomy, "awayAfterMs", config.autonomy.awayAfterMs);
    putNum(doc, autonomy, "familiarity", config.autonomy.familiarity);
    putBool(doc, autonomy, "breakReminder", config.autonomy.breakReminder);
    putNum(doc, autonomy, "breakAfterMs", config.autonomy.breakAfterMs);
    putBool(doc, autonomy, "move", config.autonomy.move);
  }
  {
    yyjson_mut_val* wind = addSection(doc, root, "wind");
    putBool(doc, wind, "enabled", config.wind.enabled);
    putStr(doc, wind, "direction", config.wind.direction);
    putNum(doc, wind, "strength", config.wind.strength);
  }
  {
    yyjson_mut_val* llm = addSection(doc, root, "llm");
    putBool(doc, llm, "enabled", config.llm.enabled);
    putStr(doc, llm, "provider", config.llm.provider);
    putStr(doc, llm, "baseUrl", config.llm.baseUrl);
    putStr(doc, llm, "apiKey", config.llm.apiKey);
    putStr(doc, llm, "model", config.llm.model);
    putStr(doc, llm, "anthropicApiKey", config.llm.anthropicApiKey);
    putStr(doc, llm, "anthropicModel", config.llm.anthropicModel);
    putNum(doc, llm, "temperature", config.llm.temperature);
    putNum(doc, llm, "maxTokens", config.llm.maxTokens);
    putNum(doc, llm, "timeoutMs", config.llm.timeoutMs);
    putBool(doc, llm, "driveIdle", config.llm.driveIdle);
    putNum(doc, llm, "idleLlmCooldownMs", config.llm.idleLlmCooldownMs);
  }
  {
    yyjson_mut_val* crash = addSection(doc, root, "crash");
    putStr(doc, crash, "lastNotified", config.crash.lastNotified);
  }
  {
    yyjson_mut_val* weather = addSection(doc, root, "weather");
    putBool(doc, weather, "enabled", config.weather.enabled);
    putBool(doc, weather, "autoLocate", config.weather.autoLocate);
    putNum(doc, weather, "latitude", config.weather.latitude);
    putNum(doc, weather, "longitude", config.weather.longitude);
    putStr(doc, weather, "locationName", config.weather.locationName);
    putStr(doc, weather, "unit", config.weather.unit);
    putNum(doc, weather, "pollIntervalMs", config.weather.pollIntervalMs);
    putBool(doc, weather, "alertEnabled", config.weather.alertEnabled);
    putNum(doc, weather, "rainProbability", config.weather.rainProbability);
    putNum(doc, weather, "lookaheadMinutes", config.weather.lookaheadMinutes);
    putStr(doc, weather, "lastAlertKey", config.weather.lastAlertKey);
  }

  return doc.write(pretty);
}

}  // namespace l2m
