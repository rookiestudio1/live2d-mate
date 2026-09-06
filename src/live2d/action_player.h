#pragma once

// 「一個名稱」→「對 ModelController 與 ParameterOverlay 的實際呼叫」。
//
// 為什麼要獨立成一層：一個動作／表情名稱可能有三種來源，分支規則一點都不明顯 ——
//
//   1. 模型自己的（model3.json 的 Motions／Expressions）→ 走 Cubism 的動作／表情管理
//   2. core/builtin_actions.h 合成的內建項目 → 動作走 buildMotion3 + playSynthesizedMotion，
//      表情走 ParameterOverlay（模型裡根本沒有這個群組，走 startMotion 會被
//      GetMotionCount 當場回絕而靜默沒反應）
//   3. 臉部追蹤模型的「虛擬表情」（其實是一個參數開關）→ 也走 ParameterOverlay
//
// 而且切換時要收乾淨：從 sleepy 切回模型自己的表情，得先把內建表情寫進覆寫層的
// 那雙半睜的眼睛放掉，否則會疊在新表情上。
//
// 這一層刻意**不做名稱解析**（那是 core/model_commands.h 的 resolveMotion／
// resolveExpression，純邏輯、可測），也不回 CommandResult、不碰閒置倒數與
// hold 計時器 —— 那些是桌寵的產品行為，Viewer 不需要。
// 呼叫端：src/app/app_controller.cpp（桌寵）與 src/viewer/（Live2D Viewer）。

#include <optional>
#include <string>

#include "core/model_types.h"

namespace l2m {

class ModelController;
class ParameterOverlay;

// 播放 group 這個動作群組（index < 0 代表群組內隨機）。
// group 必須是已經解析過的真實群組名（含內建動作名）。
// 回 false ＝ 沒播起來（優先權被擋下、群組不存在、內建動作編譯失敗）。
//
// loop：要不要一直重播（Live2D Viewer 的「自動重播」）。內建動作本來就有自己的
// loop 設定（只有 doze 是循環的），兩邊取聯集 —— 這個開關只能多加循環，
// 不會把本來該循環的內建動作關掉。
bool playMotionByName(ModelController& controller, const ModelInfo& model, const std::string& group, int index, int priority, bool loop = false);

// 套用 name 這個表情；nullopt 代表清除表情。
// name 必須是已經解析過的真實表情名（含內建表情名）。
// 回 false ＝ 模型的 .exp3.json 裡沒有這個表情（其餘路徑一律成功）。
bool applyExpressionByName(ModelController& controller, ParameterOverlay& overlay, const ModelInfo& model, const std::optional<std::string>& name);

}  // namespace l2m
