#include "autostart_command.h"

namespace l2m::platform {

std::string autostartCommandLine(const std::string& exePath) { return "\"" + exePath + "\""; }

bool shouldStartHidden(const std::vector<std::string>& args) {
  for (const auto& arg : args) {
    if (arg == kHiddenFlag) return true;
  }
  return false;
}

bool needsHiddenFlagStripped(const std::string& storedLine, const std::string& exePath) { return storedLine == autostartCommandLine(exePath) + " " + kHiddenFlag; }

}  // namespace l2m::platform
