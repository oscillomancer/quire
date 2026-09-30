// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include "supple/params/params.h"
#include "supple/patch/patch.h"
namespace supple::delay {
inline constexpr const char* kName = SUPPLE_DELAY_DISPLAY_NAME;
inline constexpr char kPluginId[] = "com.suppleaudio.delay";
inline constexpr uint32_t kStateMagic = 0x51444c31;
enum ParamId : uint32_t {
    kSync,
    kTime,
    kMode,
    kOffset,
    kFeedback,
    kHighpass,
    kLowpass,
    kModRate,
    kModDepth,
    kDrive,
    kDuck,
    kWidth,
    kFreeze,
    kMix,
    kOutput,
    kNumParams
};
using Values = std::array<double, kNumParams>;
params::Table table();
Values defaults();
const patch::FlatPatchType& patchType();
std::string describe(std::span<const double>, bool changedOnly = true);
std::array<double, 2> lineTimes(const Values&, double tempo = 120) SUPPLE_RT;
uint32_t tailSamples(const Values&, double fs, double tempo = 120) SUPPLE_RT;
double safetyLimit(double) SUPPLE_RT;
double loopMagnitude(double hz, double hp, double lp, double fs);
}  // namespace supple::delay
