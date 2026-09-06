#include "perform_sync.h"

namespace l2m {

bool isSpeechCompanionAction(const std::string& action) { return action == "motion" || action == "expression" || action == "parameters" || action == "animate"; }

bool deferUntilSpeech(const std::vector<PerformStep>& steps, size_t index) {
  if (index >= steps.size()) return false;
  if (!isSpeechCompanionAction(steps[index].action)) return false;

  // 往後找第一個不是伴奏的步驟：是 speak 才押後（規則②）。
  // 中途的伴奏步驟會各自問一次這支函式，所以整串會一起押後或一起不押後。
  for (size_t i = index + 1; i < steps.size(); ++i) {
    if (isSpeechCompanionAction(steps[i].action)) continue;
    return steps[i].action == "speak" && !steps[i].bubbleOnly;
  }
  return false;
}

}  // namespace l2m
