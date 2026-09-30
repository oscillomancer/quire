// SPDX-License-Identifier: MIT
#pragma once
#include <cstdint>
#include "supple/base/rt.h"

namespace supple::console9 {
// Physical gain is clamped to [0,4]; -infinity is mute. NaN maps to mute.
double faderGain(double db) SUPPLE_RT;
double nativeFader(double db) SUPPLE_RT;
double faderDb(double native) SUPPLE_RT;
double nativePan(double percent) SUPPLE_RT;

struct Seeds {
    uint32_t left = 16386, right = 16387;
    bool operator==(const Seeds&) const = default;
};
// Caller supplies a stable salt identifying strip/stage/send, independent of scheduling.
Seeds seedsFor(uint64_t projectSeed, uint64_t salt) SUPPLE_RT;

namespace detail {
template <bool Encode, bool Stereo>
class Stage {
  public:
    Stage() SUPPLE_RT;
    // Reset only the RNG streams. Settings and pending ramps remain unchanged.
    // Invalid seed words are clamped to the reference constructor's lower bound.
    void reset(Seeds seeds) SUPPLE_RT;
    // Plan-level choice: preserve gains/pan, omit curves/noise/dither and RNG advances.
    // Does not alter parameters, ramps or seeds; no internal mode crossfade.
    void setLinear(bool linear) SUPPLE_RT;
    // Gain is f(2B), pan is native A. Zero ramp length applies immediately.
    // Call on the audio thread; retargeting starts from the current ramp position.
    void set(double gain, double pan = .5, uint32_t rampSamples = 0) SUPPLE_RT;
    // Native float arithmetic for reference comparisons; ramps still interpolate linear gain.
    void setNative(float fader, float pan = .5f, uint32_t rampSamples = 0) SUPPLE_RT;
    // Mono source for stereo: pass the same input pointer twice. In-place supported.
    void process(const float* const* in, float* const* out, uint32_t frames) SUPPLE_RT
        requires(Stereo);
    void process(const double* const* in, double* const* out, uint32_t frames) SUPPLE_RT
        requires(Stereo);
    // Fader-only kernels for independent channels of a future wider strip.
    void process(const float* in, float* out, uint32_t frames) SUPPLE_RT
        requires(!Stereo);
    void process(const double* in, double* out, uint32_t frames) SUPPLE_RT
        requires(!Stereo);

  private:
    // Test-only coefficient injection isolates upstream block-interpolation rounding.
    friend struct StaticReferenceAccess;
    struct Ramp {
        double value = 0, target = 0, step = 0;
        uint32_t remaining = 0;
        void set(double v, uint32_t samples);
        double next();
    };
    template <class T, bool Linear>
    void render(const T* left, const T* right, T* outLeft, T* outRight, uint32_t frames);
    template <class T>
    void dispatch(const T* left, const T* right, T* outLeft, T* outRight, uint32_t frames);
    void coefficients();
    Ramp gain_, angle_;
    double trim_ = 0, left_ = 0, right_ = 0;
    Seeds seeds_{};
    bool linear_ = false;
};
}  // namespace detail

using Channel = detail::Stage<true, true>;
using Buss = detail::Stage<false, true>;
using MonoChannel = detail::Stage<true, false>;
using MonoBuss = detail::Stage<false, false>;
}  // namespace supple::console9
