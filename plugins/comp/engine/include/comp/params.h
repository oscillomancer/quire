#pragma once
#include <array>
#include "supple/params/params.h"
#include "supple/patch/patch.h"

namespace supple::comp {
inline constexpr const char* kName = SUPPLE_COMP_DISPLAY_NAME;
enum ParamId : std::uint32_t {
    kMode,
    kThreshold,
    kRatio,
    kKnee,
    kAttack,
    kRelease,
    kAutoRelease,
    kDetector,
    kRmsWindow,
    kTopology,
    kSidechain,
    kScHighpass,
    kScLowpass,
    kScListen,
    kStereoLink,
    kLookahead,
    kMakeup,
    kAutoMakeup,
    kCeiling,
    kTruePeak,
    kMix,
    kOutput,
    kGr,
    kNumParams
};
using Values = std::array<double, kNumParams>;
params::Table table();
Values defaults();
std::string formatValue(std::size_t index, double physical, bool compact = false);
const patch::FlatPatchType& patchType();
std::string describe(const Values&, patch::Detail detail = patch::Detail::Changed);
// Static feedforward reduction, negative dB. Ratio 100 represents infinity.
double gainReduction(double levelDb, double threshold, double ratio, double knee) SUPPLE_RT;
// Slope-domain form lets the processor smooth all the way to infinity.
double reductionFromSlope(double levelDb, double threshold, double slope, double knee) SUPPLE_RT;
// Includes lookahead and the true-peak detector's 16-sample group delay.
std::uint32_t latencyFor(const Values&, double sampleRate) SUPPLE_RT;
}  // namespace supple::comp
