#pragma once

// LLM 行為大腦的 prompt 組裝與回應解析。
//
// 這一層是「IdleWorld → 訊息陣列」與「LLM 回覆文字 → PerformStep」的純函式，
// 不碰網路 —— 非同步橋接在 src/llm/llm_behavior_planner.h。放 core 的理由：
// prompt 少帶一個欄位（例如 annotations 的語意）症狀是「LLM 亂選動作」，
// 只有測試釘得住；回應解析踩到 code fence、多包一層物件、非法 action
// 都是實際會發生的事，錯了就是整輪表演消失。
//
// 輸出格式刻意就是 MCP perform 的同一份 schema（core/perform_step.h 檔頭
// 從第一天就這麼承諾）：解析直接重用 parsePerformSteps，同一套驗證、
// 同一個 kMaxPerformSteps 上限。解析失敗回 nullopt —— 呼叫端據此退回
// IdleDirector，桌寵永遠不因 LLM 的怪輸出而僵住。

#include <optional>
#include <string>
#include <vector>

#include "day_period.h"
#include "idle_director.h"
#include "llm_types.h"
#include "perform_step.h"

namespace l2m {

// speak 步驟的單句上限沿用台詞區的規則（persona_doc.h）：再長氣泡撐不下
inline constexpr size_t kLlmSpeakMaxChars = 200;

struct LlmBehaviorPromptInput {
  IdleWorld world;
  IdlePlanOptions options;
  // 角色 .md 的 # Character Description（唯一送給 AI 的一區；persona_doc.h）
  std::string personaDescription;
  // 現在時刻（0~23），組進 context 讓「早安」不會出現在半夜
  int hour = 12;
  // 長期記憶（memory/<角色>.md，已由 clampPersonaMemory 夾過；core/persona_memory.h）。
  // 空字串＝沒有記憶檔，prompt 整段省略。
  std::string memory;
  // 最近幾輪 LLM 自己講過的台詞（llm_behavior_planner 記在記憶體）。
  // 無狀態 API 不會記得上一輪，不餵回去的話隔幾輪就寫出意思幾乎一樣的句子。
  std::vector<std::string> recentLines;
  // 現在的天氣（core/weather_alert.h 的 weatherContextLine，已經是一行英文）。
  // **每一輪都帶，不只預警時** —— 使用者要的「天氣當背景知識」就是這一格：
  // 角色本來就要講話時自然帶到，而不是為了報天氣特地開口。
  // 空字串＝沒有天氣資料，prompt 整段省略（不能讓 LLM 拿全零的資料編天氣）。
  std::string weather;
  // occasion == "weatherAlert" 時這一則預警的英文摘要（WeatherAlert::summary）。
  // 與 weather 分開：背景那一行是「現在怎樣」，這一行是「馬上要發生什麼」，
  // 合成一段會讓模型分不清該提醒哪一件事。
  std::string weatherAlert;
};

// 組出 system + user 兩則訊息。system 是固定的角色扮演指令與輸出格式，
// user 是這一輪的 context（occasion、閒置分級、心情、可用動作與表情含命名語意…）。
std::vector<LlmMessage> buildBehaviorPromptMessages(const LlmBehaviorPromptInput& input);

// 解析 LLM 回覆成表演步驟。
//  * 先剝 markdown code fence（```json … ```）。
//  * 接受裸陣列或 {"steps":[…]} 兩種形狀。
//  * 走 parsePerformSteps 的同一套驗證；之後再過濾：只留
//    motion / expression / speak / move / wait（parameters 與 animate 不開放給
//    自主表演 —— 亂寫參數會讓角色姿勢壞掉，而且閒置復原收不乾淨）。
//  * allowMove（＝規劃當下的 IdleWorld::canMove）為 false 時 move 步驟整個丟掉。
//    prompt 裡的 can_move 只是拿自然語言拜託模型，小模型照樣吐 move —— 擋得住的
//    只有程式碼。那個旗標來自 core/autonomy_gates.h 的 autonomousMoveAllowed()
//    （autonomy.move／!lockPosition／dragMove 三者皆備），漏掉的症狀是
//    「隨機移動明明關著（甚至位置鎖著），桌寵還是自己散步」。
//    這是**第一層**，而且是送出當下的快照：回覆落地時設定可能已經被改掉，
//    所以 AppController::runAutonomous() 執行前還會現讀同一支閘門再擋一次。
//  * speak 文字超過 kLlmSpeakMaxChars 的步驟整個丟掉（截半句話更怪）。
//  * expression 補上與 IdleDirector 相同的自動退回（holdMs）；
//    move 補上滑行（glideMs），閒置散步不該瞬移。
// 整包解不開（不是 JSON、schema 錯）回 nullopt 並在 error 放原因。
std::optional<std::vector<PerformStep>> parseBehaviorSteps(const std::string& responseText, bool allowMove, std::string* error);

}  // namespace l2m
