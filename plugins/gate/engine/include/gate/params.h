// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include "supple/params/params.h"
#include "supple/patch/patch.h"
namespace supple::gate {
inline constexpr char kName[] = SUPPLE_GATE_DISPLAY_NAME;
inline constexpr char kPluginId[] = "com.suppleaudio.gate";
inline constexpr uint32_t kStateMagic = 0x51475431;
enum ParamId : uint32_t {
    kMode,
    kThreshold,
    kRange,
    kRatio,
    kHysteresis,
    kAttack,
    kHold,
    kRelease,
    kLookahead,
    kSidechain,
    kScHighpass,
    kScLowpass,
    kScListen,
    kStereoLink,
    kMix,
    kOutput,
    kGr,
    kOpen,
    kNumParams
};
using Values = std::array<double, kNumParams>;
params::Table table();
Values defaults();
const patch::FlatPatchType& patchType();
std::string describe(std::span<const double>, bool changedOnly = true);
uint32_t lookaheadSamples(double choice, double sampleRate);
// Static downward-expansion curve before history-dependent hysteresis/hold.
double expansionDb(double level, double threshold, double ratio, double range) SUPPLE_RT;
}  // namespace supple::gate
