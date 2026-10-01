// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include "supple/params/params.h"
#include "supple/patch/patch.h"
namespace supple::reverb {
inline constexpr const char* kName = SUPPLE_REVERB_DISPLAY_NAME;
enum ParamId : std::uint32_t {
    kSize,
    kDecay,
    kPredelay,
    kLowDecay,
    kHighDecay,
    kCrossover,
    kDampingFreq,
    kDiffusion,
    kEarly,
    kModulation,
    kWidth,
    kLowCut,
    kHighCut,
    kFreeze,
    kMix,
    kOutput,
    kNumParams
};
using Values = std::array<double, kNumParams>;
params::Table table();
Values defaults();
std::string formatValue(std::size_t, double, bool compact = false);
const patch::FlatPatchType& patchType();
std::string describe(const Values&, patch::Detail = patch::Detail::Changed);
// Tail in samples, UINT32_MAX while frozen. Zero processing latency.
std::uint32_t tailFor(const Values&, double sampleRate) SUPPLE_RT;
// Continuous three-region target, seconds; the editor plots this design target.
double decaySeconds(const Values&, double hz, double sampleRate = 48000);
}  // namespace supple::reverb
