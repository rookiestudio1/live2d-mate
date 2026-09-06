#include "idle_motion_pick.h"

#include "string_util.h"

namespace l2m {

namespace {

constexpr const char* kIdleGroup = "Idle";
constexpr const char* kIdleWord = "idle";

// 取檔名的主檔名：去掉目錄與 `.motion3.json`（也接受只有 `.json` 的寫法）
std::string motionBaseName(const std::string& file) {
  const size_t slash = file.find_last_of("/\\");
  std::string name = slash == std::string::npos ? file : file.substr(slash + 1);
  constexpr const char* kMotionExt = ".motion3.json";
  constexpr size_t kMotionExtLen = 13;  // sizeof - 1；寫死一個數字下次改字串會漏改
  static_assert(sizeof(".motion3.json") - 1 == kMotionExtLen, "副檔名長度要跟字串一致");
  if (strutil::endsWithInsensitive(name, kMotionExt)) return name.substr(0, name.size() - kMotionExtLen);
  const size_t dot = name.find_last_of('.');
  return dot == std::string::npos ? name : name.substr(0, dot);
}

}  // namespace

std::optional<IdleMotionSlot> pickIdleMotion(const std::vector<MotionGroupFiles>& groups) {
  // 1. 群組名就叫 Idle：官方慣例，維持「群組內隨機」的既有行為
  for (const auto& group : groups) {
    if (!group.files.empty() && strutil::equalsInsensitive(group.name, kIdleGroup)) return IdleMotionSlot{group.name, -1};
  }
  // 2. 檔名主檔名就叫 idle：指名到那一支（碧藍航線那批走的是這條）
  for (const auto& group : groups) {
    for (size_t i = 0; i < group.files.size(); ++i) {
      if (strutil::equalsInsensitive(motionBaseName(group.files[i]), kIdleWord)) return IdleMotionSlot{group.name, static_cast<int>(i)};
    }
  }
  // 3. 群組名含 idle（Idle2、idle_loop…）
  for (const auto& group : groups) {
    if (!group.files.empty() && strutil::containsInsensitive(group.name, kIdleWord)) return IdleMotionSlot{group.name, -1};
  }
  // 4. 檔名含 idle（idle_01、touch_idle…）
  for (const auto& group : groups) {
    for (size_t i = 0; i < group.files.size(); ++i) {
      if (strutil::containsInsensitive(motionBaseName(group.files[i]), kIdleWord)) return IdleMotionSlot{group.name, static_cast<int>(i)};
    }
  }
  return std::nullopt;
}

}  // namespace l2m
