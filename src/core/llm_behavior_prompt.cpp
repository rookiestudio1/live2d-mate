#include "llm_behavior_prompt.h"

#include <algorithm>

#include "json_doc.h"
#include "string_util.h"

namespace l2m {

namespace {

// 與 IdleDirector 的 kIdleExpressionHoldMs 同值：自主表演套的表情自己收，
// 不依賴 idle.autoReset。兩處要一起改（那邊是匿名 namespace 的常數）。
constexpr double kLlmExpressionHoldMs = 12000;
// 與 IdleDirector 的 kMoveGlideMs 同值：閒置散步用緩滑，瞬移看起來像 bug
constexpr double kLlmMoveGlideMs = 1500;

// prompt 裡的台詞樣本上限：全部塞進去只是燒 token，挑前幾句夠 LLM 抓語氣
constexpr size_t kMaxSampleLines = 8;

const char* idleLevelName(IdleLevel level) {
  switch (level) {
    case IdleLevel::Fidget:
      return "fidget (only idle briefly; keep it subtle)";
    case IdleLevel::Bored:
      return "bored (idle for a while; a fuller performance is fine)";
    case IdleLevel::Sleepy:
      return "sleepy (idle for a long time; act drowsy, or do nothing at all)";
  }
  return "fidget";
}

// 動作群組的命名語意：群組層級優先，沒有就拿群組內第一個有寫的
std::string motionMeaning(const ModelAnnotations& annotations, const MotionGroupInfo& group) {
  const auto find = [&annotations](const std::string& key) -> std::string {
    const auto it = annotations.motions.find(key);
    return it != annotations.motions.end() ? it->second : std::string();
  };
  std::string meaning = find(motionKey(group.name));
  for (int i = 0; meaning.empty() && i < group.count; ++i) {
    meaning = find(motionKey(group.name, i));
  }
  return meaning;
}

std::string occasionNote(const std::string& occasion) {
  if (occasion == "welcome") {
    return "The user just arrived (app start, back from away, or wake from sleep). Greet "
           "them with one line; skip motions and expressions unless one clearly fits. If "
           "the weather line below says something worth remarking on, you may work it into "
           "the greeting - but only when it fits naturally; a plain hello is fine, and "
           "reciting a forecast is not a greeting.";
  }
  if (occasion == "breakReminder") {
    return "The user has been at the computer for a very long stretch. Gently suggest a "
           "short break in one line, in character. Do not nag.";
  }
  if (occasion == "petted") {
    return "The user just petted the character. React warmly with at most one short line.";
  }
  if (occasion == "weatherAlert") {
    return "Bad weather is about to start (see weather_alert below). Warn the user in ONE "
           "short line so they can act on it - take an umbrella, grab a coat, close the "
           "window. Say what is coming and roughly when. Do not report the current "
           "weather, and do not add motions or expressions unless one clearly fits.";
  }
  return "Routine idle moment. A tiny performance, one muttered line, or nothing at all "
         "are all good answers. Doing nothing is often best; when you do speak, mutter "
         "something fresh the persona would plausibly say right now.";
}

}  // namespace

std::vector<LlmMessage> buildBehaviorPromptMessages(const LlmBehaviorPromptInput& input) {
  const IdleWorld& world = input.world;
  const IdlePlanOptions& options = input.options;

  std::string system;
  system +=
    "You are the behavior brain of a Live2D desktop mascot. Plan a short performance for "
    "this moment.\n"
    "Respond with ONLY a JSON array of steps - no prose, no markdown fences. Step shapes:\n"
    "  {\"action\":\"motion\",\"group\":\"<group name>\"}\n"
    "  {\"action\":\"expression\",\"name\":\"<expression name>\"}\n"
    "  {\"action\":\"speak\",\"text\":\"<one short line, max 200 chars>\"}\n";
  // move 的格式只在真的能動時才列出來：沒看過格式的步驟，模型不太會憑空生出來。
  // 這只是省一次白工，**不是**防線 —— 真正擋下來的是 parseBehaviorSteps 的 allowMove
  if (world.canMove) system += "  {\"action\":\"move\",\"x\":<0..1>,\"y\":<0..1>}\n";
  system +=
    "  {\"action\":\"wait\",\"ms\":<0..60000>}\n"
    "Rules:\n"
    "- 1 to 3 steps is usually right; the hard cap is 20. An empty array [] means \"do "
    "nothing this round\" and is a perfectly good answer - a mascot that moves every "
    "minute is annoying.\n"
    "- Only use motion groups and expression names from the lists in the context. Prefer "
    "entries that have a described meaning; never guess names.\n"
    "- speak lines must be ORIGINAL lines you write yourself, in the persona's voice and "
    "language, grounded in the current context (time of day, mood, familiarity, occasion). "
    "One short sentence only. Do not repeat any sample line verbatim.\n"
    "- Respect can_speak / can_move in the context.\n";
  if (!input.personaDescription.empty()) {
    system += "\nPersona (stay in character):\n" + input.personaDescription + "\n";
  } else {
    system += "\nNo persona is set: stay quiet (no speak steps), gestures only.\n";
  }
  if (!input.memory.empty()) {
    // 長期記憶接在 persona 後面：兩者都是「你是誰／你知道什麼」，
    // 屬於 system 而不是這一輪的 context
    system +=
      "\nLong-term notes about your user (things you remember; the user maintains "
      "this file - treat it as true):\n" +
      input.memory + "\n";
  }

  std::string user;
  user += "occasion: " + options.occasion + "\n";
  user += std::string("note: ") + occasionNote(options.occasion) + "\n";
  user += std::string("idle_level: ") + idleLevelName(options.level) + "\n";
  user += std::string("mood: ") + (options.cheerful ? "cheerful (was petted recently)" : "neutral") + "\n";
  user += "familiarity: " + std::to_string(options.familiarityLevel) + " of 3\n";
  user += std::string("time_of_day: ") + dayPeriodPrefix(dayPeriodFor(input.hour)) + " (hour " + std::to_string(input.hour) + ")\n";
  // 天氣永遠帶（有資料的話）：使用者要的「當背景知識」就是這一格。
  // 預警的那一句另外一行 —— 合成一段模型會分不清該提醒哪一件事
  if (!input.weather.empty()) user += "weather: " + input.weather + "\n";
  if (!input.weatherAlert.empty()) user += "weather_alert: " + input.weatherAlert + "\n";
  user += std::string("can_speak: ") + (world.canSpeak ? "true" : "false") + "\n";
  user += std::string("can_move: ") + (world.canMove ? "true" : "false") + "\n";
  if (world.windowXRatio && world.windowYRatio) {
    user += "window_position: x=" + std::to_string(*world.windowXRatio) + " y=" + std::to_string(*world.windowYRatio) + " (0..1 of the work area)\n";
  }

  user += "motion groups:\n";
  if (world.motions.empty()) user += "  (none)\n";
  for (const auto& group : world.motions) {
    user += "  - " + group.name;
    const std::string meaning = motionMeaning(world.annotations, group);
    if (!meaning.empty()) user += " (meaning: " + meaning + ")";
    user += "\n";
  }

  user += "expressions:\n";
  if (world.expressions.empty()) user += "  (none)\n";
  for (const auto& name : world.expressions) {
    user += "  - " + name;
    const auto it = world.annotations.expressions.find(name);
    if (it != world.annotations.expressions.end() && !it->second.empty()) {
      user += " (meaning: " + it->second + ")";
    }
    user += "\n";
  }

  if (world.canSpeak && !world.lines.empty()) {
    // 台詞清單只當語氣參考 —— 目的就是讓 LLM「自主產生」：
    // 給成候選的話模型十次有八次直接挑一句，跟規則版沒有分別
    user +=
      "sample lines the persona author wrote (STYLE REFERENCE ONLY - capture the "
      "voice, then write your own fresh line; never copy one verbatim):\n";
    const size_t count = std::min(world.lines.size(), kMaxSampleLines);
    for (size_t i = 0; i < count; ++i) user += "  - " + world.lines[i] + "\n";
  }

  if (!input.recentLines.empty()) {
    user +=
      "lines you already said in the last rounds (do NOT repeat or closely "
      "paraphrase any of them - say something different, or stay silent):\n";
    for (const auto& line : input.recentLines) user += "  - " + line + "\n";
  }

  return {{"system", system}, {"user", user}};
}

std::optional<std::vector<PerformStep>> parseBehaviorSteps(const std::string& responseText, bool allowMove, std::string* error) {
  // 剝 markdown code fence：指令說了不要，但小模型照加是常態
  const std::string text = strutil::stripCodeFence(responseText);

  const auto doc = jsonu::Doc::parse(text);
  if (!doc || !doc->root()) {
    if (error) *error = "response is not valid JSON";
    return std::nullopt;
  }

  // 裸陣列或 {"steps":[…]} 都收
  yyjson_val* steps = doc->root();
  if (yyjson_is_obj(steps)) steps = jsonu::get(steps, "steps");
  if (!steps || !yyjson_is_arr(steps)) {
    if (error) *error = "response has no steps array";
    return std::nullopt;
  }

  auto parsed = parsePerformSteps(steps, error);
  if (!parsed) return std::nullopt;

  std::vector<PerformStep> out;
  for (auto& step : *parsed) {
    if (out.size() >= kMaxPerformSteps) break;
    // 自主表演只開放這五種：parameters / animate 亂寫會讓姿勢壞掉且復原收不乾淨
    if (step.action == "parameters" || step.action == "animate") continue;
    if (step.action == "speak") {
      const std::string line = strutil::trim(step.text);
      if (line.empty() || strutil::utf8Length(line) > kLlmSpeakMaxChars) continue;
      step.text = line;
      step.voice = std::nullopt;  // 語音永遠用使用者設定的，不讓 LLM 指定
      step.speakWait = true;      // 講完才進下一步，idleSuppress_ 才蓋得住整段
    } else if (step.action == "expression") {
      if (step.name.empty()) continue;
      step.holdMs = kLlmExpressionHoldMs;
    } else if (step.action == "motion") {
      if (step.group.empty()) continue;
    } else if (step.action == "move") {
      // 不准動就整個丟掉 —— prompt 的 can_move 是拿自然語言拜託模型，小模型照樣吐 move。
      // 這是規劃當下的快照；設定在網路往返途中被改掉的那一段，由
      // AppController::runAutonomous() 執行前現讀 autonomousMoveAllowed() 收尾
      if (!allowMove) continue;
      if (!step.x && !step.y && !step.preset) continue;
      if (step.x) step.x = std::clamp(*step.x, 0.0, 1.0);
      if (step.y) step.y = std::clamp(*step.y, 0.0, 1.0);
      step.glideMs = kLlmMoveGlideMs;
    } else if (step.action == "wait") {
      step.ms = std::clamp(step.ms, 0.0, kMaxWaitMs);
    }
    out.push_back(std::move(step));
  }
  return out;
}

}  // namespace l2m
