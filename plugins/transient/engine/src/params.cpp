#include "transient/params.h"
namespace supple::transient {
namespace {
using namespace params;
using units::Unit;
constexpr std::string_view level[] = {"role:level"}, time[] = {"role:time"}, mix[] = {"role:mix"};
constexpr ParamSpec specs[] = {
    {.id = kAttack,
     .key = "attack",
     .name = "Attack",
     .group = "Shape",
     .unit = Unit::percent,
     .min = -100,
     .max = 100,
     .tags = level,
     .description = "Onset boost or cut, up to 12 dB.",
     .smoothingMs = 20},
    {.id = kSustain,
     .key = "sustain",
     .name = "Sustain",
     .group = "Shape",
     .unit = Unit::percent,
     .min = -100,
     .max = 100,
     .tags = level,
     .description = "Decay boost or cut, up to 12 dB.",
     .smoothingMs = 20},
    {.id = kSpeed,
     .key = "speed",
     .name = "Speed",
     .group = "Detector",
     .unit = Unit::ms,
     .min = 1,
     .max = 50,
     .def = 10,
     .curve = {CurveKind::Log},
     .tags = time,
     .description = "Fast release and slow attack time constant.",
     .smoothingMs = 20},
    {.id = kSensitivity,
     .key = "sensitivity",
     .name = "Sensitivity",
     .group = "Detector",
     .unit = Unit::percent,
     .max = 100,
     .def = 50,
     .tags = level,
     .description = "Scales the fast/slow envelope difference.",
     .smoothingMs = 20},
    {.id = kClip,
     .key = "clip",
     .name = "Clip",
     .group = "Output",
     .type = Type::Bool,
     .description = "2x tanh soft clip; 128 samples latency, restart required.",
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
     .description = "Final gain after shaping, clipping and mix.",
     .smoothingMs = 20},
    {.id = kInputLevel,
     .key = "input_level",
     .name = "Input Level",
     .group = "Meters",
     .unit = Unit::dB,
     .min = -120,
     .max = 40,
     .def = -120,
     .tags = level,
     .description = "Input sample peak, dBFS.",
     .flags = kReadonly},
    {.id = kOutputLevel,
     .key = "output_level",
     .name = "Output Level",
     .group = "Meters",
     .unit = Unit::dB,
     .min = -120,
     .max = 40,
     .def = -120,
     .tags = level,
     .description = "Output sample peak, dBFS.",
     .flags = kReadonly},
    {.id = kGain,
     .key = "gain",
     .name = "Shaping Gain",
     .group = "Meters",
     .unit = Unit::dB,
     .min = -12,
     .max = 12,
     .tags = level,
     .description = "Mean shaping gain since the previous report, dB.",
     .flags = kReadonly},
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
    static const patch::FlatPatchType p("transient", 1, 0, specs, false);
    return p;
}
std::string describe(const Values& v, patch::Detail detail) {
    std::string text;
    for (std::size_t i = 0; i < kInputLevel; ++i) {
        if (detail == patch::Detail::Changed && v[i] == specs[i].def) continue;
        auto part = std::string(specs[i].name) + " " + formatValue(i, v[i]);
        if (detail == patch::Detail::Changed && text.size() + part.size() + 2 > 300) break;
        if (!text.empty()) text += "; ";
        text += part;
    }
    return text;
}
std::uint32_t latencyFor(const Values& v) SUPPLE_RT { return v[kClip] > .5 ? kClipLatency : 0; }
}  // namespace supple::transient
