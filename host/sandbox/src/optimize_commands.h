// optimize.start: CMA-ES parameter tuning as a job (docs/specs/optimizer.md).
#pragma once

#include "supple/cmd/bus.h"
#include "supple/sandbox/session.h"

namespace supple::sandbox {

void registerOptimizeCommands(cmd::Registry& registry);

}  // namespace supple::sandbox
