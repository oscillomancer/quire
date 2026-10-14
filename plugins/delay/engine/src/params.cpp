// SPDX-License-Identifier: MIT
#include "delay/params.h"
#include <algorithm>
#include <cmath>
#include <limits>
namespace supple::delay {
namespace {
using namespace params;
using units::Unit;
constexpr Choice notes[] = {{"off", "off"},      {"n8_1", "8/1"},     {"n4_1", "4/1"},
                            {"n2_1", "2/1"},     {"n1_1", "1/1"},     {"n1_2", "1/2"},
                            {"n1_2d", "1/2."},   {"n1_2t", "1/2t"},   {"n1_4", "1/4"},
                            {"n1_4d", "1/4."},   {"n1_4t", "1/4t"},   {"n1_8", "1/8"},
                            {"n1_8d", "1/8."},   {"n1_8t", "1/8t"},   {"n1_16", "1/16"},
                            {"n1_16d", "1/16."}, {"n1_16t", "1/16t"}, {"n1_32", "1/32"}};
constexpr Choice modes[] = {{"stereo", "Stereo"}, {"ping_pong", "Ping-pong"}, {"mono", "Mono"}};
constexpr std::string_view depth[] = {"role:depth"};
constexpr std::string_view drive[] = {"role:drive"};
constexpr std::string_view frequency[] = {"role:frequency"};
constexpr std::string_view level[] = {"role:level"};
constexpr std::string_view mix[] = {"role:mix"};
constexpr std::string_view mode[] = {"role:mode"};
constexpr std::string_view time[] = {"role:time"};
constexpr std::string_view width[] = {"role:width"};
constexpr ParamSpec specs[] = {
    {.id = kSync,
     .key = "sync",
     .name = "Sync",
     .group = "Timing",
     .type = Type::Enum,
     .max = 17,
     .def = 12,
     .choices = notes,
     .tags = mode,
     .description = "Transport note length, or free-running time."},
    {.id = kTime,
     .key = "time",
     .name = "Free time",
     .group = "Timing",
     .unit = Unit::ms,
     .min = 1,
     .max = 5000,
     .def = 375,
     .curve = {CurveKind::Log},
     .tags = time,
     .description = "Free-running delay time.",
     .smoothingMs = 20},
    {.id = kMode,
     .key = "mode",
     .name = "Mode",
     .group = "Timing",
     .type = Type::Enum,
     .max = 2,
     .def = 0,
     .choices = modes,
     .tags = mode,
     .description = "Independent, alternating or mono echoes."},
    {.id = kOffset,
     .key = "offset",
     .name = "Offset",
     .group = "Timing",
     .unit = Unit::percent,
     .min = -50,
     .max = 50,
     .def = 0,
     .curve = {CurveKind::Linear},
     .tags = time,
     .description = "Right time offset relative to left.",
     .smoothingMs = 20},
    {.id = kFeedback,
     .key = "feedback",
     .name = "Feedback",
     .group = "Loop",
     .unit = Unit::percent,
     .min = 0,
     .max = 110,
     .def = 35,
     .curve = {CurveKind::Linear},
     .tags = level,
     .description = "Loop gain; above 100 percent self-oscillates safely.",
     .smoothingMs = 20},
    {.id = kHighpass,
     .key = "highpass",
     .name = "High-pass",
     .group = "Loop",
     .unit = Unit::Hz,
     .min = 20,
     .max = 2000,
     .def = 80,
     .curve = {CurveKind::Log},
     .tags = frequency,
     .description = "Feedback high-pass cutoff.",
     .smoothingMs = 20},
    {.id = kLowpass,
     .key = "lowpass",
     .name = "Low-pass",
     .group = "Loop",
     .unit = Unit::Hz,
     .min = 1000,
     .max = 20000,
     .def = 8000,
     .curve = {CurveKind::Log},
     .tags = frequency,
     .description = "Feedback low-pass cutoff.",
     .smoothingMs = 20},
    {.id = kModRate,
     .key = "mod_rate",
     .name = "Wow rate",
     .group = "Wow",
     .unit = Unit::Hz,
     .min = 0.05,
     .max = 10,
     .def = 0.5,
     .curve = {CurveKind::Log},
     .tags = frequency,
     .description = "Sine and smooth random modulation rate.",
     .smoothingMs = 20},
    {.id = kModDepth,
     .key = "mod_depth",
     .name = "Wow depth",
     .group = "Wow",
     .unit = Unit::percent,
     .min = 0,
     .max = 100,
     .def = 0,
     .curve = {CurveKind::Linear},
     .tags = depth,
     .description = "Three millisecond maximum modulation depth.",
     .smoothingMs = 20},
    {.id = kDrive,
     .key = "drive",
     .name = "Drive",
     .group = "Loop",
     .unit = Unit::dB,
     .min = 0,
     .max = 24,
     .def = 0,
     .curve = {CurveKind::Linear},
     .tags = drive,
     .description = "Loop saturation; exactly bypassed at zero.",
     .smoothingMs = 20},
    {.id = kDuck,
     .key = "duck",
     .name = "Duck",
     .group = "Output",
     .unit = Unit::dB,
     .min = 0,
     .max = 24,
     .def = 0,
     .curve = {CurveKind::Linear},
     .tags = level,
     .description = "Maximum wet ducking under the input.",
     .smoothingMs = 20},
    {.id = kWidth,
     .key = "width",
     .name = "Width",
     .group = "Output",
     .unit = Unit::percent,
     .min = 0,
     .max = 200,
     .def = 100,
     .curve = {CurveKind::Linear},
     .tags = width,
     .description = "Echo side gain; ignored in mono mode.",
     .smoothingMs = 20},
    {.id = kFreeze,
     .key = "freeze",
     .name = "Freeze",
     .group = "Loop",
     .type = Type::Bool,
     .tags = mode,
     .description = "Hold the existing loop without new input or losses.",
     .smoothingMs = 10},
    {.id = kMix,
     .key = "mix",
     .name = "Mix",
     .group = "Output",
     .unit = Unit::percent,
     .min = 0,
     .max = 100,
     .def = 25,
     .curve = {CurveKind::Linear},
     .tags = mix,
     .description = "Linear dry/wet balance.",
     .smoothingMs = 20},
    {.id = kOutput,
     .key = "output",
     .name = "Output",
     .group = "Output",
     .unit = Unit::dB,
     .min = -24,
     .max = 24,
     .def = 0,
     .curve = {CurveKind::Linear},
     .tags = level,
     .description = "Final output gain.",
     .smoothingMs = 20},
};
}  // namespace
params::Table table() { return specs; }
Values defaults() {
    Values v{};
    for (const auto& p : specs) v[p.id] = p.def;
    return v;
}
const patch::FlatPatchType& patchType() {
    static const patch::FlatPatchType p{"delay", 1, 0, table(), false};
    return p;
}
std::string describe(std::span<const double> v, bool changedOnly) {
    std::string out;
    for (const auto& p : specs) {
        if (p.id >= v.size() || (changedOnly && v[p.id] == p.def)) continue;
        const auto term = std::string(p.name) + " " + params::format(p, v[p.id]);
        if (changedOnly && out.size() + term.size() + (out.empty() ? 0 : 2) > 300) continue;
        if (!out.empty()) out += "; ";
        out += term;
    }
    return out.empty() ? "Default tempo-synced stereo echoes." : out;
}
std::array<double, 2> lineTimes(const Values& v, double tempo) SUPPLE_RT {
    constexpr double beats[]{0,   32,     16, 8,   4,      2,   3,    4. / 3, 1,
                             1.5, 2. / 3, .5, .75, 1. / 3, .25, .375, 1. / 6, .125};
    tempo = std::isfinite(tempo) && tempo > 0 ? tempo : 120;
    const auto sync = static_cast<size_t>(std::clamp(v[kSync], 0., 17.));
    const double base = sync ? 60000 / tempo * beats[sync] : v[kTime];
    return {std::clamp(base, 1., 5000.), std::clamp(base * (1 + v[kOffset] / 100), 1., 5000.)};
}
uint32_t tailSamples(const Values& v, double fs, double tempo) SUPPLE_RT {
    constexpr auto infinite = uint32_t(std::numeric_limits<int32_t>::max());
    if (v[kFreeze] != 0 || v[kFeedback] >= 100) return infinite;
    const auto times = lineTimes(v, tempo);
    const double fb = v[kFeedback] / 100;
    const double repeats = fb > 0 ? std::max(1., std::ceil(std::log(1e-4) / std::log(fb))) : 1;
    return static_cast<uint32_t>(std::min(
        double(infinite), std::ceil(fs * (std::max(times[0], times[1]) * repeats + 50) / 1000)));
}
double safetyLimit(double x) SUPPLE_RT {
    const double a = std::abs(x);
    return a <= .5 ? x : std::copysign(.5 + .5 * std::tanh(2 * (a - .5)), x);
}
double loopMagnitude(double hz, double hp, double lp, double fs) {
    hz = std::clamp(hz, 0., fs * .499999);
    const double w = std::tan(3.141592653589793 * hz / fs);
    const double h = std::tan(3.141592653589793 * std::min(hp, fs * .45) / fs);
    const double l = std::tan(3.141592653589793 * std::min(lp, fs * .45) / fs);
    const double wh = w / h, wl = w / l;
    return wh * wh / std::sqrt(1 + std::pow(wh, 4)) / std::sqrt(1 + std::pow(wl, 4));
}
}  // namespace supple::delay
