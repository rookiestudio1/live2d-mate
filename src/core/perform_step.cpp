#include "perform_step.h"

#include "json_doc.h"

namespace l2m {

namespace {

bool isNumber(yyjson_val* v) { return v && yyjson_is_num(v); }

std::optional<double> optNumber(yyjson_val* obj, const char* key) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!isNumber(v)) return std::nullopt;
  return yyjson_get_num(v);
}

std::optional<int> optInt(yyjson_val* obj, const char* key) {
  const auto n = optNumber(obj, key);
  if (!n) return std::nullopt;
  return static_cast<int>(*n);
}

std::optional<std::string> optString(yyjson_val* obj, const char* key) {
  yyjson_val* v = jsonu::get(obj, key);
  const char* s = v ? yyjson_get_str(v) : nullptr;
  if (!s) return std::nullopt;
  return std::string(s);
}

bool optBool(yyjson_val* obj, const char* key, bool fallback) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!v || !yyjson_is_bool(v)) return fallback;
  return yyjson_get_bool(v);
}

void setError(std::string* error, std::string message) {
  if (error) *error = std::move(message);
}

}  // namespace

std::optional<std::vector<SetParameterRequest>> parseParameterRequests(yyjson_val* params, std::string* error) {
  if (!params || !yyjson_is_arr(params)) {
    setError(error, "params must be an array");
    return std::nullopt;
  }

  std::vector<SetParameterRequest> out;
  size_t idx = 0;
  size_t max = 0;
  yyjson_val* item = nullptr;
  yyjson_arr_iter iter;
  yyjson_arr_iter_init(params, &iter);
  while ((item = yyjson_arr_iter_next(&iter))) {
    ++idx;
    (void)max;
    if (!yyjson_is_obj(item)) {
      setError(error, "params[" + std::to_string(idx - 1) + "] must be an object");
      return std::nullopt;
    }
    const auto id = optString(item, "id");
    const auto value = optNumber(item, "value");
    if (!id || id->empty()) {
      setError(error, "params[" + std::to_string(idx - 1) + "].id is required");
      return std::nullopt;
    }
    if (!value) {
      setError(error, "params[" + std::to_string(idx - 1) + "].value must be a number");
      return std::nullopt;
    }

    SetParameterRequest req;
    req.id = *id;
    req.value = *value;
    req.durationMs = optNumber(item, "duration_ms");
    req.holdMs = optNumber(item, "hold_ms");
    const auto mode = optString(item, "mode");
    req.mode = (mode && *mode == "add") ? ParameterMode::Add : ParameterMode::Set;
    out.push_back(std::move(req));
  }
  return out;
}

std::optional<std::vector<Keyframe>> parseKeyframes(yyjson_val* keyframes, std::string* error) {
  if (!keyframes || !yyjson_is_arr(keyframes)) {
    setError(error, "keyframes must be an array");
    return std::nullopt;
  }

  std::vector<Keyframe> out;
  size_t idx = 0;
  yyjson_val* item = nullptr;
  yyjson_arr_iter iter;
  yyjson_arr_iter_init(keyframes, &iter);
  while ((item = yyjson_arr_iter_next(&iter))) {
    ++idx;
    if (!yyjson_is_obj(item)) {
      setError(error, "keyframes[" + std::to_string(idx - 1) + "] must be an object");
      return std::nullopt;
    }
    const auto at = optNumber(item, "at_ms");
    if (!at) {
      setError(error, "keyframes[" + std::to_string(idx - 1) + "].at_ms must be a number");
      return std::nullopt;
    }

    Keyframe frame;
    frame.at = *at;
    yyjson_val* params = jsonu::get(item, "params");
    if (params && yyjson_is_obj(params)) {
      yyjson_obj_iter pit;
      yyjson_obj_iter_init(params, &pit);
      yyjson_val* key = nullptr;
      while ((key = yyjson_obj_iter_next(&pit))) {
        yyjson_val* val = yyjson_obj_iter_get_val(key);
        if (!isNumber(val)) continue;
        frame.params.emplace_back(yyjson_get_str(key), yyjson_get_num(val));
      }
    }
    out.push_back(std::move(frame));
  }
  return out;
}

std::optional<std::vector<PerformStep>> parsePerformSteps(yyjson_val* steps, std::string* error) {
  if (!steps || !yyjson_is_arr(steps)) {
    setError(error, "steps must be an array");
    return std::nullopt;
  }

  std::vector<PerformStep> out;
  size_t idx = 0;
  yyjson_val* item = nullptr;
  yyjson_arr_iter iter;
  yyjson_arr_iter_init(steps, &iter);
  while ((item = yyjson_arr_iter_next(&iter))) {
    const std::string where = "steps[" + std::to_string(idx) + "]";
    ++idx;
    if (!yyjson_is_obj(item)) {
      setError(error, where + " must be an object");
      return std::nullopt;
    }

    PerformStep step;
    const auto action = optString(item, "action");
    if (!action) {
      setError(error, where + ".action is required");
      return std::nullopt;
    }
    step.action = *action;

    if (step.action == "motion") {
      step.group = optString(item, "group").value_or("");
      step.index = optInt(item, "index");
    } else if (step.action == "expression") {
      step.name = optString(item, "name").value_or("");
    } else if (step.action == "speak") {
      step.text = optString(item, "text").value_or("");
      step.voice = optString(item, "voice");
      step.speakWait = optBool(item, "wait", true);
      step.thinking = optBool(item, "thinking", false);
    } else if (step.action == "move") {
      step.x = optNumber(item, "x");
      step.y = optNumber(item, "y");
      step.preset = optString(item, "preset");
    } else if (step.action == "wait") {
      step.ms = optNumber(item, "ms").value_or(0);
    } else if (step.action == "parameters") {
      std::string inner;
      auto params = parseParameterRequests(jsonu::get(item, "params"), &inner);
      if (!params) {
        setError(error, where + ": " + inner);
        return std::nullopt;
      }
      step.params = std::move(*params);
    } else if (step.action == "animate") {
      std::string inner;
      auto frames = parseKeyframes(jsonu::get(item, "keyframes"), &inner);
      if (!frames) {
        setError(error, where + ": " + inner);
        return std::nullopt;
      }
      step.keyframes = std::move(*frames);
      step.animateOptions.loop = optBool(item, "loop", false);
      step.animateOptions.fadeInMs = optNumber(item, "fade_in_ms");
      step.animateOptions.fadeOutMs = optNumber(item, "fade_out_ms");
    } else {
      setError(error, "Unknown step action: " + step.action);
      return std::nullopt;
    }

    out.push_back(std::move(step));
  }
  return out;
}

}  // namespace l2m
