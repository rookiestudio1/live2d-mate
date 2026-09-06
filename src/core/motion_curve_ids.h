#pragma once

// 「這支 motion3.json 會驅動哪些參數／部件」的清單。
//
// 為什麼需要：`ModelController` 在起播新動作前會 `restoreBaseline()`，把參數與
// part 不透明度清回 moc3 的預設值，用意是清掉上一段動作留下的道具／特效殘值。
// 問題出在還原的目標 —— moc3 的預設值是作者的**編輯狀態**，很多模型在那個狀態下
// 「所有變體一起開著」。實測碧藍航線 40 隻裡有 13 隻（33%）如此：yichui_2 的預設
// 同時開著 `foot`/`foot2`/`foot_r`/`foot_r3`（4 隻腳）、aidang_2 同時開著兩組手臂與
// 三隻右手（4 隻手）。動作的淡入預設是 1 秒，於是**每一段動作的開頭一秒**都會
// 實實在在看到那些多出來的手腳，從預設狀態淡到動作指定的那一組。
//
// 修法是「即將起播的那支動作自己會驅動的 id 不要還原」：那些值本來就會被新動作
// 接手，讓它從上一段動作的值直接淡到目標值就好（這也才是 Cubism 原本的交叉淡入
// 行為）；新動作**不管**的參數才是真正的殘值，照樣清回預設。
//
// 為什麼是純函式：判斷的依據只有 motion3.json 的 `Curves[].Target` / `Curves[].Id`
// 兩個欄位，錯一格的後果是「該清的沒清」或「不該清的清了」—— 前者是道具留在畫面上、
// 後者就是上面那個 4 隻腳，兩種都沒有錯誤訊息可看，只能靠測試釘住。
//
// `Target` 為 `Model` 的曲線（`LipSync`／`EyeBlink` 這種效果曲線）刻意不收：
// 它驅動的是呼叫端另外指定的一組 id（`SetEffectIds`），這裡看不到，而那些都是
// 嘴巴與眼睛的開合參數，還原了也只是被下一幀的動作／眨眼蓋回去。
//
// **已知限制：保留的是「新動作」會驅動的 id，不是「上一段動作」動過的。**
// 部件不透明度尤其要注意 —— `CubismModel::SaveParameters()`／`LoadParameters()`
// 只複製參數值，**不含 part opacity**，所以部件是「寫了就一直留著」。於是
// 「動作 A 把 Part31 關掉 → 動作 B 完全沒提到 Part31」時，B 起播的那一刻
// Part31 會被還原成 baseline（＝可見）。這對實際踩到的那批模型是成立的
//（每一支動作都驅動同一組 PartOpacity），但要是哪天又出現「某一支動作還是
// 多一隻手」，先從這裡查起：解法會是連上一段動作的 partIds 一起保留。

#include <string>
#include <vector>

namespace l2m {

// 一支動作會寫到的 id（保留出現順序，重複的只留第一次）
struct MotionDrivenIds {
  std::vector<std::string> parameterIds;  // Target == "Parameter"
  std::vector<std::string> partIds;       // Target == "PartOpacity"
};

// 解析 motion3.json 文字。解析不了（壞掉的 JSON、沒有 Curves）一律回空的 ——
// 空清單代表「什麼都不保留」，也就是修改前的舊行為，是安全的那一邊。
MotionDrivenIds motionDrivenIds(const std::string& motionJson);

}  // namespace l2m
