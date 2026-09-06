#include "autonomy_gates.h"

namespace l2m {

bool autonomousMoveAllowed(const AppConfig& config) { return config.autonomy.move && !config.interaction.lockPosition && config.interaction.dragMove; }

}  // namespace l2m
