#pragma once

#include <limits>
#include <map>
#include <string>

#include "supple/analysis/analysis.h"
#include "supple/audio/audio_file.h"

namespace supple::sandbox::detail {

struct Loss {
    double total = std::numeric_limits<double>::infinity();
    std::map<std::string, double> terms;
};

void term(Loss& loss, const std::string& name, double distance, double weight, double scale);
Loss referenceLoss(const analysis::Report& reference, const analysis::Report& candidate);
Result<analysis::Report> analyzeMatched(const audio::AudioData& audio,
                                        std::optional<double> targetLufs,
                                        std::optional<double> noteOff);

// One scored reference comparison (optimizer.md §3): the reference analyzed
// as-is, the test loudness-matched to it, each with its own note-off time,
// then `referenceLoss`. `compare` and the evaluation scorer both go through
// here, so the number an agent reads is the number the optimizer minimizes
// and the scorer judges.
struct ReferenceScore {
    Loss loss;
    double ltasRmsDb = 0.0;
};
Result<ReferenceScore> referenceScore(const audio::AudioData& reference,
                                      std::optional<double> referenceNoteOff,
                                      const audio::AudioData& test,
                                      std::optional<double> testNoteOff);

}  // namespace supple::sandbox::detail
