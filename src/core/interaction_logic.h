#pragma once

// 互動的純邏輯部分（視窗與滑鼠事件的綁定在 UI 層；這裡只放可單測的規則）。

#include <optional>
#include <string>
#include <vector>

#include "hit_area_semantics.h"
#include "model_types.h"

namespace l2m {

// 判定為「點擊」而非拖曳的最大位移（px）
inline constexpr double kTapThresholdPx = 5;

// 點擊時要播的那一段動作。
// index < 0 代表「整個群組」，播的時候由 ModelController 隨機挑一段。
struct TapMotionPick {
  std::string group;
  int index = -1;

  bool operator==(const TapMotionPick& other) const { return group == other.group && index == other.index; }
};

// 挑選點擊時要播的動作。找不到回 nullopt，交給呼叫端決定（桌寵與 Viewer 都是
// 退回隨機動作 —— 點了完全沒反應比播錯一段還糟）。
//
// **為什麼要連「動作檔名」一起比對**：Cubism 規格裡動作是掛在群組底下的，
// 官方模型也確實會取 `TapHead` 這種群組名。但實測手邊 2956 個 model3.json，
// 有 1535 個（52%）把**全部動作塞在同一個空字串群組**裡：
//
//   "Motions": { "": [ {"File":"motions/touch_head.motion3.json"}, ... ] }
//
// 這種模型的 `touch_head` 只存在於檔名，只看群組名的話一個都對不上。
// `MotionGroupInfo::files` 掃描時本來就填好了，比對它就同時吃得下兩種寫法
// （另外 215 個是把 touch_head 取成群組名的正規寫法）。
//
// 挑選順序（先到先得）：
//   1. 作者標的 HitArea 名稱：`tap`/`touch` + 區域名、區域名本身，再退回包含關係。
//      規格上最準，但只有 1.1% 的模型有填。
//   2. 部位關鍵字：在「觸摸池」裡找含 head／body／chest… 的那一段。
//      部位來自 HitArea，或（絕大多數情況）core/model_regions.h 的幾何推算。
//   3. 觸摸池裡不含 idle／drag 的任一段 —— `touch_idle*`（待機變體）與
//      `touch_drag*`（拖曳反應）不是點擊該播的東西，要留到最後才考慮。
//   4. 觸摸池全部。
//
// 「觸摸池」＝群組名或檔名含 touch／tap／pat／poke／触／觸／撫 的動作。
// 實測整個語料庫沒有任何一個非觸摸動作誤入（`tap1`、`tapchest`、`taphead`…
// 全是真的觸摸動作，`pat`／`poke` 則一次都沒有誤命中）。
//
// 沒有 `files` 的群組一律略過 —— 那是 core/builtin_actions.h 合成出來的內建
// 動作，model3.json 裡根本不存在這個群組，挑到了也只會被 GetMotionCount 回絕。
std::optional<TapMotionPick> pickTapMotion(const std::vector<std::string>& areas, BodyPart part, const std::vector<MotionGroupInfo>& motions);

}  // namespace l2m
