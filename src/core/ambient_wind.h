#pragma once

// 環境風：讓模型的頭髮／衣服持續飄動。
// 與 core/drag_swing.h 同一個切法 —— 時間由呼叫端以毫秒傳入，不碰 Qt/GL，
// 強度映射、陣風包絡與 clamp 才驗得到。輸出直接進 CubismPhysics 的 Options.Wind。
//
// 關鍵設計：Cubism 的 Wind 是一個「力」。餵常數進去，物理系統只會收斂到新的
// 平衡位置然後靜止 —— 畫面上是「頭髮固定歪向一邊」，看起來像模型壞掉，不是在飄。
// 所以輸出是「基準強度 × 陣風包絡」，包絡用兩個週期互質一點的正弦波疊出來。
// 刻意不用亂數：同一個 nowMs 結果相同，測試才驗得到，也不會因更新率不同而抖動。

namespace l2m {

// 風力向量的總保險絲（physics rig 空間）。原本是 DragSwingTuning::windMax，
// 搬到這裡當共用具名常數：拖曳搖晃與環境風在 ModelController 是**相加**後才寫進
// Options.Wind，單一來源各自觸不到的上限，相加之後才可能真的碰到 ——
// 保險絲放在相加之後（model_controller.cpp 的第 6.5 步）才有意義。
inline constexpr double kWindMax = 25;

// 「這陣風該吹哪些 PhysicsSetting」是另一件事，在 core/wind_targets.h ——
// 吹到把頭身角度轉成身體傾斜的跟隨 rig 上，身體會搖得比頭髮還大。

enum class WindDirection { Left, Right };

struct WindVector {
  double x = 0;
  double y = 0;
};

struct AmbientWindTuning {
  // strength 1.0 對應的風力（physics rig 空間）。錨點取自 drag_swing.h 的實測：
  // 快甩 2500 px/s × windPerPxPerSec 0.0005 = 1.25，那是「明顯但不誇張」的量級
  // （那份註解寫得很清楚：強度逐輪從 0.02 調到 0.0005，「頭髮對風極敏感」）。
  double maxWind = 1.25;
  // 陣風包絡：輸出恆為正，風向不會在週期中途反轉。
  // 0 = 恆定風（會定住）；1 = 會完全停下來再吹。
  // 0.35 實測起伏太小 —— 物理收斂到 0.65~1.0 的窄帶裡，頭髮只是微微顫，
  // 看起來仍像「歪著定住」。0.8 讓風在 0.2~1.0 之間大開大合，
  // 谷底夠低頭髮才會真的盪回來，飄動的行程一眼就看得出來。
  double gustDepth = 0.8;
  // 週期同步拉長：包絡擺盪變大之後，太快的起伏看起來像抖動而不是陣風，
  // 慢一點頭髮才跟得上（物理本身有慣性與阻尼）。
  double gustPeriodMsA = 2700;
  double gustPeriodMsB = 1100;  // 兩個互質一點的週期，疊起來才不會聽出規律
  double gustWeightA = 0.6;     // B 的權重 = 1 - A
};

// 純函數，不持有狀態：enabled 為 false 或 strength <= 0 時回 {0,0}。
// y 恆為 0 —— 風是水平的，垂直方向交給重力。
// Left 為 x < 0、Right 為 x > 0；實際看起來往哪飄取決於各模型 rig 的座標習慣。
WindVector ambientWind(const AmbientWindTuning& tuning, bool enabled, WindDirection direction, double strength, double nowMs);

}  // namespace l2m
