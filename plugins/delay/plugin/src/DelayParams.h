// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// One parameter definition table shared by the wrapper and editor.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace delay {

// Parameter IDs are session-file ABI. Never reorder or recycle an ID. Retire
// old slots in place and append new parameters before kNumParams.
enum ParamId : uint32_t {
    kInputGain = 0,
    kOutputGain,
    kMix,
    kBypass,
    kInputMeter,
    kOutputMeter,
    kNumParams
};

struct ParamDef {
    uint32_t id;
    const char* name;
    const char* module;
    double min, max, def;
    bool stepped, readonly, isBypass;
    const char* unit;
};

inline constexpr ParamDef kParams[] = {
    {kInputGain,  "Input Gain",  "",      -24.0, 24.0, 0.0, false, false, false, "dB"},
    {kOutputGain, "Output Gain", "",      -24.0, 24.0, 0.0, false, false, false, "dB"},
    {kMix,        "Mix",         "",        0.0,  1.0, 1.0, false, false, false, "%"},
    {kBypass,     "Bypass",      "",        0.0,  1.0, 0.0, true,  false, true,  ""},
    {kInputMeter, "Input Level", "Meters", -120.0,  6.0, -120.0, false, true, false, "dBFS"},
    {kOutputMeter,"Output Level","Meters", -120.0,  6.0, -120.0, false, true, false, "dBFS"},
};
inline constexpr int kParamCount = static_cast<int>(sizeof(kParams) / sizeof(kParams[0]));

inline const ParamDef* findParam(uint32_t id) {
    for (const auto& p : kParams) if (p.id == id) return &p;
    return nullptr;
}

inline double toNormalised(const ParamDef& p, double v) {
    return std::clamp((v - p.min) / (p.max - p.min), 0.0, 1.0);
}
inline double fromNormalised(const ParamDef& p, double t) {
    return p.min + std::clamp(t, 0.0, 1.0) * (p.max - p.min);
}

inline void formatValue(uint32_t id, double v, char* out, size_t size) {
    const ParamDef* p = findParam(id);
    if (!p) { std::snprintf(out, size, "%.2f", v); return; }
    if (id == kBypass) std::snprintf(out, size, "%s", v > 0.5 ? "Bypassed" : "Active");
    else if (!std::strcmp(p->unit, "%")) std::snprintf(out, size, "%.0f %%", v * 100.0);
    else std::snprintf(out, size, "%+.2f %s", v, p->unit);
}

inline void formatCompact(uint32_t id, double v, char* out, size_t size) {
    if (id == kMix) std::snprintf(out, size, "%.0f", v * 100.0);
    else std::snprintf(out, size, "%+.1f", v);
}

inline bool parseValue(uint32_t id, const char* text, double& out) {
    const ParamDef* p = findParam(id);
    if (!p || !text) return false;
    if (id == kBypass) {
        if (!std::strcmp(text, "Active")) { out = 0.0; return true; }
        if (!std::strcmp(text, "Bypassed")) { out = 1.0; return true; }
    }
    char* end = nullptr;
    double v = std::strtod(text, &end);
    if (end == text || !std::isfinite(v)) return false;
    if (id == kMix && std::strchr(text, '%')) v *= 0.01;
    out = std::clamp(v, p->min, p->max);
    return true;
}

}  // namespace delay
