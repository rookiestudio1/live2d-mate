#pragma once

// 參數覆寫軌道。
//
// 每個被外部接管的參數是一條軌道：從呼叫當下的值補間到目標值、撐一段時間、
// 再淡回原本的值然後自動消失。時間一律由呼叫端傳進來，這裡不碰系統時鐘，
// 才有辦法用固定時間點把補間與歸位一格一格測出來。

#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace l2m {

// 參數寫入模式：覆寫或疊加（疊加讓值跟內建的呼吸視線混在一起）
enum class ParameterMode { Set, Add };

// 外部要求接管的一個參數
struct SetParameterRequest {
  std::string id;
  double value = 0;
  std::optional<double> durationMs;
  std::optional<double> holdMs;
  ParameterMode mode = ParameterMode::Set;
};

// 這一刻該寫進模型的一個參數
struct ActiveParameter {
  std::string id;
  double value = 0;
  ParameterMode mode = ParameterMode::Set;

  bool operator==(const ActiveParameter& other) const { return id == other.id && value == other.value && mode == other.mode; }
};

// 呼叫端提供的現況：value 是參數當下的值，base 是放掉之後要回去的值
struct ParameterProbe {
  double value = 0;
  double base = 0;
};

class ParameterTracks {
public:
  using ProbeFn = std::function<ParameterProbe(const std::string& id)>;

  size_t size() const { return tracks_.size(); }

  std::vector<std::string> ids() const;

  // 接管一批參數。
  // probe 回報每個參數的現況，補間才會從畫面上真正的值出發（而不是從上一次的目標值）。
  void set(const std::vector<SetParameterRequest>& requests, const ProbeFn& probe, double now);

  // 放掉指定參數（nullopt 代表全部）：緩動淡回 base，淡完自動移除。
  // 淡回是 smoothstep 不是線性 —— 起點是被釘住不動的值，線性會在放掉那一刻
  // 憑空生出速度，看起來像被抽掉而不是放開（時間與理由見 .cpp 的 kReleaseFadeMs）
  void release(const std::optional<std::vector<std::string>>& ids, double now);

  // 立刻清空，不淡出。切換或卸載模型時用。
  void clear() { tracks_.clear(); }

  // 這一刻要寫進模型的參數。
  //
  // 淡回結束的軌道會「再寫最後一格 base 才刪掉」—— 直接刪的話，模型上留著的是
  // 淡回途中最後一次寫進去的值（差不多但不等於 base），那個殘值會一直掛在畫面上。
  std::vector<ActiveParameter> sample(double now);

private:
  struct Track {
    double from = 0;
    double to = 0;
    double startAt = 0;
    double durationMs = 0;
    // 補間結束後還要撐多久；nullopt 代表不自動歸位
    std::optional<double> holdMs;
    // 放掉之後要回去的值
    double base = 0;
    ParameterMode mode = ParameterMode::Set;
    // 已經進入淡回階段的話，這是淡回的起點時間；否則是 nullopt
    std::optional<double> releaseAt;
    // 淡回起點的值。淡回途中被讀到才不會從頭跳。
    double releaseFrom = 0;
    // 已經把最後一格 base 寫出去了，下一次 sample 就可以安心刪掉
    bool settled = false;
  };

  double valueOf(const Track& track, double now) const;

  std::map<std::string, Track> tracks_;
};

}  // namespace l2m
