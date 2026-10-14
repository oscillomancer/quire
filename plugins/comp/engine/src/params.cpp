// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "comp/params.h"
#include <cmath>

namespace supple::comp {
namespace {
using namespace params;
using units::Unit;
constexpr Choice modes[] = {{"compressor", "Compressor"}, {"limiter", "Limiter"}};
constexpr Choice detectors[] = {{"peak", "Peak"}, {"rms", "RMS"}};
constexpr Choice topologies[] = {{"feedforward", "Feedforward"}, {"feedback", "Feedback"}};
constexpr Choice keys[] = {{"internal", "Internal"}, {"external", "External"}};
constexpr Choice ahead[] = {{"off", "Off"}, {"ms1", "1 ms"}, {"ms5", "5 ms"}};
constexpr std::string_view level[] = {"role:level"}, time[] = {"role:time"}, mode[] = {"role:mode"},
                           threshold[] = {"role:threshold"}, ratio[] = {"role:ratio"},
                           frequency[] = {"role:frequency"}, mix[] = {"role:mix"};
constexpr ParamSpec specs[] = {
    {.id = kMode,
     .key = "mode",
     .name = "Mode",
     .group = "Detector",
     .type = Type::Enum,
     .max = 1,
     .choices = modes,
     .tags = mode,
     .description = "Compressor or ceiling limiter; restart when latency changes.",
     .flags = 0},
    {.id = kThreshold,
     .key = "threshold",
     .name = "Threshold",
     .group = "Detector",
     .unit = Unit::dB,
     .min = -60,
     .max = 0,
     .def = -18,
     .tags = threshold,
     .description = "Compressor threshold.",
     .smoothingMs = 20},
    {.id = kRatio,
     .key = "ratio",
     .name = "Ratio",
     .group = "Detector",
     .unit = Unit::ratio,
     .min = 1,
     .max = 100,
     .def = 4,
     .curve = {CurveKind::Pow, 3},
     .tags = ratio,
     .description = "Compression ratio; 100 means infinity.",
     .smoothingMs = 20},
    {.id = kKnee,
     .key = "knee",
     .name = "Knee",
     .group = "Detector",
     .unit = Unit::dB,
     .max = 24,
     .def = 6,
     .tags = level,
     .description = "Full quadratic knee width.",
     .smoothingMs = 20},
    {.id = kAttack,
     .key = "attack",
     .name = "Attack",
     .group = "Detector",
     .unit = Unit::ms,
     .min = .01,
     .max = 300,
     .def = 10,
     .curve = {CurveKind::Log},
     .tags = time,
     .description = "63-percent attack time.",
     .smoothingMs = 20},
    {.id = kRelease,
     .key = "release",
     .name = "Release",
     .group = "Detector",
     .unit = Unit::ms,
     .min = 5,
     .max = 5000,
     .def = 150,
     .curve = {CurveKind::Log},
     .tags = time,
     .description = "63-percent release time.",
     .smoothingMs = 20},
    {.id = kAutoRelease,
     .key = "auto_release",
     .name = "Auto Release",
     .group = "Detector",
     .type = Type::Bool,
     .tags = mode,
     .description = "Fast/slow program-dependent release."},
    {.id = kDetector,
     .key = "detector",
     .name = "Detector",
     .group = "Detector",
     .type = Type::Enum,
     .max = 1,
     .choices = detectors,
     .tags = mode,
     .description = "Sample peak or exponential mean-square detector."},
    {.id = kRmsWindow,
     .key = "rms_window",
     .name = "RMS Window",
     .group = "Detector",
     .unit = Unit::ms,
     .min = 1,
     .max = 100,
     .def = 10,
     .curve = {CurveKind::Log},
     .tags = time,
     .description = "Mean-square averaging time constant.",
     .smoothingMs = 20},
    {.id = kTopology,
     .key = "topology",
     .name = "Topology",
     .group = "Detector",
     .type = Type::Enum,
     .max = 1,
     .choices = topologies,
     .tags = mode,
     .description = "Main input or previous compressed wet output."},
    {.id = kSidechain,
     .key = "sidechain",
     .name = "Sidechain",
     .group = "Sidechain",
     .type = Type::Enum,
     .max = 1,
     .choices = keys,
     .tags = mode,
     .description = "Internal detector or external stereo key."},
    {.id = kScHighpass,
     .key = "sc_highpass",
     .name = "SC Highpass",
     .group = "Sidechain",
     .unit = Unit::Hz,
     .min = 10,
     .max = 500,
     .def = 10,
     .curve = {CurveKind::Log},
     .tags = frequency,
     .description = "12 dB/oct key highpass; minimum disables it.",
     .flags = kAutomatable,
     .smoothingMs = 20},
    {.id = kScLowpass,
     .key = "sc_lowpass",
     .name = "SC Lowpass",
     .group = "Sidechain",
     .unit = Unit::Hz,
     .min = 1000,
     .max = 20000,
     .def = 20000,
     .curve = {CurveKind::Log},
     .tags = frequency,
     .description = "12 dB/oct key lowpass; maximum disables it.",
     .smoothingMs = 20},
    {.id = kScListen,
     .key = "sc_listen",
     .name = "SC Listen",
     .group = "Sidechain",
     .type = Type::Bool,
     .tags = mode,
     .description = "Audition the filtered key."},
    {.id = kStereoLink,
     .key = "stereo_link",
     .name = "Stereo Link",
     .group = "Sidechain",
     .unit = Unit::percent,
     .max = 100,
     .def = 100,
     .tags = mix,
     .description = "Linked maximum versus each channel's detector.",
     .smoothingMs = 20},
    {.id = kLookahead,
     .key = "lookahead",
     .name = "Lookahead",
     .group = "Detector",
     .type = Type::Enum,
     .max = 2,
     .choices = ahead,
     .tags = time,
     .description = "Audio and dry-path delay; requires restart.",
     .flags = 0},
    {.id = kMakeup,
     .key = "makeup",
     .name = "Makeup",
     .group = "Output",
     .unit = Unit::dB,
     .min = -12,
     .max = 24,
     .tags = level,
     .description = "Wet makeup gain.",
     .smoothingMs = 20},
    {.id = kAutoMakeup,
     .key = "auto_makeup",
     .name = "Auto Makeup",
     .group = "Output",
     .type = Type::Bool,
     .tags = mode,
     .description = "Compensate the static reduction at -18 dBFS."},
    {.id = kCeiling,
     .key = "ceiling",
     .name = "Ceiling",
     .group = "Output",
     .unit = Unit::dB,
     .min = -24,
     .max = 0,
     .def = -1,
     .tags = level,
     .description = "Limiter output ceiling, after mix and output gain.",
     .smoothingMs = 20},
    {.id = kTruePeak,
     .key = "true_peak",
     .name = "True Peak",
     .group = "Detector",
     .type = Type::Bool,
     .def = 1,
     .tags = mode,
     .description = "4x limiter detection; requires restart if latency changes.",
     .flags = 0},
    {.id = kMix,
     .key = "mix",
     .name = "Mix",
     .group = "Output",
     .unit = Unit::percent,
     .max = 100,
     .def = 100,
     .tags = mix,
     .description = "Latency-aligned dry/wet blend.",
     .smoothingMs = 20},
    {.id = kOutput,
     .key = "output",
     .name = "Output",
     .group = "Output",
     .unit = Unit::dB,
     .min = -24,
     .max = 24,
     .tags = level,
     .description = "Final output gain before limiter safety.",
     .smoothingMs = 20},
    {.id = kGr,
     .key = "gr",
     .name = "Gain Reduction",
     .group = "Meters",
     .unit = Unit::dB,
     .min = -60,
     .max = 0,
     .tags = level,
     .description = "Deepest reduction since the last report.",
     .flags = kReadonly,
     .smoothingMs = 20},
};
}  // namespace
params::Table table() { return specs; }
std::string formatValue(std::size_t i, double v, bool compact) {
    if (i >= kNumParams) return {};
    if (i == kRatio && v >= 100) return "∞";
    if ((i == kScHighpass && v <= 10) || (i == kScLowpass && v >= 20000)) return "Off";
    return compact ? params::formatCompact(specs[i], v) : params::format(specs[i], v);
}
Values defaults() {
    Values v{};
    for (std::size_t i = 0; i < v.size(); ++i) v[i] = specs[i].def;
    return v;
}
const patch::FlatPatchType& patchType() {
    static const patch::FlatPatchType t("comp", 1, 0, specs, false);
    return t;
}
std::string describe(const Values& v, patch::Detail detail) {
    std::string text;
    for (std::size_t i = 0; i < v.size(); ++i) {
        const auto& p = specs[i];
        if ((p.flags & params::kReadonly) || (detail == patch::Detail::Changed && v[i] == p.def))
            continue;
        std::string part = std::string(p.name) + " " + formatValue(i, v[i]);
        if (detail == patch::Detail::Changed && text.size() + part.size() + 2 > 300) break;
        if (!text.empty()) text += "; ";
        text += part;
    }
    return text;
}
double gainReduction(double x, double t, double r, double w) SUPPLE_RT {
    return reductionFromSlope(x, t, (r >= 100 ? 0 : 1 / r) - 1, w);
}
double reductionFromSlope(double x, double t, double slope, double w) SUPPLE_RT {
    if (x <= t - w / 2) return 0;
    if (w <= 0 || x >= t + w / 2) return slope * (x - t);
    const double d = x - t + w / 2;
    return slope * d * d / (2 * w);
}
std::uint32_t latencyFor(const Values& v, double sr) SUPPLE_RT {
    const double ms = v[kLookahead] < .5 ? 0 : v[kLookahead] < 1.5 ? 1 : 5;
    return static_cast<std::uint32_t>(std::lround(ms * .001 * sr)) +
           (v[kMode] > .5 && v[kTruePeak] > .5 ? 16u : 0u);
}
}  // namespace supple::comp
