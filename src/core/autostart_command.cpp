#include "autostart_command.h"

namespace l2m::platform {

std::string autostartCommandLine(const std::string& exePath) { return "\"" + exePath + "\" " + kHiddenFlag; }

bool shouldStartHidden(const std::vector<std::string>& args) {
  for (const auto& arg : args) {
    if (arg == kHiddenFlag) return true;
  }
  return false;
}

}  // namespace l2m::platform
