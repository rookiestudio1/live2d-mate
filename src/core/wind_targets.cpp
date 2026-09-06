#include "wind_targets.h"

#include <array>

#include "json_doc.h"

namespace l2m {

namespace {

using jsonu::Doc;

// 「整體姿勢」的 Cubism 標準參數。輸出寫到這裡的 setting 是角度跟隨器，不是會飄的鏈。
// 精確比對（理由見標頭：ParamBodyAngleZ2 這種自訂參數不算）。
constexpr std::array<const char*, 6> kPoseParamIds = {
  "ParamAngleX", "ParamAngleY", "ParamAngleZ", "ParamBodyAngleX", "ParamBodyAngleY", "ParamBodyAngleZ",
};

bool isPoseParam(const std::string& id) {
  for (const char* pose : kPoseParamIds) {
    if (id == pose) return true;
  }
  return false;
}

}  // namespace

bool settingAcceptsWind(const PhysicsSettingWind& setting) {
  if (setting.particleCount < kWindMinParticles) return false;
  for (const auto& id : setting.outputIds) {
    if (isPoseParam(id)) return false;
  }
  return true;
}

std::vector<std::uint8_t> windTargetMask(const std::string& physicsJson) {
  const auto doc = Doc::parse(physicsJson);
  if (!doc) return {};

  yyjson_val* settings = jsonu::get(doc->root(), "PhysicsSettings");
  if (!yyjson_is_arr(settings)) return {};

  std::vector<std::uint8_t> mask;
  size_t idx, max;
  yyjson_val* setting;
  yyjson_arr_foreach(settings, idx, max, setting) {
    PhysicsSettingWind info;

    yyjson_val* vertices = jsonu::get(setting, "Vertices");
    if (yyjson_is_arr(vertices)) info.particleCount = static_cast<int>(yyjson_arr_size(vertices));

    yyjson_val* outputs = jsonu::get(setting, "Output");
    if (yyjson_is_arr(outputs)) {
      size_t i, m;
      yyjson_val* output;
      yyjson_arr_foreach(outputs, i, m, output) {
        const std::string id = jsonu::getString(jsonu::get(output, "Destination"), "Id");
        if (!id.empty()) info.outputIds.push_back(id);
      }
    }

    mask.push_back(settingAcceptsWind(info) ? 1 : 0);
  }
  return mask;
}

}  // namespace l2m
