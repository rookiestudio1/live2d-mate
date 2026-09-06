#include "action_player.h"

#include <QDebug>

#include <algorithm>

#include "core/builtin_actions.h"
#include "core/model_commands.h"
#include "core/motion_builder.h"
#include "model_controller.h"
#include "parameter_overlay.h"

namespace l2m {

namespace {

bool contains(const std::vector<std::string>& list, const std::string& value) { return std::find(list.begin(), list.end(), value) != list.end(); }

}  // namespace

bool playMotionByName(ModelController& controller, const ModelInfo& model, const std::string& group, int index, int priority, bool loop) {
  if (!contains(model.builtinMotions, group)) return controller.startMotion(group, index, priority, loop);

  const auto built = builtinMotionFor(model.parameters, group);
  if (!built) return false;
  try {
    return controller.playSynthesizedMotion(buildMotion3(built->keyframes, built->options), built->options.loop || loop, built->options.fadeInMs, built->options.fadeOutMs, built->carryPastPhysics);
  } catch (const std::exception& err) {
    // 只有內建表本身寫壞了才會走到這裡（關鍵影格不合法）。對呼叫端來說
    // 等於這個動作播不出來，跟優先權被擋下是同一種結果。
    qWarning() << "[live2d] 內建動作" << group.c_str() << "編譯失敗：" << err.what();
    return false;
  }
}

bool applyExpressionByName(ModelController& controller, ParameterOverlay& overlay, const ModelInfo& model, const std::optional<std::string>& name) {
  const bool builtin = name.has_value() && contains(model.builtinExpressions, *name);

  // core/builtin_actions.h 合成的內建表情走參數層。不論這個模型平常用哪一種表情，
  // 切走時都要把內建的痕跡收乾淨 —— 只在「這次選的是內建」時才處理的話，
  // 從 sleepy 切回模型自己的表情會留下一雙半睜的眼睛。
  if (!model.builtinExpressions.empty()) {
    const auto apply = applyBuiltinExpression(model.parameters, builtin ? name : std::nullopt);
    if (!apply.release.empty()) overlay.release(apply.release);
    if (!apply.set.empty()) overlay.set(apply.set);
  }

  // 這個模型的「表情」其實是參數開關的話，改走參數層（互斥：套一個就把其他個歸零）
  if (!model.paramExpressions.empty()) {
    overlay.set(virtualExpressionParams(model, builtin ? std::nullopt : name));
    return true;
  }
  if (name.has_value() && !builtin) return controller.setExpression(*name);

  // 選了內建表情也要把 .exp3.json 的那一份收掉，否則星星眼會疊在 sleepy 上面
  controller.clearExpression();
  return true;
}

}  // namespace l2m
