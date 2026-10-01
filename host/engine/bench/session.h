#pragma once
#include "supple/engine/app.h"

namespace supple::engine::bench {
// All edits use the command bus: track defaults and routing follow the model.
Status buildSession(EngineApp& app, json::Value recipe, int tracks);
// REAPER consumes the model's compiled MIDI and CLAP-saved opaque states.
Result<json::Value> exportSession(EngineApp& app);
}  // namespace supple::engine::bench
