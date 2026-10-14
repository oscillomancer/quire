// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Sandbox analysis commands: analyze, compare, masking, profile.evaluate.
#pragma once

#include <string>

#include "supple/analysis/analysis.h"
#include "supple/cmd/bus.h"
#include "supple/sandbox/session.h"

namespace supple::sandbox {

void registerAnalysisCommands(cmd::Registry& registry);

// The report of a render (render's note-off applied) plus its "summary"
// prose; used by `render` with `analyze`.
Result<json::Value> analyzeRender(Session& s, const std::string& render, analysis::Detail detail);

}  // namespace supple::sandbox
