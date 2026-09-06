#pragma once

// 把 ParameterTracks 算出來的值真的寫進 Cubism 模型。
//
// 寫入時機是這一層的重點，也是 ModelController::update() 的順序被寫死的原因：
//
//   動作 → SaveParameters → [早寫] → 眨眼 → 表情 → 視線 → 呼吸 → physics
//        → 口型 → [晚寫] → pose → Update()
//
// 所以：
//  - 想讓頭髮衣服的物理跟著反應，就得在「早寫」寫（physics 在那之後才算）
//  - 想不被眨眼、呼吸、視線疊掉，就得在「晚寫」再寫一次
//
// 覆寫模式（Set）兩邊都寫，兩者兼得（Set 冪等，寫兩次結果相同）。
// 疊加模式（Add）只能挑一邊寫 —— 兩邊都寫會在同一幀疊兩次變成雙倍；
// 這裡選晚寫，讓 Add 的語意是「疊在眨眼呼吸視線之上」。
//（跨幀不會累積：SaveParameters 在兩個掛點之前就存好了，
//  掛點之後的疊加不會進到下一幀的起點 —— 第 5 步的視線加成同理。）
//
// 時間由本類別自己的單調時鐘提供，不用呼叫端傳。
// ParameterTracks 那層仍然是注入時間的（它才是被單元測試釘住的部分）。

#include <QElapsedTimer>

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/model_types.h"
#include "core/parameter_tracks.h"

namespace Live2D::Cubism::Framework {
class CubismModel;
}

namespace l2m {

class ModelController;

class ParameterOverlay {
public:
  ParameterOverlay();

  // 綁上模型並建立 id → 索引表。模型沒有參數表時 available() 會是 false。
  void attach(ModelController* controller);
  void detach();

  bool available() const { return controller_ != nullptr; }
  bool has(const std::string& id) const { return index_.count(id) > 0; }

  // 目前被接管中的參數 id
  std::vector<std::string> activeIds() const { return tracks_.ids(); }

  struct SetOutcome {
    // 模型裡不存在的 id，讓上層把可用清單當成 hint 回給 AI
    std::vector<std::string> unknown;
  };

  // 接管一批參數。值會夾在模型宣告的上下限之內 ——
  // 超出範圍的值在 Cubism 裡是未定義行為。
  SetOutcome set(const std::vector<SetParameterRequest>& requests);

  // 放掉指定參數（nullopt 代表全部），淡回原值後自動移除
  void release(const std::optional<std::vector<std::string>>& ids);

  // 切換／卸載模型時立刻清空，不淡出
  void clear() { tracks_.clear(); }

  // ModelController 的兩個掛點
  void applyEarly(Live2D::Cubism::Framework::CubismModel* model);
  void applyLate(Live2D::Cubism::Framework::CubismModel* model);

  // 說話中回 true：嘴巴歸口型同步管，硬蓋上去會讓嘴型一動也不動
  std::function<bool()> lipSyncActive;

private:
  double nowMs() const { return static_cast<double>(clock_.nsecsElapsed()) / 1e6; }
  bool skipForLipSync(const std::string& id) const;

  ModelController* controller_ = nullptr;
  ParameterTracks tracks_;
  // id → 參數索引，查上下限用；參數上百個，每幀線性搜尋太浪費
  std::map<std::string, int> index_;
  std::vector<ParameterSnapshot> declared_;

  // 每幀在早寫掛點取樣一次，晚寫沿用同一份 ——
  // 取樣有副作用（淡完的軌道會被移除），一幀取兩次會讓晚寫漏掉最後一格。
  std::vector<ActiveParameter> pending_;

  QElapsedTimer clock_;
};

}  // namespace l2m
