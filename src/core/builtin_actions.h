#pragma once

// 內建動作與表情：模型沒做、但用參數就合成得出來的那幾個。
//
// 為什麼需要這一層：實務上大多數模型只綁了一個 Idle 動作（VTube Studio 出身的
// 尤其如此 —— 語意全藏在參數裡），list_motions 給 AI 看的就是一行 Idle，
// 想叫它揮個手、眨個眼根本無從叫起。而「揮手」「點頭」「微笑」這種通用動作
// 其實只是幾個參數的關鍵影格，模型作者做不做都一樣算得出來。
//
// 這裡只做**決策**：這個模型的參數表撐不撐得起某個內建項目、撐得起的話要寫哪些
// 參數。實際播放交給既有的兩條路 —— 動作走 buildMotion3 + playSynthesizedMotion
//（跟 MCP 的 animate 同一條），表情走 ParameterOverlay。磁碟上一個位元組都不動。
//
// ── 槽位（slot）為什麼要有 ───────────────────────────────
// 「揮手」在 Cubism 官方範例模型上是 ParamArmRA，在使用者手上那隻魔女上卻是
// Param28（作者在 cdi3 取名「招手1」）。內建表寫死參數 id 就只有官方模型能用。
// 所以中間隔一層槽位：內建表用邏輯角色（ArmWave、EyeLOpen…）描述動作，
// resolveSlots() 再依「標準 id 優先 → cdi3 名稱關鍵字補救」把角色對到真實 id。
// 關鍵字刻意保守（例如只收「招手／揮手／wave」，不收單一個「手」），理由跟
// display_info.h 的 expressionsFromDisplayInfo 一樣：認錯了會在 list_motions
// 裡塞一堆按了看不出差別的項目，AI 只會更難挑。
//
// ── 數值一律絕對值，靠 Cubism 自己夾 ─────────────────────
// 掃描期拿不到參數的上下限（範圍在 moc3 裡，cdi3 不帶），所以揮手就直接寫 ±30：
// 模型範圍是 -10..10 就夾成滿舵，是 -1..1 也還是滿舵，結果都對。
// ParameterOverlay::set 與 CubismMotion 兩條路都會夾。
//
// ── 已知限制 ────────────────────────────────────────────
// 沒有 cdi3.json 的模型拿不到參數表（describeParameters 回空陣列），
// 因此一個內建項目都不會有。這是掃描期的硬限制，不是 bug。

#include <optional>
#include <string>
#include <vector>

#include "model_types.h"
#include "motion_builder.h"
#include "parameter_tracks.h"

namespace l2m {

// 一個這個模型撐得起來的內建項目
struct BuiltinActionInfo {
  // 餵給 AI 與 play_motion／set_expression 的名稱，例如 "wave"。
  // 一律小寫英文：它同時是 MCP 介面的一部分。
  std::string name;
  // true = 動作（關鍵影格，走 play_motion）；false = 表情（靜態參數，走 set_expression）
  bool motion = false;
  // 實際對應到的參數 id，依槽位在內建表裡的順序。設定畫面拿它顯示「這隻模型
  // 是用哪幾個參數做出來的」—— 關鍵字比對有猜的成分，猜錯時使用者要看得見。
  std::vector<std::string> params;

  bool operator==(const BuiltinActionInfo& other) const { return name == other.name && motion == other.motion && params == other.params; }
};

// 內建動作編出來的關鍵影格，交給 buildMotion3
struct BuiltinMotion {
  std::vector<Keyframe> keyframes;
  BuildMotionOptions options;
  // 這幾個參數是這隻模型的**物理輸出**，動作寫在物理之前會被整個蓋掉。
  // 播放端要把它們原樣交給 ModelController::playSynthesizedMotion 的
  // carryPastPhysics，值才會在物理之後再寫一次。
  //
  // 為什麼要有這條路：實務上「身體上下彈跳」沒有現成參數可以直接動 ——
  // 很多模型（例如 March 7th）的 ParamBodyAngleY 是用 ParamAngleY 經物理算出來的，
  // 所以身體會跟著頭一起動，想要「身體在跳、頭不動」在動作那條路上完全做不到。
  // 越過物理之後就做得到了：動作直接寫身體，頭一格都不碰。
  //
  // 第二種用途是頭部角度：有些模型（ariu）把頭的慣性做成 ParamAngle* 自我回授的物理
  //（同時是 Input 也是 Output），不收的話 nod／shake／tilt 那一批保底動作會整個消失。
  // 頭部角度這條路有一個刻意保留的副作用：越過物理的晚寫是 SetParameterValue（覆蓋），
  // 而視線加成與拖曳搖晃都是寫在同一組 ParamAngle* 上的 AddParameterValue ——
  // 所以在那一類模型上，nod／shake／tilt／look_away／sigh／doze 播放期間頭不跟游標
  //（doze 是循環動作，等於整段打瞌睡都不看人）。身體角度那條沒有這個問題，
  // ParamBodyAngle* 上本來就沒有視線加成。留著：那六個一秒多就結束，而打瞌睡不看人是對的。
  std::vector<std::string> carryPastPhysics;
};

// 套用一個內建表情要對 ParameterOverlay 做的兩件事。
//
// 為什麼要分成 set 與 release 兩袋：內建表情之間是互斥的，切走時得把上一個用到、
// 新的沒用到的參數收回去。virtualExpressionParams 那邊是「其他的一律寫 0」，
// 這裡不能照抄 —— ParamEyeLOpen 的靜止值是 1 不是 0，寫 0 會讓眼睛永遠閉著。
// release 走 ParameterTracks 的淡回 base，回到的是模型自己當下該有的值。
struct BuiltinExpressionApply {
  std::vector<SetParameterRequest> set;
  std::vector<std::string> release;
};

// 這個模型的參數表撐得起哪幾個內建項目。順序固定（內建表的順序），
// 撞名（模型自己已經有同名動作群組／表情）由呼叫端負責排除。
std::vector<BuiltinActionInfo> availableBuiltinActions(const std::vector<ParameterInfo>& params);

// 這個名稱是不是內建動作／表情（不管這個模型撐不撐得起來）
bool isBuiltinActionName(const std::string& name);

// 切換到 name 這個內建表情要做的事；name 為 nullopt 代表清除全部內建表情。
// name 不是內建表情時等同 nullopt。
BuiltinExpressionApply applyBuiltinExpression(const std::vector<ParameterInfo>& params, const std::optional<std::string>& name);

// 內建動作的關鍵影格。name 不是內建動作、或這個模型撐不起來時回 nullopt。
std::optional<BuiltinMotion> builtinMotionFor(const std::vector<ParameterInfo>& params, const std::string& name);

// 把掃描時合成的內建動作／表情整組從一份 ModelInfo 裡拿掉，只留模型作者做的東西
//（motions／expressions 各自濾掉，builtinMotions／builtinExpressions 清空）。
//
// 存在的理由是 Live2D Viewer：內建項目是**桌寵餵給 AI 的介面**（讓只綁一個 Idle
// 的模型也叫得動「揮手」），檢視器的用途卻是看這隻模型到底做了什麼，混進十來個
// 合成項目只會讓人分不清哪些是作者做的。掃描端不動：describeModel 與 scanModels
// 一字不差是它的合約（tests/test_model_scanner.cpp 釘住），要不要留由呼叫端決定。
//
// 名稱比對與 model_scanner.cpp 附加時同源 —— 那裡撞名一律跳過，所以登記在
// builtinMotions／builtinExpressions 裡的名字必定就是合成出來的那一個。
void removeBuiltinActions(ModelInfo& model);

}  // namespace l2m
