#include "mcp_tool_specs.h"

#include <yyjson.h>

#include <QDebug>

#include <algorithm>

#include "config_schema.h"
#include "json_doc.h"
#include "mcp_resources.h"
#include "string_util.h"

namespace l2m {

const char* const kNotNamedNote =
  "The motions/expressions of this model have no user-written meanings yet, and their raw names are "
  "usually not meaningful, so it is hard to tell which one to use. Call open_naming_editor to bring "
  "up the Settings window on its Models tab, then ask the user to write a meaning for each entry in "
  "the \"Name Motions & Expressions\" list and confirm it with \"Try it\".";

const char* const kParamExpressionNote =
  "These expressions are not .exp3.json files - they are single model parameters that the author "
  "named (this is how VTuber models made for face tracking store their expressions). They are "
  "mutually exclusive: applying one resets the others. For anything finer, use list_parameters and "
  "set_parameters.";

const char* const kParameterRoleNote =
  "role tells you whether writing a parameter does anything: \"free\" is always writable; "
  "\"physics-input\" is writable and also drives the hair/cloth physics; \"physics-output\" is "
  "computed by the physics simulation every frame, so writing it has no effect. value/min/max/"
  "default come from the loaded model.";

namespace {

std::string escapeJsonText(const std::string& raw) {
  std::string out;
  out.reserve(raw.size() + 16);
  for (const unsigned char c : raw) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
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
          static const char* kHex = "0123456789abcdef";
          out += "\\u00";
          out += kHex[c >> 4];
          out += kHex[c & 0x0F];
        } else {
          out += static_cast<char>(c);
        }
    }
  }
  return out;
}

// 數字轉字串時去掉沒必要的小數尾巴，說明文字才不會出現 "0.200000"
std::string numberText(double value) {
  std::string text = std::to_string(value);
  if (text.find('.') == std::string::npos) return text;
  while (!text.empty() && text.back() == '0') text.pop_back();
  if (!text.empty() && text.back() == '.') text.pop_back();
  return text;
}

// ── schema 常數 ──

constexpr const char* kEmptySchema = R"J({"type":"object","properties":{}})J";

constexpr const char* kParameterItemSchema = R"J({
  "type": "object",
  "properties": {
    "id": { "type": "string", "description": "Parameter id from list_parameters, e.g. expression6" },
    "value": { "type": "number", "description": "Target value; clamped to the range list_parameters reports" },
    "duration_ms": { "type": "integer", "minimum": 0, "maximum": 60000, "description": "Tween time from the current value; omit for an instant change" },
    "hold_ms": { "type": "integer", "minimum": 0, "maximum": 600000, "description": "How long to hold after arriving, then fade back to the default. Omit to hold until reset_parameters." },
    "mode": { "type": "string", "enum": ["set", "add"], "description": "set (default) overrides the parameter; add layers on top of the built-in breathing and gaze" }
  },
  "required": ["id", "value"]
})J";

constexpr const char* kKeyframeItemSchema = R"J({
  "type": "object",
  "properties": {
    "at_ms": { "type": "integer", "minimum": 0, "maximum": 600000, "description": "Milliseconds from the start of the motion" },
    "params": { "type": "object", "additionalProperties": { "type": "number" }, "description": "Parameter id to value at this point in time" }
  },
  "required": ["at_ms", "params"]
})J";

}  // namespace

std::string mcpTextOutput(const std::string& value) { return "{\"content\":[{\"type\":\"text\",\"text\":\"" + escapeJsonText(value) + "\"}]}"; }

std::string mcpErrorOutput(const std::string& message, const std::string& hint) {
  const std::string body = hint.empty() ? message : message + "\n" + hint;
  return "{\"content\":[{\"type\":\"text\",\"text\":\"" + escapeJsonText(body) + "\"}],\"isError\":true}";
}

// 在既有 JSON 物件文字上補一個字串欄位（用來加 note）
std::string mcpWithNote(const std::string& json, const char* note) {
  auto parsed = jsonu::Doc::parse(json);
  if (!parsed || !parsed->root()) return json;

  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_val_mut_copy(d, parsed->root());
  doc.setRoot(root);
  yyjson_mut_obj_add_str(d, root, "note", note);
  return doc.write(true);
}

const std::vector<ToolSpec>& mcpToolSpecs() {
  // 這些字面值是 AI 唯一看得到的說明，測試逐字釘住，改一個字都會改變 AI 的行為。
  static const std::string kParamsArraySchema =
    std::string(R"J({"type":"object","properties":{"params":{"type":"array","minItems":1,"items":)J") + kParameterItemSchema + R"J(}},"required":["params"]})J";

  static const std::string kAnimateSchema = std::string(R"J({"type":"object","properties":{"keyframes":{"type":"array","minItems":2,)J"
                                                        R"J("description":"At least two keyframes at different times. Repeat a value to hold )J"
                                                        R"J(a parameter still through a section.","items":)J") +
                                            kKeyframeItemSchema +
                                            R"J(},"loop":{"type":"boolean","description":"Repeat until another motion takes over, default false"},)J"
                                            R"J("fade_in_ms":{"type":"integer","minimum":0,"maximum":5000},)J"
                                            R"J("fade_out_ms":{"type":"integer","minimum":0,"maximum":5000}},"required":["keyframes"]})J";

  static const std::string kPerformSchema = std::string(R"J({"type":"object","properties":{"steps":{"type":"array","minItems":1,)J"
                                                        R"J("description":"Steps to run in order","items":{"type":"object","properties":{)J"
                                                        R"J("action":{"type":"string","enum":["motion","expression","speak","move","wait","parameters","animate"]},)J"
                                                        R"J("group":{"type":"string"},"index":{"type":"integer","minimum":0},)J"
                                                        R"J("name":{"type":"string"},"text":{"type":"string"},"voice":{"type":"string"},)J"
                                                        R"J("wait":{"type":"boolean","description":"Defaults to true: wait for this line to finish before the next step"},)J"
                                                        R"J("thinking":{"type":"boolean","description":"Make this line a thought instead of speech: thought bubble, echo, mouth stays shut"},)J"
                                                        R"J("x":{"type":"number"},"y":{"type":"number"},)J"
                                                        R"J("preset":{"type":"string","enum":["bottom-right","bottom-left","top-right","top-left","center"]},)J"
                                                        R"J("ms":{"type":"integer","minimum":0,"maximum":60000},)J"
                                                        R"J("params":{"type":"array","items":)J") +
                                            kParameterItemSchema + R"J(},"keyframes":{"type":"array","items":)J" + kKeyframeItemSchema +
                                            R"J(},"loop":{"type":"boolean"},"fade_in_ms":{"type":"integer"},"fade_out_ms":{"type":"integer"}},)J"
                                            R"J("required":["action"]}}},"required":["steps"]})J";

  static const std::string kScaleSchema =
    std::string(R"J({"type":"object","properties":{"scale":{"type":"number","minimum":)J") + numberText(kMinScale) + R"J(,"maximum":)J" + numberText(kMaxScale) + R"J(}},"required":["scale"]})J";

  static const std::string kScaleDescription = "Set the character scale multiplier (" + numberText(kMinScale) + " ~ " + numberText(kMaxScale) + ").";

  static const std::vector<ToolSpec> specs{
    // ── 查詢 ──
    {"list_models",
     "List every available Live2D model together with its motion groups and expressions, and "
     "which one is current. Get the name for switch_model from here.",
     kEmptySchema, true},
    {"list_motions",
     "List the motion groups of the current model, each motion inside a group, and the meanings the "
     "user wrote for them. Raw model names are usually not meaningful, so prefer picking by meaning.",
     kEmptySchema, true},
    {"list_expressions",
     "List the expression names of the current model and the meanings the user wrote for them. "
     "Prefer picking by meaning.",
     kEmptySchema, true},
    {"list_voices", "List the available TTS voices.",
     R"J({"type":"object","properties":{"engine":{"type":"string","description":"Only list voices of this engine, e.g. edge, sapi, gptsovits, voicebox or custom"}}})J", true},
    {"get_state",
     "Get the full state: current model, window position and size, screen size, settings, TTS "
     "engines and MCP info.",
     kEmptySchema, true},

    // ── 核心動作 ──
    {"switch_model", "Switch to the given Live2D model.",
     R"J({"type":"object","properties":{"name":{"type":"string","description":"Model name or id; get them from list_models"}},"required":["name"]})J", false},
    {"play_motion",
     "Play a motion. group accepts a motion group name, or a meaning the user wrote (e.g. \"waving "
     "happily\"); when a meaning refers to one specific motion, its index is filled in "
     "automatically. Omit index to pick one at random within the group.",
     R"J({"type":"object","properties":{"group":{"type":"string","description":"Motion group name (e.g. Idle, TapBody), or a meaning named by the user"},"index":{"type":"integer","minimum":0,"description":"Motion index within the group, starting at 0"},"priority":{"type":"integer","minimum":0,"maximum":3,"description":"0=none 1=idle 2=normal 3=force (default; interrupts the current motion so it plays)"}},"required":["group"]})J",
     false},
    {"set_expression",
     "Apply an expression; omit name to reset to the default one. name accepts an expression name or "
     "a meaning the user wrote. Expressions persist until something changes them, so pass hold_ms "
     "when the expression belongs to one moment. Either way the app resets the expression on its own "
     "once nothing has happened for a while, so a face never gets stuck.",
     R"J({"type":"object","properties":{"name":{"type":"string","description":"Expression name (e.g. F01), or a meaning named by the user"},"hold_ms":{"type":"integer","minimum":0,"maximum":600000,"description":"How long to hold before resetting on its own; omit to hold until something changes it"}}})J",
     false},
    {"speak",
     "Make the character speak: synthesize speech, sync the mouth and show a speech bubble. With "
     "wait=true it returns only after the line finishes.",
     R"J({"type":"object","properties":{"text":{"type":"string","minLength":1,"description":"Text to speak"},"voice":{"type":"string","description":"Voice id; get them from list_voices"},"engine":{"type":"string","description":"TTS engine to use, e.g. edge, sapi, gptsovits, voicebox or custom"},"rate":{"type":"number","minimum":0.5,"maximum":2,"description":"Speech rate multiplier, 1 is normal"},"wait":{"type":"boolean","description":"Wait for playback to finish before returning, default false"}},"required":["text"]})J",
     false},
    {"think",
     "Let the character think out loud. Same pipeline as speak, but it reads as an inner voice: "
     "the line plays with a soft echo, the mouth stays shut, and the bubble is a thought bubble "
     "(an oval trailed by small circles) instead of a speech bubble. Use it for what the character "
     "is not saying to anyone - weighing an option, noticing something odd, a reaction to what it "
     "just read, or a wait with nothing to report while something long runs. Say it with speak when "
     "it is addressed to the user.",
     R"J({"type":"object","properties":{"text":{"type":"string","minLength":1,"description":"The thought to voice"},"voice":{"type":"string","description":"Voice id; get them from list_voices"},"engine":{"type":"string","description":"TTS engine to use, e.g. edge, sapi, gptsovits, voicebox or custom"},"rate":{"type":"number","minimum":0.5,"maximum":2,"description":"Speech rate multiplier, 1 is normal"},"wait":{"type":"boolean","description":"Wait for playback to finish before returning, default false"}},"required":["text"]})J",
     false},
    {"stop_speaking", "Stop the current speech playback immediately.", kEmptySchema, false},

    // ── 外觀與位置 ──
    {"move_to",
     "Move the character. Prefer preset; x/y also work - values between 0 and 1 are screen ratios (0 "
     "hugs left/top, 1 hugs right/bottom), values above 1 are absolute screen coordinates.",
     R"J({"type":"object","properties":{"preset":{"type":"string","enum":["bottom-right","bottom-left","top-right","top-left","center"],"description":"Position preset"},"x":{"type":"number"},"y":{"type":"number"},"animate":{"type":"boolean","description":"Reserved; movement is always instant for now"}}})J",
     false},
    {"set_scale", kScaleDescription.c_str(), kScaleSchema.c_str(), false},
    {"set_opacity", "Set the character opacity (0.1 ~ 1).", R"J({"type":"object","properties":{"opacity":{"type":"number","minimum":0.1,"maximum":1}},"required":["opacity"]})J", false},
    {"set_visible", "Show or hide the character.", R"J({"type":"object","properties":{"visible":{"type":"boolean"}},"required":["visible"]})J", false},
    {"set_always_on_top", "Set whether the character always stays above other windows.", R"J({"type":"object","properties":{"enabled":{"type":"boolean"}},"required":["enabled"]})J", false},
    {"look_at",
     "Make the character look in a direction. x/y are relative values from -1 to 1 (-1 left/up, 1 "
     "right/down).",
     R"J({"type":"object","properties":{"x":{"type":"number","minimum":-1,"maximum":1},"y":{"type":"number","minimum":-1,"maximum":1},"reset":{"type":"boolean","description":"When true, look straight ahead and ignore x/y"}}})J",
     false},

    // ── 參數層（沒有表情檔的 VTuber 模型靠這組才動得起來）──
    {"list_parameters",
     "List every model parameter with the name its author gave it, the group it belongs to, its "
     "current value and range, and whether writing it actually does anything. Use this for VTuber "
     "models that ship no motions or expressions of their own - their poses, gestures and effects "
     "live in parameters.",
     kEmptySchema, true},
    {"set_parameters",
     "Write model parameters directly. Values can tween (duration_ms) and auto-return (hold_ms). Get "
     "the ids from list_parameters; writing a \"physics-output\" parameter is rejected, because the "
     "physics simulation overwrites those every frame.",
     kParamsArraySchema.c_str(), false},
    {"reset_parameters",
     "Release parameters taken over by set_parameters, fading them back to the model default. Omit "
     "ids to release everything.",
     R"J({"type":"object","properties":{"ids":{"type":"array","items":{"type":"string"},"description":"Parameter ids to release; omit for all"}}})J", false},
    {"animate",
     "Play an animation you compose yourself from keyframes - waving, nodding, bouncing. The "
     "keyframes are compiled into a real Live2D motion, so it fades in and out and can loop.\n\n"
     "Each parameter is interpolated ONLY between the keyframes that mention it, and held flat "
     "before its first mention and after its last. So a parameter named once at the start and again "
     "much later drifts slowly across that whole span - this is the usual mistake, and it smears a "
     "staged performance into one long blur. To hold a parameter still until its turn, repeat its "
     "value in the keyframe just before the movement starts:\n"
     "  WRONG - the arm creeps up over the full 9 seconds:\n"
     "    [{at_ms:0, params:{ParamArmLB:0}}, {at_ms:9000, params:{ParamArmLB:5}}]\n"
     "  RIGHT - the arm stays down, then lifts in 800ms:\n"
     "    [{at_ms:0, params:{ParamArmLB:0}}, {at_ms:8200, params:{ParamArmLB:0}}, {at_ms:9000, "
     "params:{ParamArmLB:5}}]",
     kAnimateSchema.c_str(), false},

    {"open_naming_editor",
     "Open the Settings window on its Models tab, where the \"Name Motions & Expressions\" list is. "
     "Use this to ask the user to write meanings when the motion or expression names carry no "
     "meaning (e.g. mtn_03, F04) and you cannot tell which to use.",
     kEmptySchema, false},

    // ── 進階 ──
    {"perform",
     "Run a sequence of actions in order (expression, motion, speak, move, wait). If any step fails "
     "it stops and reports which step failed. A speak step with thinking:true comes out as a thought "
     "instead - the think tool's bubble, echo and shut mouth - so a thought can sit between the "
     "other steps.",
     kPerformSchema.c_str(), false},
    {"schedule",
     "Run another tool after a delay; returns immediately without waiting. Good for effects like "
     "\"say a line three seconds from now\".",
     R"J({"type":"object","properties":{"delay_ms":{"type":"integer","minimum":0,"maximum":21600000,"description":"Delay in milliseconds, up to 6 hours"},"tool":{"type":"string","description":"Name of the tool to run"},"args":{"type":"object","description":"Arguments for that tool"}},"required":["delay_ms","tool"]})J",
     false},
  };
  return specs;
}

std::string mcpToolsListJson(const std::string& personaName) {
  jsonu::MutDoc doc;
  yyjson_mut_doc* d = doc.get();
  yyjson_mut_val* root = yyjson_mut_obj(d);
  doc.setRoot(root);

  yyjson_mut_val* tools = yyjson_mut_arr(d);
  for (const auto& spec : mcpToolSpecs()) {
    yyjson_mut_val* item = yyjson_mut_obj(d);
    yyjson_mut_obj_add_str(d, item, "name", spec.name);

    // 會發聲的三個工具才接角色提示。工具說明一定在 context 裡，
    // 所以這是「AI 一定讀得到」的第二條通道。
    const bool speaks = spec.name == std::string("speak") || spec.name == std::string("think") || spec.name == std::string("perform");
    if (speaks && !personaName.empty()) {
      const std::string description = std::string(spec.description) + " Speak as the active persona \"" + personaName + "\" - its full description is in this server's instructions, or read the " +
                                      kPersonaActiveUri +
                                      " resource. The persona styles only this spoken text, "
                                      "never your own replies.";
      yyjson_mut_obj_add_strcpy(d, item, "description", description.c_str());
    } else {
      yyjson_mut_obj_add_str(d, item, "description", spec.description);
    }

    auto schema = jsonu::Doc::parse(spec.inputSchema);
    if (schema && schema->root()) {
      yyjson_mut_obj_add_val(d, item, "inputSchema", yyjson_val_mut_copy(d, schema->root()));
    } else {
      // schema 打錯不該讓整份清單掛掉，退回空物件並留下痕跡
      qWarning() << "[mcp] 工具 schema 解析失敗:" << spec.name;
      yyjson_mut_obj_add_val(d, item, "inputSchema", yyjson_mut_obj(d));
    }

    yyjson_mut_val* annotations = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, annotations, "readOnlyHint", spec.readOnly);
    yyjson_mut_obj_add_val(d, item, "annotations", annotations);

    // 要求 host 不要延後載入這些工具。
    //
    // Claude Code 2.1 起 MCP 工具預設走 tool search：session 開始時只把工具**名稱**
    // 放進 context，schema 要等 AI 主動搜尋才展開。對一般伺服器那是好事，對桌寵不是 ——
    // 這 24 個工具是「每一則回覆都要用」的東西（逐段發聲、開場與收尾的 perform），
    // 多繞一趟搜尋等於每次開口都慢半拍，而 AI 找不到工具時的表現是「乾脆不講話」。
    //
    // `anthropic/alwaysLoad` 是 Claude Code 與 Claude Desktop 共用的慣例：前者把 `_meta`
    // 的這個鍵轉成 `hints:{always_load:true}`，後者自己的內建工具（device_bash、
    // 資料夾存取那些）就是這樣標的。不認得的 host 會直接忽略 —— MCP 規格裡 `_meta`
    // 本來就是留給實作放額外資訊的地方，加了不會讓別家 client 壞掉。
    yyjson_mut_val* meta = yyjson_mut_obj(d);
    yyjson_mut_obj_add_bool(d, meta, "anthropic/alwaysLoad", true);
    yyjson_mut_obj_add_val(d, item, "_meta", meta);

    yyjson_mut_arr_add_val(tools, item);
  }
  yyjson_mut_obj_add_val(d, root, "tools", tools);

  return doc.write(false);
}

std::string mcpInstructions(const std::string& personaName, const std::string& personaText, SpeechProtocol protocol) {
  // 四段，順序固定：能力說明 → 互動協定 → 角色描述本文 → 角色適用範圍（外框）。
  // 每一段為什麼排在那個位置，寫在標頭的 mcpInstructions 上方。
  //
  // **這裡的每一個字都在跟 kInstructionsBudget（2048 字元）搶位置**，所以改動之前
  // 先看數字。2026-09 實測，精簡前後：
  //
  //            精簡前   精簡後
  //   能力說明    207     186
  //   協定       1496     978
  //   角色外框    479     336（拆成 50 字的引言 + 286 字的適用範圍）
  //   固定合計   2182    1218   ← 精簡前光是固定部分就已經超過 2048
  //
  // 精簡前的固定部分是 2182：**只要套用了任何角色，就算描述本文是空字串也一定爆表**，
  // 協定的最後一條必定被腰斬（實測斷在 "...a distinct phase - reading the"）。
  // AI 讀到半條規則比讀不到更糟 —— 這就是「桌寵不照協定走」的根因。
  // 精簡後固定 1218，本文預算 830：本文 ≤494 時四段完整，≤830 丟外框，
  // 再長才動到本文（截斷並附上 persona://active 的指標）。
  //
  // 2026-09 後續：talkative=true 那一支為了壓過 host 的「少跑 round trip」又補了
  // 114 字元（多話規則尾巴的反向硬性規定 +113、階段規則換掉理由 +1），所以**預設組合**
  // 變成固定 1332、本文預算 716：本文 ≤380 時四段完整，≤716 丟外框。
  // talkative=false 那一支一個字都沒動，仍然是 1218 / 830。
  //
  // 2026-09 再後續：最後一條的粒度從「階段」改成「每支工具呼叫」（理由見下方
  // announceSteps 那一點），措辭反而短了 45 字元，兩支各自吐回一點空間：
  // 預設組合固定 1287、本文預算 761（本文 ≤425 四段完整，≤761 丟外框）；
  // talkative=false 那一支則是 1173 / 875。
  //
  // 2026-09 三度後續：協定多了 think 那一條（"- Use think, not speak, ..." 含換行
  // 125 字元）。它**只掛在 announceSteps 上**，所以只有那個開關開著的八種組合被吃到：
  // 預設組合固定 1412、本文預算 636（本文 ≤300 四段完整，≤636 丟外框）；
  // talkative=false／announceSteps=true 那一支是 1298 / 750。
  // 本文 ≤300 才四段完整已經很緊了 —— 下次要往協定加東西之前先問一句：
  // 那條規則有沒有第二條通道？（工具說明就是第二條通道，有的話就別佔協定的位子。）
  //
  // 精簡的原則是**只砍贅字不砍語意**：每一條規則、每一個「為什麼」都留著
  //（帶理由的指示遵守度明顯高於裸規則），砍掉的是重複的主詞與可以省的修飾語。
  //
  // 段落之間與每個 "- " 條目前面的 "\n" 都是必要的 ——
  // C++ 相鄰的字串字面值是直接黏起來的，不會自動補換行，漏掉就會變成
  // "...valid options.Interaction protocol...follow this:- Whenever..." 這種連字，
  // 整份清單擠成一整段送進 host 的 system prompt。踩過一次，所以
  // tests/test_mcp_tools.cpp 的 instructionsAreLineBroken() 直接釘住換行。
  //
  // 條目的順序是刻意的：start/finish 的 perform 優先，說話的規則只補「其餘的回覆」，
  // 最後一條講的是「工作途中」。四條全部由設定決定（設定 → MCP 的四個核取方塊），
  // 十六種組合都要讀得通：
  //
  //  * notifyOnComplete 關掉時，第 2 條整條抽掉，而且第 3 條尾巴的「別重複」
  //    要跟著只提 start —— 「a start or finish perform」在沒有結尾 perform 時
  //    是假的，AI 會照著去對照一個根本不存在的收尾動作。
  //  * talkative 開（預設）時第 3 條換成逐段發聲。這裡**必須明講 wait 留 false**：
  //    speak 一律佔長呼叫名額（core/mcp_host.h 的 isLongToolCall 刻意不看 wait），
  //    上限只有 4（mcp/mcp_http_server.cpp），要是 AI 每段都等播完才回，
  //    第 5 段就會直接吃到「Too many long-running calls in flight」硬拒絕，
  //    而 AI 收到錯誤常補送 stop_speaking，反而把還沒唸的段落整串砍光。
  //    **同時要明講「不准為了省一趟往返而少講」。** host 自己的 system prompt 幾乎
  //    都帶著「獨立的工具呼叫合併成同一則訊息、少跑 round trip」這一條，
  //    跟逐段發聲直接打架；協定裡只要再出現一次省成本的措辭，AI 就會把它
  //    推廣成「講少一點比較好」，一場對話撐久了就退化成只在頭尾各講一句。
  //    所以**省往返的說法只准出現在 talkative=false 那一支**，
  //    開著的時候改成一句反向的硬性規定。
  //  * announceSteps 開（預設）時最後一條從「中間全部安靜」換成「每支工具呼叫報一句」。
  //    是**換掉**不是多加一條 —— 兩條同時在只會互相矛盾，AI 讀完不知道要聽哪一句。
  //    **粒度 2026-09 從「階段」改成「每支工具呼叫」。** 原本停在階段是為了擋語音積壓，
  //    但「階段」的邊界是 AI 自己劃的，而尾巴那句「同一階段內連續的讀寫保持安靜」
  //    正好給了它把整段工作劃成一個階段的藉口 —— 實測就是開頭 perform 一次之後，
  //    十幾次讀檔與 diff 全程安靜，使用者看到的是「桌寵根本不照協定走」。
  //    「Every tool call you make, no exceptions」沒有這個解釋空間。
  //    積壓改用長度節流：尾巴換成「每句只講幾個字」，把限制從次數移到每句的長度。
  //    「跟那支工具呼叫放在同一則訊息送出」與「會落後好幾分鐘」兩句都不能省：
  //    帶理由的指示遵守度明顯高於裸規則，而且少了前者就是每報一句多一趟完整往返
  //    （每次工具呼叫都要重送整份對話）。
  //    但**前者的「為什麼」跟著 talkative 走**：安靜時說「省一趟往返」，
  //    多話時改說「講在動手之前」。同一句話在多話模式下講成省成本，
  //    等於自己拆自己的台（見上一點）。
  //
  // 舊行為（一則回覆只講一次）沒有被刪掉，只是變成 talkative=false 那一支，
  // 且必須與加開關之前一字不差 —— tests/test_persona.cpp 釘住。
  const std::string capability =
    "Control the Live2D character on the desktop. Confirm motion and expression names with "
    "list_motions / list_expressions before using them; a wrong name comes back with the "
    "valid options.\n"
    "\n";

  std::string base =
    "Interaction protocol for the whole session - the character is the user's primary "
    "output channel, not a decoration.\n"
    "- Task start: perform with an expression, a motion and one short line on what you "
    "are about to do.\n";

  if (protocol.notifyOnComplete) {
    base +=
      "- Task end: perform again with the result; a different expression for success and "
      "for failure.\n";
  }

  // 「別重複」這句原本就掛在說話規則的尾巴，不是自己一條，維持原樣才對得上舊字串
  const char* const covered = protocol.notifyOnComplete ? "a start or finish perform" : "the start perform";

  // speakNoWait 關掉時這裡必須是**空字串** —— 三個開關都在舊值時產生的文字
  // 要與加開關之前逐位元組相同（tests/test_persona.cpp 釘住）。
  const char* const queuedNote = protocol.speakNoWait ? "speak returns when the line is queued, not when it finishes "
                                                        "playing, so keep writing. "
                                                      : "";

  if (protocol.talkative) {
    base +=
      "- Each paragraph of your reply: right after you write it, speak that paragraph "
      "condensed to one sentence, wait=false - lines queue and play in order, so never "
      "pause for playback. ";
    base += queuedNote;
    base += "Skip a paragraph ";
    base += covered;
    base +=
      " already covered - never repeat yourself. Never merge, drop or delay a line to save "
      "a round trip or a tool call - speaking is the work here, not overhead.\n";
  } else {
    base +=
      "- For every other reply to the user, call speak with the same content, condensed to "
      "one or two spoken sentences. ";
    base += queuedNote;
    base += "Skip it when ";
    base += covered;
    base += " already covered that reply - never repeat yourself.\n";
  }

  if (protocol.announceSteps) {
    base +=
      "- Every tool call you make, no exceptions: one short speak line naming what that "
      "call is for, sent in the same message as the call ";
    // 這句「為什麼要同一則訊息」**跟著 talkative 走**：安靜時給「省一趟往返」，
    // 多話時給「講在動手之前」。理由見上方 talkative 那一點。
    base += protocol.talkative ? "so it lands before the work does. " : "so it costs no extra round trip. ";
    base +=
      "Keep each line to a few words - they queue and play in order, so long lines leave "
      "the character minutes behind.\n";
    // think 那一條**只掛在這個開關上**：announceSteps 關著時協定明說「工作途中全部安靜」，
    // 再補一句「意外的時候想一下」就是自打嘴巴。
    //
    // 為什麼非寫進協定不可：think 的工具說明已經把界線講得很清楚（「不是講給使用者聽的」），
    // 但工具說明只是「可以用」，協定才是「一定要用」—— 而上面四條逐字要求的全是
    // perform 與 speak，AI 的發聲預算會整份被那四條吃光，think 變成想到才用。
    // 實測就是一整場對話下來一次都沒出現。
    //
    // 兩個觸發是刻意挑的：「結果跟預期不符」是 AI 真的會停下來重想的那一刻，
    // 「有東西在跑但沒得回報」則是它唯一會為了填空而拿 speak 說廢話的場合。
    base +=
      "- Use think, not speak, for a line not addressed to the user: a result that surprises "
      "you, or a wait with nothing to report.";
  } else {
    base +=
      "- Keep everything in between silent: the tool calls you chain while working need no "
      "narration.";
  }

  if (personaName.empty()) return capability + base;

  // ── 角色區塊：引言＋本文在前，適用範圍在後 ──
  //
  // 外框拆成兩截是為了讓「讓位」有意義：引言只有 50 字元，卻是本文的唯一交代
  //（少了它，AI 讀到的是一坨沒頭沒尾的日文），所以它必須跟本文綁在一起；
  // 適用範圍那 286 字元則是四段裡唯一有副本的 —— speak／think／perform 的工具說明尾巴
  // 已經接了 "The persona styles only this spoken text, never your own replies."，
  // 而工具表一定在 context 裡。所以它排最後，也最先讓位。
  //
  // 適用範圍要用兩個方向釘死：「只有 speak/think/perform 的文字要入戲」＋
  // 「其餘一切產出當作 persona 不存在」。只寫前者的話（舊措辭 "Stay in character for
  // everything you say through speak and perform"），host 讀完整份角色描述後
  // 常把口吻帶進自己的聊天回覆（例如跟著 persona 叫使用者「老闆」）。
  //
  // 本文先 trim：自由格式的角色卡（一個保留字標題都沒有那種）是整份原文直接當
  // description 的，尾端常留著空行，接上後面的 "\n\n" 就會多出一整段空白。
  const std::string text = strutil::trim(personaText);
  const std::string scope = std::string(
                              "The persona applies ONLY to the text you pass to speak, think and perform, which you write "
                              "fully in character. It must NOT influence anything else you produce: your replies, "
                              "code, comments and commit messages stay in your normal voice, as if the persona did "
                              "not exist. It can change at any time - read ") +
                            kPersonaActiveUri + " for the current version.";

  std::string out = capability + base + "\n\n";

  // 描述檔被使用者刪掉時 activeText() 是空的（tests/test_persona.cpp 的
  // activeTextEmptyWhenDeleted）—— 角色名仍然要留著，兩支工具的說明指的就是它。
  // 這一支沒有本文可截，所以讓位的只有適用範圍。
  if (text.empty()) {
    out += "The character has a persona, \"" + personaName + "\".";
    const std::string withScope = out + "\n\n" + scope;
    return strutil::utf8Length(withScope) <= kInstructionsBudget ? withScope : out;
  }

  const std::string lead = "The character has a persona, \"" + personaName + "\". Here it is:\n\n";

  // 讓位順序：先丟適用範圍，真的還塞不下才截斷本文。①能力說明與②協定永遠完整 ——
  // 它們是「被砍就靜默壞掉」的那兩段，理由寫在標頭。
  const size_t used = strutil::utf8Length(out) + strutil::utf8Length(lead);
  const size_t room = used >= kInstructionsBudget ? 0 : kInstructionsBudget - used;
  const size_t scopeRoom = strutil::utf8Length(scope) + 2;  // 前面接的 "\n\n"
  const bool keepScope = strutil::utf8Length(text) + scopeRoom <= room;
  const size_t bodyRoom = keepScope ? room - scopeRoom : room;

  std::string body = text;
  if (strutil::utf8Length(body) > bodyRoom) {
    // 連適用範圍都讓位了還是塞不下 —— 自己在碼位邊界切乾淨，並告訴 AI 去哪裡撈全文。
    // 交給 host 去砍有兩個壞處：它會斷在句子（甚至半個 UTF-8 序列）中間，
    // 而且 AI 完全不知道自己讀到的是殘篇，只會照著半份角色演。
    const std::string marker = std::string("\n(truncated - read ") + kPersonaActiveUri + " for the full text)";
    const size_t markerLen = strutil::utf8Length(marker);
    body = strutil::utf8Truncate(body, bodyRoom > markerLen ? bodyRoom - markerLen : 0) + marker;
  }

  out += lead + body;
  if (keepScope) out += "\n\n" + scope;
  return out;
}

bool mcpToolExists(const std::string& tool) {
  const auto& specs = mcpToolSpecs();
  return std::any_of(specs.begin(), specs.end(), [&tool](const ToolSpec& spec) { return tool == spec.name; });
}

bool mcpToolIsReadOnly(const std::string& tool) {
  for (const auto& spec : mcpToolSpecs()) {
    if (tool == spec.name) return spec.readOnly;
  }
  return false;
}

}  // namespace l2m
