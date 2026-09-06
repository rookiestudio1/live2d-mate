#pragma once

// 模型命令的純邏輯層：解析名稱、產生提示、驗證參數、組報告。
//
// 為什麼要獨立成一個模組：AppController 綁著視窗與 Cubism 執行期，測不動；
// 但這些「找不到 X 時該回什麼 hint」的規則才是 MCP 介面的真正產品面，
// 必須逐字釘住。抽到 l2m_core 之後就能用 QTest 一比一驗。
//
// 所有面向使用者／AI 的字串一律英文：它們同時是 MCP 工具的輸出內容。

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "command_result.h"
#include "model_types.h"
#include "parameter_tracks.h"

namespace l2m {

// 虛擬表情的切換時間。瞬切太生硬，跟 .exp3.json 的淡入感覺差太多。
inline constexpr double kExpressionFadeMs = 150;

// ── 模型 ────────────────────────────────────────────────

// 依 id（相對路徑）或顯示名稱找模型，名稱比對不分大小寫；找不到回 nullptr。
// 順序：id 全等 → 名稱全等 → 名稱包含。
const ModelInfo* resolveModel(const std::vector<ModelInfo>& models, const std::string& idOrName);

// 「找不到模型」時附的提示
std::string modelListHint(const std::vector<ModelInfo>& models, const std::string& modelsDir);

// ── 使用者命名 ──────────────────────────────────────────

// 依使用者寫的意義找出對應的鍵。
// 先找完全相同的，再找互相包含的 —— AI 講「開心」也能命中「開心地揮手」。
std::optional<std::string> matchByMeaning(const std::map<std::string, std::string>& meanings, const std::string& query);

// ── 提示文字 ────────────────────────────────────────────

// 動作清單的提示，有命名的話一起附上，AI 才知道可以怎麼指定
std::string motionHint(const ModelInfo& model);
std::string expressionHint(const ModelInfo& model);

// 參數清單的提示。上百個參數全列出來太吵，只挑名字最接近的幾個。
std::string parameterHint(const ModelInfo& model, const std::vector<std::string>& wanted);

// ── 解析 ────────────────────────────────────────────────

struct ResolvedMotion {
  std::string group;
  // 沒有指定索引時為 nullopt（代表群組內隨機）
  std::optional<int> index;
};

// 把 AI 給的群組名／意義解析成真正的群組與索引。失敗時回傳帶 hint 的錯誤。
CommandResult resolveMotion(const ModelInfo& model, const std::string& group, std::optional<int> index, ResolvedMotion* out);

// 把 AI 給的表情名／意義解析成真正的表情名。失敗時回傳帶 hint 的錯誤。
CommandResult resolveExpression(const ModelInfo& model, const std::string& name, std::string* out);

// 這個模型的「表情」其實是參數開關時，把套用某個表情翻成一批參數寫入。
// 表情互斥：套一個就把同一批的其他個歸零，行為跟 .exp3.json 一致。
// name 為 nullopt 代表全部歸零（清除表情）。
std::vector<SetParameterRequest> virtualExpressionParams(const ModelInfo& model, const std::optional<std::string>& name);

// ── 驗證 ────────────────────────────────────────────────

// 擋掉兩種一定沒效果的要求：模型根本沒有的 id，以及物理演算的輸出參數
// —— 後者寫進去每一幀都會被 physics 蓋掉，不講清楚 AI 只會一直重試。
CommandResult validateSetParameters(const ModelInfo& model, const std::vector<SetParameterRequest>& requests);

// animate 的關鍵影格用到的參數 id（已去重）
CommandResult validateAnimateParams(const ModelInfo& model, const std::vector<std::string>& used);

// keyframes 裡出現過的參數 id，依首次出現順序去重
std::vector<std::string> uniqueKeyframeParams(const std::vector<std::map<std::string, double>>& frameParams);

// ── 報告 ────────────────────────────────────────────────

// cdi3 的名稱與物理角色（掃描時就有）＋ Cubism 執行期的現值與上下限，
// 合成一份給 AI，省得它自己對照兩張表。
// snapshots 缺的項目刻意不補 0 —— 那會讓 AI 以為讀到了真的現值。
std::string buildParameterReportJson(const ModelInfo& model, bool supported, const std::vector<ParameterSnapshot>& snapshots, const std::vector<std::string>& overridden);

// list_models / list_motions / list_expressions / list_parameters 的 JSON 輸出
std::string modelListJson(const std::vector<ModelInfo>& models, const std::string& currentModelId);
std::string motionListJson(const ModelInfo& model);
std::string expressionListJson(const ModelInfo& model);

// 角色名稱（給 list_parameters 與報告用）
const char* roleName(ParameterRole role);

}  // namespace l2m
