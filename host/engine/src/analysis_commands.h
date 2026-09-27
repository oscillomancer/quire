#pragma once

#include "supple/analysis/analysis.h"
#include "supple/audio/audio_file.h"
#include "supple/cmd/bus.h"

namespace supple::engine {
Result<json::Value> analyzeAudio(const audio::AudioData& data, analysis::Detail detail);
void registerEngineAnalysisCommands(cmd::Registry& registry);
}  // namespace supple::engine
