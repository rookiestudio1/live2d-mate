#pragma once

// 加權隨機 ＋ 冷卻 ＋ 不重複最近 N 個。零領域知識 —— 不認識動作、表情或台詞，
// 只認 id 與權重，IdleDirector 拿它挑任何東西。
//
// 現況是 QRandomGenerator 均勻 bounded()：只有三個動作的模型會連播到同一個，
// 看起來像卡住。時間與亂數都由呼叫端注入，這一層才測得到
//（否則「冷卻」只能靠肉眼盯 5 分鐘）。

#include <deque>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace l2m {

struct PickCandidate {
  std::string id;
  double weight = 1;  // <= 0 直接排除
};

class BehaviorPicker {
public:
  struct Options {
    double cooldownMs = 0;  // 挑中之後多久內不再挑（0 = 不冷卻）
    int noRepeatLast = 0;   // 不重複最近 N 個（0 = 不管）
  };
  // 不寫 `Options options = {}` 的預設引數：預設引數屬於外層類別的 complete-class
  // context，會在類別本身還沒定義完成時就要用到 Options 的成員預設值 ——
  // clang 直接報錯（MSVC 放行），所以拆成兩個建構子。
  BehaviorPicker() = default;
  explicit BehaviorPicker(Options options) : options_(options) {}

  // unit 是呼叫端給的 [0,1) 亂數（測試注定值，production 注 QRandomGenerator）。
  // 過濾順序：權重 <= 0 → 排除；最近 N 個 → 排除（除非會把候選清空 ——
  // 只有一個候選時永遠回它）；冷卻中 → 排除，但**全部都在冷卻時退回
  // 冷卻剩餘最短的那一個**而不是 nullopt —— 只有兩個動作的模型不該因為
  // 冷卻規則就整段安靜下來。挑中即記錄。
  std::optional<std::string> pick(const std::vector<PickCandidate>& candidates, double nowMs, double unit);

  // 把某個 id 的紀錄抹掉（例如那一步執行失敗，不該佔用冷卻）
  void forget(const std::string& id);
  // 換模型時整個歸零
  void reset();

  // 測試斷言用：某個 id 最後一次被挑中的時刻
  std::optional<double> lastUsedMs(const std::string& id) const;

private:
  Options options_;
  std::map<std::string, double> lastUsed_;
  // 最近挑中的 id，新的在尾端
  std::deque<std::string> recent_;
};

}  // namespace l2m
