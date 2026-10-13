#pragma once

#include "supple/cmd/bus.h"
#include "supple/sandbox/commands.h"

namespace supple::sandbox {
// dsp.write, dsp.check, dsp.get, dsp.list (host/dsp-commands) and dsp.test.
void registerDspCommands(cmd::Registry& registry, const ExamplePlugins& examples);
}  // namespace supple::sandbox
