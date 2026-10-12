// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Sources: a seeded random generator, noise, band-limited oscillators
// (polyBLEP and mip-mapped wavetables) and LFOs.
//
// Phase is a fraction of a cycle, [0, 1). Everything here is deterministic:
// the same seed, settings and calls give the same samples on every platform,
// which is what lets a test, a bounce and a reset() agree.
#pragma once

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <vector>

#include "dsp/Math.h"
#include "core/Fft.h"

namespace supple::dsp {

// PCG32 (Melissa O'Neill): small, fast, statistically sound, and the same
// sequence everywhere -- unlike std::uniform_real_distribution, whose output
// differs between standard libraries.
class Random {
  public:
    explicit Random(uint64_t seed = 0x853c49e6748fea9bull) { reseed(seed); }
    void reseed(uint64_t seed) {
        state_ = 0;
        next();
        state_ += seed;
        next();
    }

    uint32_t next() {
        const uint64_t old = state_;
        state_ = old * 6364136223846793005ull + kIncrement;
        const uint32_t shifted = static_cast<uint32_t>(((old >> 18u) ^ old) >> 27u);
        const uint32_t rotation = static_cast<uint32_t>(old >> 59u);
        return (shifted >> rotation) | (shifted << ((32u - rotation) & 31u));
    }
    // [0, 1)
    double uniform() { return next() * (1.0 / 4294967296.0); }
    // [-1, 1)
    double bipolar() { return 2.0 * uniform() - 1.0; }
    // 0 .. n - 1
    uint32_t below(uint32_t n) { return n == 0 ? 0 : static_cast<uint32_t>(uniform() * n); }
    // Mean 0, standard deviation 1 (Box-Muller; one draw per two uniforms).
    double gaussian() {
        const double u = 1.0 - uniform(), v = uniform();
        return std::sqrt(-2.0 * std::log(u)) * std::cos(2.0 * kPi * v);
    }

  private:
    static constexpr uint64_t kIncrement = 1442695040888963407ull;
    uint64_t state_ = 0;
};

// White, pink (-3 dB an octave: Paul Kellet's filter) and brown (-6 dB an
// octave) noise, each about as loud as the others, peaks near 1.
class Noise {
  public:
    explicit Noise(uint64_t seed = 1) : random_(seed) {}
    void reseed(uint64_t seed) {
        random_.reseed(seed);
        for (double& b : pink_) b = 0.0;
        brown_ = 0.0;
    }
    double white() { return random_.bipolar(); }
    double pink() {
        const double w = white();
        pink_[0] = 0.99886 * pink_[0] + w * 0.0555179;
        pink_[1] = 0.99332 * pink_[1] + w * 0.0750759;
        pink_[2] = 0.96900 * pink_[2] + w * 0.1538520;
        pink_[3] = 0.86650 * pink_[3] + w * 0.3104856;
        pink_[4] = 0.55000 * pink_[4] + w * 0.5329522;
        pink_[5] = -0.7616 * pink_[5] - w * 0.0168980;
        const double y = pink_[0] + pink_[1] + pink_[2] + pink_[3] + pink_[4] + pink_[5] + pink_[6] + w * 0.5362;
        pink_[6] = w * 0.115926;
        return y * 0.11;
    }
    double brown() {
        brown_ = 0.998 * brown_ + 0.02 * white();
        return std::clamp(brown_ * 3.5, -1.0, 1.0);
    }

  private:
    Random random_;
    double pink_[7] = {};
    double brown_ = 0.0;
};

// A phase accumulator.
struct Phasor {
    double phase = 0.0;      // [0, 1)
    double increment = 0.0;  // cycles per sample

    void setFrequency(double hz, double sampleRate) { increment = hz / sampleRate; }
    // The phase for this sample, then advances.
    double tick() {
        const double p = phase;
        phase += increment;
        phase -= std::floor(phase);
        return p;
    }
};

// The polyBLEP residual: what turns a naive step of 2 (+1 to -1, a saw's
// reset) at phase 0 into a band-limited one, over the sample either side of
// it (Valimaki and Huovilainen). t is the phase, dt the increment.
inline double polyBlep(double t, double dt) {
    if (t < dt) {
        t /= dt;
        return t + t - t * t - 1.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return t * t + t + t + 1.0;
    }
    return 0.0;
}
// The same for a corner, a step in slope (Esqueda, Valimaki and Bilbao): the
// integral of the above, for a change of slope of 2 per sample.
inline double polyBlamp(double t, double dt) {
    if (t < dt) {
        t = t / dt - 1.0;
        return -t * t * t / 3.0;
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt + 1.0;
        return t * t * t / 3.0;
    }
    return 0.0;
}

// The classic shapes, with their discontinuities smoothed by polyBLEP: far
// less aliasing than the naive shapes for the cost of a few operations. Up
// to about a quarter of the sample rate; beyond that, use a Wavetable.
class Oscillator {
  public:
    enum class Shape { Sine, Saw, Square, Triangle };

    void setShape(Shape s) { shape_ = s; }
    void setFrequency(double hz, double sampleRate) {
        phasor_.setFrequency(std::clamp(hz, 0.0, 0.5 * sampleRate), sampleRate);
    }
    // The square's high part, as a fraction of the cycle: 0.5 is square.
    void setPulseWidth(double width) { width_ = std::clamp(width, 0.01, 0.99); }
    void setPhase(double phase) { phasor_.phase = phase - std::floor(phase); }
    double phase() const { return phasor_.phase; }

    double tick() {
        const double dt = phasor_.increment, t = phasor_.tick();
        switch (shape_) {
            case Shape::Sine: return std::sin(2.0 * kPi * t);
            case Shape::Saw: return 2.0 * t - 1.0 - polyBlep(t, dt);
            case Shape::Square: {
                double y = t < width_ ? 1.0 : -1.0;
                y += polyBlep(t, dt);
                double down = t + 1.0 - width_;
                down -= std::floor(down);
                return y - polyBlep(down, dt);
            }
            case Shape::Triangle: {
                double y = 1.0 - 4.0 * std::fabs(t - 0.5);
                double half = t + 0.5;
                half -= std::floor(half);
                // The slope turns by 8 a cycle at each corner, 8 dt a
                // sample: up at 0, down at 1/2.
                y += 4.0 * dt * (polyBlamp(t, dt) - polyBlamp(half, dt));
                return y;
            }
        }
        return 0.0;
    }

  private:
    Phasor phasor_;
    Shape shape_ = Shape::Saw;
    double width_ = 0.5;
};

// One cycle of any shape, played at any pitch without aliasing: a table per
// octave, each holding only the harmonics that stay below Nyquist for the
// notes it plays. Built once (it allocates and runs FFTs: prepare() or a
// worker thread), then shared by every voice, each with its own Phasor.
class Wavetable {
  public:
    static constexpr int kSize = 2048;                // samples in each table's cycle
    static constexpr int kLevels = 11;                // 1024 harmonics down to 1

    // From one cycle of `length` samples (any length).
    void build(const double* cycle, int length, double sampleRate) {
        std::vector<std::complex<double>> harmonics(kSize / 2 + 1);
        length = std::max(1, length);
        const int count = std::min(kSize / 2, length / 2);
        for (int h = 1; h <= count; ++h) {
            std::complex<double> sum = 0.0;
            for (int n = 0; n < length; ++n) sum += cycle[n] * std::polar(1.0, -2.0 * kPi * h * n / length);
            harmonics[static_cast<size_t>(h)] = sum * (2.0 / length);
        }
        fromHarmonics(harmonics, sampleRate);
    }
    // From harmonic amplitudes, sines in phase: amplitudes[0] is the
    // fundamental.
    void buildAdditive(const double* amplitudes, int count, double sampleRate) {
        std::vector<std::complex<double>> harmonics(kSize / 2 + 1);
        for (int h = 1; h <= std::min(count, kSize / 2); ++h)
            harmonics[static_cast<size_t>(h)] = std::complex<double>(0.0, -amplitudes[h - 1]);
        fromHarmonics(harmonics, sampleRate);
    }
    bool empty() const { return tables_.empty(); }

    // The sample at `phase` for a note at `hz`.
    double sample(double phase, double hz) const {
        if (tables_.empty()) return 0.0;
        const double* table = tables_.data() + static_cast<size_t>(level(hz)) * (kSize + 1);
        const double x = phase * kSize;
        const int i = static_cast<int>(x);
        const double f = x - i;
        return table[i] + f * (table[i + 1] - table[i]);
    }
    double tick(Phasor& p, double hz) const { return sample(p.tick(), hz); }

  private:
    // The table with the most harmonics none of which pass Nyquist at hz.
    int level(double hz) const {
        const double highest = nyquist_ / std::max(hz, 1e-9);  // harmonics that fit
        int l = 0;
        while (l < kLevels - 1 && (kSize / 2 >> l) > highest) ++l;
        return l;
    }

    void fromHarmonics(const std::vector<std::complex<double>>& harmonics, double sampleRate) {
        nyquist_ = 0.5 * sampleRate;
        tables_.assign(static_cast<size_t>(kLevels) * (kSize + 1), 0.0);
        Fft fft(kSize);
        std::vector<std::complex<double>> x(kSize);
        for (int l = 0; l < kLevels; ++l) {
            const int top = kSize / 2 >> l;
            std::fill(x.begin(), x.end(), std::complex<double>{});
            for (int h = 1; h <= top && h < kSize / 2; ++h) {
                x[static_cast<size_t>(h)] = harmonics[static_cast<size_t>(h)] * 0.5;
                x[static_cast<size_t>(kSize - h)] = std::conj(harmonics[static_cast<size_t>(h)]) * 0.5;
            }
            // The inverse, unscaled: a cycle with these harmonics.
            for (auto& v : x) v = std::conj(v);
            fft.forward(x.data());
            double* table = tables_.data() + static_cast<size_t>(l) * (kSize + 1);
            for (int n = 0; n < kSize; ++n) table[n] = x[static_cast<size_t>(n)].real();
            table[kSize] = table[0];  // so interpolation never wraps
        }
    }

    std::vector<double> tables_;
    double nyquist_ = 24000.0;
};

// A low-frequency oscillator, bipolar ([-1, 1]), free-running or locked to
// the host's song position.
class Lfo {
  public:
    enum class Shape { Sine, Triangle, SawUp, SawDown, Square, SampleAndHold, SmoothRandom };

    explicit Lfo(uint64_t seed = 7) : random_(seed) { pick(); }
    void setShape(Shape s) { shape_ = s; }
    void setRate(double hz, double sampleRate) { phasor_.setFrequency(hz, sampleRate); }
    // Restarts the cycle (and the random shapes' sequence) at `phase`.
    void reset(double phase = 0.0, uint64_t seed = 7) {
        phasor_.phase = phase - std::floor(phase);
        random_.reseed(seed);
        pick();
    }
    // Locks the phase to the transport: one cycle every `beatsPerCycle`
    // quarter notes, from the song's start. Call it at the start of a block
    // while the host is playing, with Block::transport's beats.
    void syncTo(double songBeats, double beatsPerCycle, double offset = 0.0) {
        const double p = songBeats / std::max(beatsPerCycle, 1e-9) + offset;
        const double phase = p - std::floor(p);
        // A new cycle has begun: the random shapes take their next value.
        if (phase < phasor_.phase - 0.5) advanceRandom();
        phasor_.phase = phase;
    }
    double phase() const { return phasor_.phase; }

    double tick() {
        const double previous = phasor_.phase;
        const double t = phasor_.tick();
        const double held = current_;
        const double from = previous_;
        if (phasor_.phase < previous) advanceRandom();  // wrapped
        switch (shape_) {
            case Shape::Sine: return std::sin(2.0 * kPi * t);
            case Shape::Triangle: return triangle(t);
            case Shape::SawUp: return 2.0 * t - 1.0;
            case Shape::SawDown: return 1.0 - 2.0 * t;
            case Shape::Square: return t < 0.5 ? 1.0 : -1.0;
            case Shape::SampleAndHold: return held;
            case Shape::SmoothRandom: {
                // Cosine-eased from the last value to the next over a cycle.
                const double ease = 0.5 - 0.5 * std::cos(kPi * t);
                return from + (held - from) * ease;
            }
        }
        return 0.0;
    }

  private:
    // From 0 up to 1 at a quarter, down to -1 at three quarters: in phase
    // with the sine.
    static double triangle(double t) {
        const double u = t + 0.25;
        return 1.0 - 4.0 * std::fabs(u - std::floor(u) - 0.5);
    }
    void advanceRandom() {
        previous_ = current_;
        current_ = random_.bipolar();
    }
    void pick() {
        previous_ = 0.0;
        current_ = random_.bipolar();
    }

    Phasor phasor_;
    Shape shape_ = Shape::Sine;
    Random random_;
    double previous_ = 0.0, current_ = 0.0;
};

}  // namespace supple::dsp
