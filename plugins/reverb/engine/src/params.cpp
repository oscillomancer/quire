// SPDX-License-Identifier: MIT
#include "reverb/params.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace supple::reverb {
namespace {
using namespace params;
using units::Unit;
constexpr std::string_view frequency[] = {"role:frequency"};
constexpr std::string_view level[] = {"role:level"};
constexpr std::string_view mix[] = {"role:mix"};
constexpr std::string_view mode[] = {"role:mode"};
constexpr std::string_view time[] = {"role:time"};
constexpr std::string_view width[] = {"role:width"};
constexpr ParamSpec specs[] = {
    {.id = kSize,
     .key = "size",
     .name = "Size",
     .group = "Space",
     .unit = Unit::percent,
     .min = 0,
     .max = 100,
     .def = 50,
     .tags = width,
     .description = "Scales line times geometrically.",
     .smoothingMs = 20},
    {.id = kDecay,
     .key = "decay",
     .name = "Decay",
     .group = "Space",
     .unit = Unit::ms,
     .min = 100,
     .max = 60000,
     .def = 2000,
     .curve = {CurveKind::Log},
     .tags = time,
     .description = "Mid-band RT60.",
     .smoothingMs = 20},
    {.id = kPredelay,
     .key = "predelay",
     .name = "Predelay",
     .group = "Space",
     .unit = Unit::ms,
     .min = 0,
     .max = 500,
     .def = 10,
     .tags = time,
     .description = "Wet onset delay; 50 ms glide.",
     .smoothingMs = 50},
    {.id = kLowDecay,
     .key = "low_decay",
     .name = "Low decay",
     .group = "Decay",
     .unit = Unit::x,
     .min = 0.25,
     .max = 4,
     .def = 1,
     .curve = {CurveKind::Log},
     .tags = time,
     .description = "Low-frequency RT60 multiplier.",
     .smoothingMs = 20},
    {.id = kHighDecay,
     .key = "high_decay",
     .name = "High decay",
     .group = "Decay",
     .unit = Unit::x,
     .min = 0.1,
     .max = 1,
     .def = 0.5,
     .curve = {CurveKind::Log},
     .tags = time,
     .description = "High-frequency RT60 multiplier.",
     .smoothingMs = 20},
    {.id = kCrossover,
     .key = "crossover",
     .name = "Crossover",
     .group = "Decay",
     .unit = Unit::Hz,
     .min = 100,
     .max = 1000,
     .def = 250,
     .curve = {CurveKind::Log},
     .tags = frequency,
     .description = "Low decay shelf frequency.",
     .smoothingMs = 20},
    {.id = kDampingFreq,
     .key = "damping_freq",
     .name = "Damping",
     .group = "Decay",
     .unit = Unit::Hz,
     .min = 1000,
     .max = 16000,
     .def = 6000,
     .curve = {CurveKind::Log},
     .tags = frequency,
     .description = "High decay shelf frequency.",
     .smoothingMs = 20},
    {.id = kDiffusion,
     .key = "diffusion",
     .name = "Diffusion",
     .group = "Texture",
     .unit = Unit::percent,
     .min = 0,
     .max = 100,
     .def = 70,
     .tags = mix,
     .description = "Four allpass stages per channel.",
     .smoothingMs = 20},
    {.id = kEarly,
     .key = "early",
     .name = "Early",
     .group = "Texture",
     .unit = Unit::dB,
     .min = -60,
     .max = 6,
     .def = -6,
     .tags = level,
     .description = "Early reflections; minimum exactly off.",
     .flags = kAutomatable | kOffAtMin,
     .smoothingMs = 20},
    {.id = kModulation,
     .key = "modulation",
     .name = "Modulation",
     .group = "Texture",
     .unit = Unit::percent,
     .min = 0,
     .max = 100,
     .def = 20,
     .tags = mix,
     .description = "Deterministic slow delay modulation.",
     .smoothingMs = 20},
    {.id = kWidth,
     .key = "width",
     .name = "Width",
     .group = "Output",
     .unit = Unit::percent,
     .min = 0,
     .max = 200,
     .def = 100,
     .tags = width,
     .description = "Wet side-channel multiplier.",
     .smoothingMs = 20},
    {.id = kLowCut,
     .key = "low_cut",
     .name = "Low cut",
     .group = "Input",
     .unit = Unit::Hz,
     .min = 20,
     .max = 1000,
     .def = 20,
     .curve = {CurveKind::Log},
     .tags = frequency,
     .description = "12 dB/oct input highpass.",
     .smoothingMs = 20},
    {.id = kHighCut,
     .key = "high_cut",
     .name = "High cut",
     .group = "Input",
     .unit = Unit::Hz,
     .min = 1000,
     .max = 20000,
     .def = 16000,
     .curve = {CurveKind::Log},
     .tags = frequency,
     .description = "12 dB/oct input lowpass.",
     .smoothingMs = 20},
    {
        .id = kFreeze,
        .key = "freeze",
        .name = "Freeze",
        .group = "Space",
        .type = Type::Bool,
        .min = 0,
        .max = 1,
        .def = 0,
        .tags = mode,
        .description = "Lossless held FDN; ignores new input.",
    },
    {.id = kMix,
     .key = "mix",
     .name = "Mix",
     .group = "Output",
     .unit = Unit::percent,
     .min = 0,
     .max = 100,
     .def = 20,
     .tags = mix,
     .description = "Linear dry/wet blend.",
     .smoothingMs = 20},
    {.id = kOutput,
     .key = "output",
     .name = "Output",
     .group = "Output",
     .unit = Unit::dB,
     .min = -24,
     .max = 24,
     .def = 0,
     .tags = level,
     .description = "Final gain.",
     .smoothingMs = 20},
};
}  // namespace
params::Table table() { return specs; }
Values defaults() {
    Values v{};
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = specs[i].def;
    return v;
}
std::string formatValue(std::size_t i, double v, bool compact) {
    if (i >= kNumParams) return {};
    return compact ? params::formatCompact(specs[i], v) : params::format(specs[i], v);
}
const patch::FlatPatchType& patchType() {
    static const patch::FlatPatchType t("reverb", 1, 0, specs, false);
    return t;
}
std::string describe(const Values& v, patch::Detail detail) {
    std::string text;
    constexpr std::size_t order[] = {
        kLowCut,    kHighCut,   kPredelay,    kEarly,      kDiffusion, kSize,  kDecay, kLowDecay,
        kCrossover, kHighDecay, kDampingFreq, kModulation, kFreeze,    kWidth, kMix,   kOutput};
    for (auto i : order) {
        if (detail == patch::Detail::Changed && v[i] == specs[i].def) continue;
        auto part = std::string(specs[i].name) + " " + formatValue(i, v[i]);
        if (detail == patch::Detail::Changed && text.size() + part.size() + 2 > 300) break;
        if (!text.empty()) text += "; ";
        text += part;
    }
    return text;
}
std::uint32_t tailFor(const Values& v, double fs) SUPPLE_RT {
    if (v[kFreeze] > .5) return std::numeric_limits<std::uint32_t>::max();
    const double longest = v[kDecay] * std::max({1., v[kLowDecay], v[kHighDecay]});
    return static_cast<std::uint32_t>(
        std::min(2147483646., std::ceil((fs / 1000) * (v[kPredelay] + 1.5 * longest + 100))));
}
double decaySeconds(const Values& v, double hz, double fs) {
    // Butterworth transitions in attenuation per unit time, not in RT60 itself.
    const double f = std::max(0., hz), low = std::min(.45 * fs, v[kCrossover]),
                 high = std::min(.45 * fs, v[kDampingFreq]);
    const double wl = std::pow(low, 4) / (std::pow(low, 4) + std::pow(f, 4)),
                 wh = std::pow(f, 4) / (std::pow(high, 4) + std::pow(f, 4));
    return v[kDecay] * .001 / (1 + wl * (1 / v[kLowDecay] - 1) + wh * (1 / v[kHighDecay] - 1));
}
}  // namespace supple::reverb
