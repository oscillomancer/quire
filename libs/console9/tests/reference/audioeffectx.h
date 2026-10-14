// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Original test stand-in, derived only from the Console9 source's uses.
// Not an SDK, plug-in ABI, or host implementation. Never used outside tests.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

using VstInt32 = int32_t;
using audioMasterCallback = void (*)();
enum VstPlugCategory { kPlugCategEffect };
inline constexpr int kVstMaxProgNameLen = 32, kVstMaxParamStrLen = 32, kVstMaxProductStrLen = 64,
                     kVstMaxVendorStrLen = 64;

class AudioEffect {
  public:
    virtual ~AudioEffect() = default;
};
class AudioEffectX : public AudioEffect {
  public:
    AudioEffectX(audioMasterCallback, int, int) {}
    void setNumInputs(int) {}
    void setNumOutputs(int) {}
    void setUniqueID(unsigned long) {}
    void canProcessReplacing() {}
    void canDoubleReplacing() {}
    void programsAreChunks(bool) {}
};
inline void vst_strncpy(char* to, const char* from, int length) {
    std::snprintf(to, size_t(length), "%s", from);
}
inline void float2string(float value, char* to, int length) {
    std::snprintf(to, size_t(length), "%.6g", double(value));
}
