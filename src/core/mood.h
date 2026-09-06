#pragma once

// 心情與熟悉度（刻意的反直覺設計，理由寫在下面 —— 改動前先讀）。
//
// **心情：只放記憶體、只往正向偏、不做狀態機。** 每條邊都要設計、要測、
// 要翻譯，而使用者感受得到的只有「摸它之後那幾分鐘比較活潑」這一件事。
// nudge() 之後 cheerful() 為 true，幾分鐘後自動退回 —— 就這樣。
// 消沉、生氣之類的負面狀態刻意不存在：桌寵擺臭臉的標準下場是被關掉。
//
// **熟悉度：只增不減。** 這個 app 沒有推播、沒有任何「賺回注意力」的管道，
// 衰減式好感度在這種載體上唯一能生產的東西，就是一隻在你忙完回來時擺臭臉的
// 桌寵。單調遞增的熟悉度給得到「它記得我」的感覺，卻不索取任何東西 ——
// 嚴格優於衰減設計。計數存在 config 的 autonomy.familiarity（永不歸零），
// 這裡只提供級距換算；級距墊高說話機率（idle_director.cpp）。

namespace l2m {

class MoodState {
public:
  struct Options {
    double decayMs = 5 * 60 * 1000;  // 開心的餘韻維持 5 分鐘
  };
  // 不寫 `Options options = {}` 的預設引數：預設引數屬於外層類別的 complete-class
  // context，會在類別本身還沒定義完成時就要用到 Options 的成員預設值 ——
  // clang 直接報錯（MSVC 放行），所以拆成兩個建構子。
  MoodState() = default;
  explicit MoodState(Options options) : options_(options) {}

  // 被摸了、被逗了 → 那幾分鐘比較活潑
  void nudge(double nowMs) { lastNudgeMs_ = nowMs; }

  // 還在開心的餘韻裡嗎
  bool cheerful(double nowMs) const { return lastNudgeMs_ >= 0 && nowMs - lastNudgeMs_ < options_.decayMs; }

private:
  Options options_;
  double lastNudgeMs_ = -1;
};

// 累計互動次數 → 級距 0~3。只用來**解鎖**（墊高說話機率等），永遠不會鎖回去。
inline int familiarityLevel(int interactionCount) {
  if (interactionCount >= 200) return 3;
  if (interactionCount >= 50) return 2;
  if (interactionCount >= 10) return 1;
  return 0;
}

}  // namespace l2m
