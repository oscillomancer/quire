#pragma once
#include <array>
#include <string>
#include "supple/params/params.h"
#include "supple/patch/patch.h"
namespace supple::eq {
inline constexpr std::size_t kBands = 8, kBandParams = 7, kParamCount = 59;
enum Field : std::size_t { Enabled, Type, Freq, Gain, Q, Slope, Channel };
enum Filter : int { Bell, LowShelf, HighShelf, HighPass, LowPass, Notch, BandPass, Tilt };
enum Placement : int { Stereo, Left, Right, Mid, Side };
inline constexpr std::size_t kPhase = 56, kAutoGain = 57, kOutput = 58;
constexpr std::size_t bandParam(std::size_t band, Field field) {
    return band * kBandParams + field;
}
using Values = std::array<double, kParamCount>;
params::Table table();
Values defaults();
const patch::FlatPatchType& patchType();
std::string describe(const Values&, patch::Detail = patch::Detail::Changed);
}  // namespace supple::eq
