// SPDX-License-Identifier: MIT
#pragma once
#include <array>
#include "supple/params/params.h"
#include "supple/patch/patch.h"
namespace supple::drive {
inline constexpr char kDisplayName[] = SUPPLE_DRIVE_DISPLAY_NAME;
enum ParamId : uint32_t {
    kCurve,
    kDrive,
    kBias,
    kTone,
    kOversampling,
    kAutoGain,
    kMix,
    kOutput,
    kInputMeter,
    kOutputMeter,
    kNumParams
};
using Values = std::array<double, kNumParams>;
using ParamDef = params::ParamSpec;
namespace pa = params;
namespace un = units;
inline constexpr pa::Choice kCurveChoices[] = {
    {"tape", "Tape"},   {"tube", "Tube"}, {"transistor", "Transistor"},
    {"diode", "Diode"}, {"fold", "Fold"}, {"hard", "Hard"}};
inline constexpr pa::Choice kOversamplingChoices[] = {
    {"x1", "1x"}, {"x2", "2x"}, {"x4", "4x"}, {"x8", "8x"}};
inline constexpr std::string_view kLevelTags[] = {"role:level"};
inline constexpr std::string_view kToneTags[] = {"role:brightness"};
inline constexpr std::string_view kModeTags[] = {"role:mode"};
inline constexpr pa::ParamSpec kParams[] = {
    {.id = kCurve,
     .key = "curve",
     .name = "Curve",
     .group = "Character",
     .type = pa::Type::Enum,
     .min = 0,
     .max = 5,
     .def = 0,
     .choices = kCurveChoices,
     .tags = kModeTags,
     .description = "Static waveshaper; 10 ms curve crossfade."},
    {.id = kDrive,
     .key = "drive",
     .name = "Drive",
     .group = "Input",
     .unit = un::Unit::dB,
     .min = 0,
     .max = 36,
     .def = 6,
     .tags = kLevelTags,
     .description = "Input drive before saturation.",
     .smoothingMs = 20},
    {.id = kBias,
     .key = "bias",
     .name = "Bias",
     .group = "Character",
     .unit = un::Unit::percent,
     .min = -100,
     .max = 100,
     .def = 0,
     .tags = kModeTags,
     .description = "DC offset before saturation; DC removed afterwards.",
     .smoothingMs = 20},
    {.id = kTone,
     .key = "tone",
     .name = "Tone",
     .group = "Character",
     .unit = un::Unit::dB,
     .min = -6,
     .max = 6,
     .def = 0,
     .tags = kToneTags,
     .description = "Tilt before saturation and its inverse afterwards (dB/oct).",
     .smoothingMs = 20},
    {.id = kOversampling,
     .key = "oversampling",
     .name = "Oversampling",
     .group = "Quality",
     .type = pa::Type::Enum,
     .min = 0,
     .max = 3,
     .def = 2,
     .choices = kOversamplingChoices,
     .tags = kModeTags,
     .description = "Oversampling quality; changes require host restart.",
     .flags = 0},
    {.id = kAutoGain,
     .key = "auto_gain",
     .name = "Auto gain",
     .group = "Output",
     .type = pa::Type::Bool,
     .min = 0,
     .max = 1,
     .def = 1,
     .tags = kLevelTags,
     .description = "RMS compensation for Gaussian noise at -18 dBFS RMS."},
    {.id = kMix,
     .key = "mix",
     .name = "Mix",
     .group = "Output",
     .unit = un::Unit::percent,
     .min = 0,
     .max = 100,
     .def = 100,
     .tags = kLevelTags,
     .description = "Latency aligned dry/wet blend.",
     .smoothingMs = 20},
    {.id = kOutput,
     .key = "output",
     .name = "Output",
     .group = "Output",
     .unit = un::Unit::dB,
     .min = -24,
     .max = 24,
     .def = 0,
     .tags = kLevelTags,
     .description = "Final output gain.",
     .smoothingMs = 20},
    {.id = kInputMeter,
     .key = "input_level",
     .name = "Input level",
     .group = "Meters",
     .unit = un::Unit::dB,
     .min = -120,
     .max = 40,
     .def = -120,
     .tags = kLevelTags,
     .description = "Input sample peak.",
     .flags = pa::kReadonly},
    {.id = kOutputMeter,
     .key = "output_level",
     .name = "Output level",
     .group = "Meters",
     .unit = un::Unit::dB,
     .min = -120,
     .max = 40,
     .def = -120,
     .tags = kLevelTags,
     .description = "Output sample peak.",
     .flags = pa::kReadonly},
};
inline constexpr int kParamCount = kNumParams;
inline const ParamDef* findParam(uint32_t id) { return pa::findById(kParams, id); }
inline double toNormalised(const ParamDef& p, double v) { return pa::toNormalized(p, v); }
inline double fromNormalised(const ParamDef& p, double v) { return pa::fromNormalized(p, v); }
const patch::FlatPatchType& patchType();
Values defaults();
std::string describe(const Values&, bool changedOnly = true);
}  // namespace supple::drive
