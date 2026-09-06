#include "mcp_tools.h"

#include <yyjson.h>

#include <QDebug>

#include <algorithm>
#include <memory>

#include "../app/app_controller.h"
#include "core/config_schema.h"
#include "core/json_doc.h"
#include "core/jsonrpc.h"
#include "core/model_commands.h"
#include "core/mcp_tool_specs.h"
#include "core/perform_step.h"

namespace l2m {

namespace {

// CommandResult → 工具輸出；失敗時把 hint（可用選項）一併帶給 AI
std::string fromResult(const CommandResult& result, const std::string& successText) {
  if (!result.ok) {
    return mcpErrorOutput(result.error.empty() ? "Command failed" : result.error, result.hint);
  }
  return mcpTextOutput(successText);
}

// 把一段 JSON 文字包成工具輸出（list_* 用）
std::string jsonOutput(const std::string& json) { return mcpTextOutput(json); }

// ── 參數取用 ──

yyjson_val* argsRoot(const jsonu::Doc& doc) { return doc.root(); }

std::optional<std::string> optString(yyjson_val* obj, const char* key) {
  yyjson_val* v = jsonu::get(obj, key);
  const char* s = v ? yyjson_get_str(v) : nullptr;
  if (!s) return std::nullopt;
  return std::string(s);
}

std::optional<double> optNumber(yyjson_val* obj, const char* key) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!v || !yyjson_is_num(v)) return std::nullopt;
  return yyjson_get_num(v);
}

std::optional<int> optInt(yyjson_val* obj, const char* key) {
  const auto n = optNumber(obj, key);
  if (!n) return std::nullopt;
  return static_cast<int>(*n);
}

std::optional<bool> optBool(yyjson_val* obj, const char* key) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!v || !yyjson_is_bool(v)) return std::nullopt;
  return yyjson_get_bool(v);
}

std::optional<std::vector<std::string>> optStringArray(yyjson_val* obj, const char* key) {
  yyjson_val* v = jsonu::get(obj, key);
  if (!v || !yyjson_is_arr(v)) return std::nullopt;

  std::vector<std::string> out;
  yyjson_arr_iter iter;
  yyjson_arr_iter_init(v, &iter);
  yyjson_val* item = nullptr;
  while ((item = yyjson_arr_iter_next(&iter))) {
    const char* s = yyjson_get_str(item);
    if (s) out.emplace_back(s);
  }
  return out;
}

std::string joinNames(const std::vector<std::string>& values) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i > 0) out += ", ";
    out += values[i];
  }
  return out;
}

std::string numberText(double value) {
  // 去掉沒必要的小數尾巴，訊息才不會出現 "1.500000"
  std::string text = std::to_string(value);
  if (text.find('.') == std::string::npos) return text;
  while (!text.empty() && text.back() == '0') text.pop_back();
  if (!text.empty() && text.back() == '.') text.pop_back();
  return text;
}

}  // namespace

McpTools::McpTools(AppController& controller, QObject* parent) : QObject(parent), controller_(controller) {}

void McpTools::invokeDetached(const std::string& tool, const std::string& argsJson) {
  auto call = std::make_shared<PendingCall>(tool, argsJson);
  invoke(call);
}

void McpTools::invoke(PendingCallPtr call) {
  if (!call) return;

  // AI 還在下指令：重新給滿「安靜 5 秒就把表情動作收乾淨」的倒數。
  // 放在分派之前、且不分工具種類 —— 唯讀的 get_state／list_* 也算，AI 還在
  // 呼叫工具就代表它還在這一輪對話裡，不該把它上一步設好的表情抽掉。
  controller_.notifyMcpCommand();

  const std::string& tool = call->tool();
  auto parsed = jsonu::Doc::parse(call->argsJson().empty() ? "{}" : call->argsJson());
  yyjson_val* args = parsed ? argsRoot(*parsed) : nullptr;

  // ── 查詢 ──
  if (tool == "list_models") {
    // 先重掃一次：使用者常常是「把新模型丟進資料夾」之後才叫 AI 去看
    controller_.rescan();
    const ModelInfo* model = controller_.currentModel();
    call->complete(jsonOutput(modelListJson(controller_.models(), model ? model->id : std::string())));
    return;
  }

  if (tool == "list_motions") {
    const ModelInfo* model = controller_.currentModel();
    if (!model) {
      call->complete(mcpErrorOutput("No model is loaded", "Call list_models to see what is available, then switch_model to load one."));
      return;
    }
    const std::string json = motionListJson(*model);
    call->complete(jsonOutput(model->annotations.motions.empty() ? mcpWithNote(json, kNotNamedNote) : json));
    return;
  }

  if (tool == "list_expressions") {
    const ModelInfo* model = controller_.currentModel();
    if (!model) {
      call->complete(mcpErrorOutput("No model is loaded", "Call list_models to see what is available, then switch_model to load one."));
      return;
    }
    const std::string json = expressionListJson(*model);
    // 參數型的表情名稱是模型作者自己寫的（哭哭、生氣），已經有意義，不必再叫使用者命名
    if (!model->paramExpressions.empty()) {
      call->complete(jsonOutput(mcpWithNote(json, kParamExpressionNote)));
    } else if (model->annotations.expressions.empty()) {
      call->complete(jsonOutput(mcpWithNote(json, kNotNamedNote)));
    } else {
      call->complete(jsonOutput(json));
    }
    return;
  }

  if (tool == "list_parameters") {
    const CommandResult report = controller_.parameterReport();
    if (!report.ok) {
      call->complete(mcpErrorOutput(report.error, report.hint));
      return;
    }
    const ModelInfo* model = controller_.currentModel();
    if (!model || model->parameters.empty()) {
      call->complete(mcpErrorOutput("This model exposes no parameter table",
                                    "Only models shipping a *.cdi3.json expose named parameters. Use list_motions and "
                                    "list_expressions instead."));
      return;
    }
    call->complete(jsonOutput(mcpWithNote(report.dataJson, kParameterRoleNote)));
    return;
  }

  if (tool == "list_voices") {
    const auto engine = optString(args, "engine");
    controller_.listVoicesJson(engine, [call](std::string json) { call->complete(jsonOutput(json)); });
    return;
  }

  if (tool == "get_state") {
    call->complete(jsonOutput(controller_.getStateJson()));
    return;
  }

  // ── 核心動作 ──
  // 回覆送出的時機是「切換已經開始」，不是「載入完成」—— switchModel() 會先淡出 400ms
  // 才真的載入（app_controller.cpp 的 fadeOutThen），而 currentModelId_ 與 config 在淡出
  // 之前就更新了，所以這裡讀到的已經是新模型的名字。刻意不等載入結束：載入是同步的、
  // 一進去 GUI 執行緒就凍住數秒，而 switch_model 不在 isLongToolCall 的名單裡（20 秒期限）。
  if (tool == "switch_model") {
    const auto name = optString(args, "name");
    if (!name) {
      call->complete(mcpErrorOutput("name is required", "Get model names from list_models."));
      return;
    }
    const CommandResult result = controller_.switchModel(*name);
    const ModelInfo* model = controller_.currentModel();
    call->complete(fromResult(result, "Switched to model \"" + (model ? model->name : *name) + "\""));
    return;
  }

  if (tool == "play_motion") {
    const auto group = optString(args, "group");
    if (!group) {
      call->complete(mcpErrorOutput("group is required", "Get motion groups from list_motions."));
      return;
    }
    const auto index = optInt(args, "index");
    const auto priority = optInt(args, "priority");
    call->complete(fromResult(controller_.playMotion(*group, index.value_or(-1), priority), "Played motion \"" + *group + "\""));
    return;
  }

  if (tool == "set_expression") {
    const auto name = optString(args, "name");
    const auto holdMs = optNumber(args, "hold_ms");
    const CommandResult result = controller_.setExpression(name, holdMs);
    std::string message = "Expression reset";
    if (name.has_value()) {
      message = "Applied expression \"" + *name + "\"";
      if (holdMs.has_value()) message += " for " + numberText(*holdMs) + "ms";
    }
    call->complete(fromResult(result, message));
    return;
  }

  // speak 與 think 是同一條路：差別只有 SpeakRequest::thinking 那一個旗標
  //（氣泡外型、殘響、嘴巴動不動全在下游決定）。分成兩個 if 會讓
  // speakNoWait 那段時序推理各存一份，遲早分岔。
  if (tool == "speak" || tool == "think") {
    const bool thinking = tool == "think";
    const auto text = optString(args, "text");
    if (!text || text->empty()) {
      call->complete(mcpErrorOutput(thinking ? "Text to think must not be empty" : "Text to speak must not be empty"));
      return;
    }
    AppController::SpeakRequest request;
    request.text = *text;
    request.voice = optString(args, "voice");
    request.engine = optString(args, "engine");
    request.rate = optNumber(args, "rate");
    request.wait = optBool(args, "wait").value_or(false);
    request.thinking = thinking;

    const std::string spoken = *text;
    qDebug().nospace() << "[mcp] " << tool.c_str() << "（" << spoken.size() << " bytes，wait=" << request.wait << "，engine=" << (request.engine ? request.engine->c_str() : "預設") << "）";

    // mcp.speakNoWait：排進佇列就回覆，不等開口也不等播完。
    //
    // 只擋在這裡，**不動 SpeechController 的 wait 語意** —— perform 走
    // PerformRunner 這條路根本不經過，它的多步時序（speakWait 預設 true）
    // 一個位元組都沒變。
    //
    // `!request.wait` 這個條件是刻意的：AI 明確送 wait=true 是在宣告一個時序
    // 相依（講完再換表情），這時當場回「已排入佇列」等於騙它 —— 它會以為那句
    // 已經唸完而接著做下一件事，正是 wait 這個參數存在的理由被拿掉。
    // 開關只負責「AI 沒有特別要求時不要卡住」。
    //
    // 代價：不掛回呼就收不到合成失敗、沒有音效裝置那類錯誤（氣泡與 log 仍然有）。
    // 所以回覆文字是 Queued 而不是 Spoke —— instructions 也會同步告訴 AI
    // 這件事，少了它 AI 可能以為講完了就補送 stop_speaking 把佇列砍光。
    if (controller_.config().get().mcp.speakNoWait && !request.wait) {
      controller_.speak(request, {});
      call->complete(fromResult(CommandResult::success(), "Queued: " + spoken));
      return;
    }

    const std::string verb = thinking ? "Thought: " : "Spoke: ";
    controller_.speak(request, [call, spoken, verb](CommandResult result) { call->complete(fromResult(result, verb + spoken)); });
    return;
  }

  if (tool == "stop_speaking") {
    // 佇列裡還沒唸的句子會在這裡整批消失，而且不會有任何錯誤。
    // AI 在 speak 被拒絕或逾時之後常會補送這個，於是使用者看到的是「沒講完」
    qWarning() << "[mcp] stop_speaking：佇列與目前這一句都會被丟棄";
    call->complete(fromResult(controller_.stopSpeaking(), "Stopped speaking"));
    return;
  }

  // ── 外觀與位置 ──
  if (tool == "move_to") {
    const CommandResult result = controller_.moveTo(optNumber(args, "x"), optNumber(args, "y"), optString(args, "preset"));
    std::string message = "Moved";
    if (auto moved = jsonu::Doc::parse(result.dataJson)) {
      message += " to (" + numberText(yyjson_get_num(jsonu::get(moved->root(), "x"))) + ", " + numberText(yyjson_get_num(jsonu::get(moved->root(), "y"))) + ")";
    }
    call->complete(fromResult(result, message));
    return;
  }

  if (tool == "set_scale") {
    const auto scale = optNumber(args, "scale");
    if (!scale) {
      call->complete(mcpErrorOutput("scale must be a number"));
      return;
    }
    call->complete(fromResult(controller_.setScale(*scale), "Scale set to " + numberText(*scale)));
    return;
  }

  if (tool == "set_opacity") {
    const auto opacity = optNumber(args, "opacity");
    if (!opacity) {
      call->complete(mcpErrorOutput("opacity must be a number"));
      return;
    }
    call->complete(fromResult(controller_.setOpacity(*opacity), "Opacity set to " + numberText(*opacity)));
    return;
  }

  if (tool == "set_visible") {
    const auto visible = optBool(args, "visible");
    if (!visible) {
      call->complete(mcpErrorOutput("visible must be a boolean"));
      return;
    }
    call->complete(fromResult(controller_.setVisible(*visible), *visible ? "Character shown" : "Character hidden"));
    return;
  }

  if (tool == "set_always_on_top") {
    const auto enabled = optBool(args, "enabled");
    if (!enabled) {
      call->complete(mcpErrorOutput("enabled must be a boolean"));
      return;
    }
    call->complete(fromResult(controller_.setAlwaysOnTop(*enabled), *enabled ? "Always-on-top enabled" : "Always-on-top disabled"));
    return;
  }

  if (tool == "look_at") {
    if (optBool(args, "reset").value_or(false)) {
      call->complete(fromResult(controller_.lookAt(std::nullopt, std::nullopt), "Gaze reset to the front"));
      return;
    }
    const double x = optNumber(args, "x").value_or(0);
    const double y = optNumber(args, "y").value_or(0);
    call->complete(fromResult(controller_.lookAt(x, y), "Now looking at (" + numberText(x) + ", " + numberText(y) + ")"));
    return;
  }

  // ── 參數層 ──
  if (tool == "set_parameters") {
    std::string error;
    auto params = parseParameterRequests(jsonu::get(args, "params"), &error);
    if (!params) {
      call->complete(mcpErrorOutput(error,
                                    "params is an array of { id, value, duration_ms?, "
                                    "hold_ms?, mode? }; get ids from list_parameters."));
      return;
    }
    const size_t count = params->size();
    call->complete(fromResult(controller_.setParameters(*params), "Set " + std::to_string(count) + " parameter(s)"));
    return;
  }

  if (tool == "reset_parameters") {
    const auto ids = optStringArray(args, "ids");
    const CommandResult result = controller_.resetParameters(ids);
    const std::string message = (ids.has_value() && !ids->empty()) ? "Released " + joinNames(*ids) : "Released every overridden parameter";
    call->complete(fromResult(result, message));
    return;
  }

  if (tool == "animate") {
    std::string error;
    auto keyframes = parseKeyframes(jsonu::get(args, "keyframes"), &error);
    if (!keyframes) {
      call->complete(mcpErrorOutput(error, "Each keyframe is { at_ms, params }; give at least two at different times."));
      return;
    }
    BuildMotionOptions options;
    options.loop = optBool(args, "loop").value_or(false);
    options.fadeInMs = optNumber(args, "fade_in_ms");
    options.fadeOutMs = optNumber(args, "fade_out_ms");
    call->complete(fromResult(controller_.animate(*keyframes, options), "Playing the synthesized motion"));
    return;
  }

  if (tool == "open_naming_editor") {
    call->complete(fromResult(controller_.openNaming(),
                              "Opened the Settings window on its Models tab. Ask the user to fill in a meaning for each "
                              "motion and expression in the \"Name Motions & Expressions\" list (\"Try it\" previews them), "
                              "then call list_motions again."));
    return;
  }

  // ── 進階 ──
  if (tool == "perform") {
    std::string error;
    auto steps = parsePerformSteps(jsonu::get(args, "steps"), &error);
    if (!steps) {
      call->complete(mcpErrorOutput(error, "steps is an array of { action, ... }; see the tool schema."));
      return;
    }
    const size_t count = steps->size();
    controller_.perform(*steps, [call, count](CommandResult result) { call->complete(fromResult(result, "Completed " + std::to_string(count) + " steps")); });
    return;
  }

  if (tool == "schedule") {
    const auto delayMs = optNumber(args, "delay_ms");
    const auto target = optString(args, "tool");
    if (!delayMs || !target) {
      call->complete(mcpErrorOutput("delay_ms and tool are required"));
      return;
    }
    if (!mcpToolExists(*target) || *target == "schedule") {
      std::vector<std::string> names;
      for (const auto& spec : mcpToolSpecs()) {
        if (std::string(spec.name) != "schedule") names.emplace_back(spec.name);
      }
      call->complete(mcpErrorOutput("Unknown tool \"" + *target + "\"", "Schedulable tools: " + joinNames(names)));
      return;
    }

    yyjson_val* inner = jsonu::get(args, "args");
    const std::string innerJson = inner ? valueToJson(inner) : "{}";
    const std::string toolName = *target;

    const CommandResult result = controller_.schedule(*delayMs, [this, toolName, innerJson] {
      // 射後不理：排程到期時沒有等待端，工具的輸出直接丟掉
      invokeDetached(toolName, innerJson);
    });
    call->complete(fromResult(result, "Scheduled: " + toolName + " runs in " + numberText(*delayMs) + "ms"));
    return;
  }

  call->complete(mcpErrorOutput("Unknown tool \"" + tool + "\""));
}

}  // namespace l2m
