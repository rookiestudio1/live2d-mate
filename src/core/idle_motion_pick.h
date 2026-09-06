#pragma once

// 「一段動作播完之後，該自動接回哪一段」的挑選規則。
//
// 為什麼需要：`ModelController::update()` 在動作佇列空掉時會自動接上待機動作，
// 原本寫死 `startMotion("Idle", ...)`。但 `Idle` 這個群組名是官方範例的慣例，
// 不是 model3.json 的規格 —— 實測碧藍航線那批模型把全部 15 支動作塞在**同一個
// 空字串群組**裡（`"Motions": { "": [ {"File":"motions/home.motion3.json"}, … ] }`），
// 待機動畫只以檔名 `idle.motion3.json` 存在，`GetMotionCount("Idle")` 永遠是 0。
// 於是那條 fallback 每幀都失敗，動作播完之後**再也沒有任何動作在驅動模型**。
//
// 這件事單獨看只是「不會自己動」，配上 `restoreBaseline()` 就會變成看得見的 bug：
// 還原的目標是 moc3 的預設參數值，而那是作者的編輯狀態，常常是「所有變體一起開著」。
// 實測碧藍航線 40 隻模型有 13 隻（33%）的預設可見 drawable 數量多於任何一支動作 ——
// yichui_2 的預設同時開著 `foot`/`foot2`/`foot_r`/`foot_r3`（**4 隻腳**）、
// aidang_2 同時開著 `arm_l1..l8` 與兩組 `hand_r`（**4 隻手**）。
// 動作播完 → 還原成 4 隻腳 → 沒有待機動作接手 → 就這樣定格在畫面上。
//
// 所以規則要能認出「檔名叫 idle 但群組名不是 Idle」的那一類。順序刻意是
// 「兩種精確比對優先，再兩種模糊比對」：`touch_idle`（點擊的待機變體）這種名字
// 不該贏過一個貨真價實的 `idle.motion3.json`。
//
// 純函式，不碰 Cubism 也不碰檔案系統：呼叫端（`ModelController`）把
// `ICubismModelSetting` 攤成群組名與檔名就好。

#include <optional>
#include <string>
#include <vector>

namespace l2m {

// model3.json 宣告的一個動作群組。files 的索引就是 `GetMotionFileName` 的索引。
struct MotionGroupFiles {
  std::string name;
  std::vector<std::string> files;
};

// 挑到的待機動作。index < 0 代表「這個群組裡隨機挑一個」——
// 群組名本身就叫 Idle 時，裡面每一支都是待機動畫，維持既有的隨機行為。
struct IdleMotionSlot {
  std::string group;
  int index = -1;
};

// 挑一支當待機動作，挑不出來回 nullopt（呼叫端此時**刻意什麼都不做**，
// 見 `ModelController::startIdleMotion()`：接不上待機還硬還原 baseline，
// 只會把模型丟回上面說的那個編輯狀態）。
//
// 比對順序：
//   1. 群組名等於 `Idle`（不分大小寫）
//   2. 某支動作的檔名主檔名等於 `idle`
//   3. 群組名**含** `idle`
//   4. 某支動作的檔名主檔名**含** `idle`
// 空群組（files 是空的）一律略過。
std::optional<IdleMotionSlot> pickIdleMotion(const std::vector<MotionGroupFiles>& groups);

}  // namespace l2m
