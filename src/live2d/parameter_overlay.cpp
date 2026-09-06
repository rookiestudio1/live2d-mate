#include "parameter_overlay.h"

#include <CubismFramework.hpp>
#include <Id/CubismIdManager.hpp>
#include <Model/CubismModel.hpp>

#include <algorithm>

#include "model_controller.h"

using namespace Live2D::Cubism::Framework;

namespace l2m {

ParameterOverlay::ParameterOverlay() { clock_.start(); }

void ParameterOverlay::attach(ModelController* controller) {
  detach();
  if (!controller || !controller->hasParameterTable()) return;

  declared_ = controller->parameterSnapshots();
  index_.clear();
  for (size_t i = 0; i < declared_.size(); ++i) index_[declared_[i].id] = static_cast<int>(i);
  controller_ = controller;
}

void ParameterOverlay::detach() {
  tracks_.clear();
  pending_.clear();
  index_.clear();
  declared_.clear();
  controller_ = nullptr;
}

ParameterOverlay::SetOutcome ParameterOverlay::set(const std::vector<SetParameterRequest>& requests) {
  SetOutcome outcome;
  if (!controller_) {
    for (const auto& r : requests) outcome.unknown.push_back(r.id);
    return outcome;
  }

  // 補間要從畫面上真正的值出發（而不是上一次的目標值），所以先把現值抓新的一份。
  // 索引順序跟著模型的參數索引走，attach 時建的 index_ 仍然有效。
  declared_ = controller_->parameterSnapshots();

  std::vector<SetParameterRequest> accepted;
  accepted.reserve(requests.size());
  for (const auto& request : requests) {
    const auto it = index_.find(request.id);
    if (it == index_.end()) {
      outcome.unknown.push_back(request.id);
      continue;
    }
    SetParameterRequest clamped = request;
    const ParameterSnapshot& declared = declared_[static_cast<size_t>(it->second)];
    clamped.value = std::clamp(request.value, declared.min, declared.max);
    accepted.push_back(std::move(clamped));
  }

  // 模式決定放掉之後要回到哪裡：疊加模式回到「沒有疊加」也就是 0，
  // 覆寫模式回到模型的預設值。
  std::map<std::string, ParameterMode> modeById;
  for (const auto& r : requests) modeById[r.id] = r.mode;

  tracks_.set(
    accepted,
    [this, &modeById](const std::string& id) {
      ParameterProbe probe;
      const auto it = index_.find(id);
      if (it == index_.end()) return probe;
      const ParameterSnapshot& declared = declared_[static_cast<size_t>(it->second)];

      probe.value = declared.value;
      const auto mode = modeById.find(id);
      probe.base = (mode != modeById.end() && mode->second == ParameterMode::Add) ? 0 : declared.defaultValue;
      return probe;
    },
    nowMs());

  return outcome;
}

void ParameterOverlay::release(const std::optional<std::vector<std::string>>& ids) { tracks_.release(ids, nowMs()); }

bool ParameterOverlay::skipForLipSync(const std::string& id) const {
  if (!lipSyncActive || !lipSyncActive() || !controller_) return false;
  const auto& ids = controller_->lipSyncParameterIds();
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

void ParameterOverlay::applyEarly(CubismModel* model) {
  if (!controller_ || !model) return;

  // 一幀只取樣一次：sample() 會把淡完的軌道刪掉，取兩次晚寫就吃不到最後一格
  pending_ = tracks_.sample(nowMs());
  if (pending_.empty()) return;

  for (const auto& param : pending_) {
    if (param.mode == ParameterMode::Add) continue;  // 疊加只能晚寫
    if (skipForLipSync(param.id)) continue;
    model->SetParameterValue(CubismFramework::GetIdManager()->GetId(param.id.c_str()), static_cast<csmFloat32>(param.value));
  }
}

void ParameterOverlay::applyLate(CubismModel* model) {
  if (!controller_ || !model || pending_.empty()) return;

  for (const auto& param : pending_) {
    if (skipForLipSync(param.id)) continue;
    const CubismIdHandle id = CubismFramework::GetIdManager()->GetId(param.id.c_str());
    if (param.mode == ParameterMode::Add) {
      model->AddParameterValue(id, static_cast<csmFloat32>(param.value));
    } else {
      model->SetParameterValue(id, static_cast<csmFloat32>(param.value));
    }
  }
}

}  // namespace l2m
