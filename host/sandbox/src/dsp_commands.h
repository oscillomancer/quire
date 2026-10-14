// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include "supple/cmd/bus.h"
#include "supple/sandbox/commands.h"

namespace supple::sandbox {
// dsp.write, dsp.check, dsp.get, dsp.list (host/dsp-commands) and dsp.test.
void registerDspCommands(cmd::Registry& registry, const ExamplePlugins& examples);
}  // namespace supple::sandbox
