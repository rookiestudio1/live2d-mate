#pragma once

// 自主行為的開關閘門：把「桌寵可不可以自己做這件事」從一堆 config 欄位
// 收斂成一個布林。
//
// 為什麼要一支純函式，而不是在 AppController 就地寫條件式：同一條規則要在
// **相隔一整趟網路往返**的兩個地方各判一次 —— 規劃前（算成
// `IdleWorld::canMove` 餵給規劃器，決定要不要產生 move 步驟）與執行前
// （步驟真的要跑的那一刻）。寫兩份字面量遲早會漂移，而 `AppController`
// 連不進 `l2m_core`，真值表一格都測不到。
//
// 踩過的坑：LLM 行為大腦原本只把 `can_move` 寫進 prompt 拿自然語言拜託模型
// 不要動，程式碼一層都沒擋 —— 於是「隨機移動」明明關著，桌寵還是自己散步
// （規則版 `IdleDirector` 從第一天就守著 `world.canMove`，只有 LLM 那條路沒有）。

#include "config_schema.h"

namespace l2m {

// 自主表演能不能移動視窗。三個開關**全部**成立才算數：
//  * `autonomy.move` —— 使用者明確要不要「隨機移動」（預設關）
//  * `!interaction.lockPosition` —— 位置鎖著時誰都不准動它
//  * `interaction.dragMove` —— 連手拖都不給拖的人，更不會想要它自己走
// **不管 MCP**：AI 明確下的 `move_to` / `perform` 是使用者的 AI 主動要求，
// 不是桌寵的自主行為，走 `AppController::moveTo()` 不經過這裡。
bool autonomousMoveAllowed(const AppConfig& config);

}  // namespace l2m
