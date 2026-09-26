// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace probesynth {
enum ParamId : uint32_t {
    kWaveform,
    kCutoff,
    kResonance,
    kFilterAmount,
    kFilterSlope,
    kAmpAttack,
    kAmpDecay,
    kAmpSustain,
    kAmpRelease,
    kFilterAttack,
    kFilterDecay,
    kFilterSustain,
    kFilterRelease,
    kOutputGain,
    kSeed,
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
    {kWaveform, "Waveform", "Oscillator", 0, 3, 0, true, false, false, ""},
    {kCutoff, "Cutoff", "Filter", 20, 18000, 4000, false, false, false, "Hz"},
    {kResonance, "Resonance", "Filter", 0, 1, 0.2, false, false, false, ""},
    {kFilterAmount, "Env Amount", "Filter", -8, 8, 2, false, false, false, "oct"},
    {kFilterSlope, "Slope", "Filter", 0, 1, 0, true, false, false, ""},
    {kAmpAttack, "Amp Attack", "Amp Envelope", 0.1, 10000, 5, false, false, false, "ms"},
    {kAmpDecay, "Amp Decay", "Amp Envelope", 0.1, 10000, 100, false, false, false, "ms"},
    {kAmpSustain, "Amp Sustain", "Amp Envelope", 0, 1, 0.7, false, false, false, ""},
    {kAmpRelease, "Amp Release", "Amp Envelope", 0.1, 10000, 200, false, false, false, "ms"},
    {kFilterAttack, "Filter Attack", "Filter Envelope", 0.1, 10000, 5, false, false, false, "ms"},
    {kFilterDecay, "Filter Decay", "Filter Envelope", 0.1, 10000, 150, false, false, false, "ms"},
    {kFilterSustain, "Filter Sustain", "Filter Envelope", 0, 1, 0.2, false, false, false, ""},
    {kFilterRelease, "Filter Release", "Filter Envelope", 0.1, 10000, 250, false, false, false,
     "ms"},
    {kOutputGain, "Output Gain", "Output", -60, 12, -12, false, false, false, "dB"},
    {kSeed, "Noise Seed", "Oscillator", 1, 65535, 1, true, false, false, ""},
    {kOutputMeter, "Output Level", "Meters", -120, 6, -120, false, true, false, "dBFS"},
};
inline constexpr int kParamCount = static_cast<int>(sizeof(kParams) / sizeof(kParams[0]));
inline const ParamDef* findParam(uint32_t id) { return id < kNumParams ? &kParams[id] : nullptr; }
inline double toNormalised(const ParamDef& p, double v) {
    return std::clamp((v - p.min) / (p.max - p.min), 0.0, 1.0);
}
inline double fromNormalised(const ParamDef& p, double t) {
    return p.min + std::clamp(t, 0.0, 1.0) * (p.max - p.min);
}
inline void formatValue(uint32_t id, double v, char* out, size_t size) {
    const auto* p = findParam(id);
    if (!p) {
        std::snprintf(out, size, "%.2f", v);
        return;
    }
    if (id == kWaveform) {
        constexpr const char* names[] = {"Saw", "Square", "Sine", "Noise"};
        std::snprintf(out, size, "%s", names[std::clamp(static_cast<int>(std::round(v)), 0, 3)]);
    } else if (id == kFilterSlope)
        std::snprintf(out, size, "%s", v >= 0.5 ? "24 dB" : "12 dB");
    else if (p->stepped)
        std::snprintf(out, size, "%.0f", v);
    else
        std::snprintf(out, size, "%.2f %s", v, p->unit);
}
inline void formatCompact(uint32_t id, double v, char* out, size_t size) {
    formatValue(id, v, out, size);
}
inline bool parseValue(uint32_t id, const char* text, double& out) {
    const auto* p = findParam(id);
    if (!p || !text) return false;
    if (id == kWaveform) {
        constexpr const char* names[] = {"Saw", "Square", "Sine", "Noise"};
        for (int i = 0; i < 4; ++i)
            if (!std::strcmp(text, names[i])) {
                out = i;
                return true;
            }
    }
    if (id == kFilterSlope) {
        if (!std::strcmp(text, "12 dB")) {
            out = 0;
            return true;
        }
        if (!std::strcmp(text, "24 dB")) {
            out = 1;
            return true;
        }
    }
    char* end = nullptr;
    const double v = std::strtod(text, &end);
    if (end == text || !std::isfinite(v)) return false;
    while (*end == ' ') ++end;
    if (*end && std::strcmp(end, p->unit)) return false;
    out = std::clamp(p->stepped ? std::round(v) : v, p->min, p->max);
    return true;
}
}  // namespace probesynth
