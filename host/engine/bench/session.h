#pragma once
#include "supple/engine/app.h"

namespace supple::engine::bench {
// All edits use the command bus: track defaults and routing follow the model.
Status buildSession(EngineApp& app, json::Value recipe, int tracks);
// Identical original audio item on independent effect tracks; optional live monitoring.
Status buildEffectSession(EngineApp& app, const std::string& plugin,
                          const std::filesystem::path& input, int tracks, bool live,
                          int fxPerTrack);
// Every FX of the session runs in this isolation mode (fx.set isolation).
Status isolateAll(EngineApp& app, const std::string& isolation);
// Service one model tick; false if a device restart retired the expected engine.
bool serviceTick(EngineApp& app, const Engine* expected);
// REAPER consumes the model's compiled MIDI and CLAP-saved opaque states.
Result<json::Value> exportSession(EngineApp& app);
}  // namespace supple::engine::bench
