// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "analyzer/params.h"

namespace supple::analyzer {
namespace {
using namespace params;
constexpr Choice windows[] = {
    {"s3", "3 s"}, {"s10", "10 s"}, {"s30", "30 s"}, {"since_reset", "Since reset"}};
constexpr std::string_view timeTags[] = {"role:time"}, modeTags[] = {"role:mode"},
                           levelTags[] = {"role:level"}, widthTags[] = {"role:width"},
                           brightTags[] = {"role:brightness"};
constexpr ParamSpec specs[] = {
    {.id = kWindow,
     .key = "window",
     .name = "Window",
     .group = "Analysis",
     .type = Type::Enum,
     .max = 3,
     .choices = windows,
     .tags = timeTags,
     .description = "Measurement horizon; a change starts fresh history."},
    {.id = kReset,
     .key = "reset",
     .name = "Reset",
     .group = "Analysis",
     .type = Type::Bool,
     .tags = modeTags,
     .description = "Momentary trigger: clears measurement history and returns to zero."},
    {.id = kHold,
     .key = "hold",
     .name = "Hold",
     .group = "Analysis",
     .type = Type::Bool,
     .tags = modeTags,
     .description = "Freeze measurements while audio continues unchanged."},
    {.id = kIntegrated,
     .key = "integrated",
     .name = "Integrated",
     .group = "Meters/Loudness",
     .unit = Unit::LUFS,
     .min = -120,
     .max = 40,
     .def = -120,
     .tags = levelTags,
     .description = "Gated integrated loudness (LUFS).",
     .flags = kReadonly,
     .smoothingMs = 20},
    {.id = kShortTerm,
     .key = "short_term",
     .name = "Short-term",
     .group = "Meters/Loudness",
     .unit = Unit::LUFS,
     .min = -120,
     .max = 40,
     .def = -120,
     .tags = levelTags,
     .description = "Three-second loudness (LUFS).",
     .flags = kReadonly,
     .smoothingMs = 20},
    {.id = kMomentary,
     .key = "momentary",
     .name = "Momentary",
     .group = "Meters/Loudness",
     .unit = Unit::LUFS,
     .min = -120,
     .max = 40,
     .def = -120,
     .tags = levelTags,
     .description = "400 ms loudness (LUFS).",
     .flags = kReadonly,
     .smoothingMs = 20},
    {.id = kLra,
     .key = "lra",
     .name = "Loudness Range",
     .group = "Meters/Loudness",
     .unit = Unit::LU,
     .max = 120,
     .tags = levelTags,
     .description = "Loudness range (LU).",
     .flags = kReadonly,
     .smoothingMs = 20},
    {.id = kTruePeak,
     .key = "true_peak",
     .name = "True Peak",
     .group = "Meters/Loudness",
     .unit = Unit::dB,
     .min = -120,
     .max = 40,
     .def = -120,
     .tags = levelTags,
     .description = "Reconstructed peak (dBTP).",
     .flags = kReadonly,
     .smoothingMs = 20},
    {.id = kCorrelation,
     .key = "correlation",
     .name = "Correlation",
     .group = "Meters/Stereo",
     .min = -1,
     .max = 1,
     .def = 1,
     .tags = widthTags,
     .description = "Pearson stereo correlation, silence is +1.",
     .flags = kReadonly,
     .smoothingMs = 20},
    {.id = kWidth,
     .key = "width",
     .name = "Width",
     .group = "Meters/Stereo",
     .unit = Unit::percent,
     .max = 100,
     .tags = widthTags,
     .description = "Side energy as percent of mid plus side energy.",
     .flags = kReadonly,
     .smoothingMs = 20},
    {.id = kBalance,
     .key = "balance",
     .name = "Balance",
     .group = "Meters/Stereo",
     .unit = Unit::dB,
     .min = -120,
     .max = 120,
     .tags = widthTags,
     .description = "Right/left power balance, positive is right.",
     .flags = kReadonly,
     .smoothingMs = 20},
    {.id = kCentroid,
     .key = "centroid",
     .name = "Centroid",
     .group = "Meters/Spectrum",
     .unit = Unit::Hz,
     .max = 20000,
     .tags = brightTags,
     .description = "Power-weighted mid-signal centroid from 20 Hz to 20 kHz.",
     .flags = kReadonly,
     .smoothingMs = 20}};
}  // namespace
params::Table table() { return specs; }
Values defaults() {
    Values v{};
    for (const auto& p : specs) v[p.id] = p.def;
    return v;
}
const patch::FlatPatchType& patchType() {
    static const patch::FlatPatchType type("analyzer", 1, 0, specs, false);
    return type;
}
double windowSeconds(double choice) {
    constexpr double seconds[]{3, 10, 30, 0};
    return seconds[static_cast<int>(params::clampPhysical(specs[kWindow], choice))];
}
std::string describe(const Values& v, bool changedOnly) {
    std::string text = std::string(kName) + ": ";
    bool changed = false;
    for (std::uint32_t id = 0; id <= kHold; ++id) {
        if (changedOnly && v[id] == specs[id].def) continue;
        if (changed) text += "; ";
        text += std::string(specs[id].name) + " " + params::format(specs[id], v[id]);
        changed = true;
    }
    if (!changed) text += "default measurement window";
    return text;
}
}  // namespace supple::analyzer
