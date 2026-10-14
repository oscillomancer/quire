// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "fx.h"

#include <algorithm>
#include <cmath>

#include "dsp.h"

namespace supple::orrery {

using dsp::kPi;
using dsp::kTwoPi;

double DelayLine::read(double delay) const SUPPLE_RT {
    const double d = std::clamp(delay, 1.0, static_cast<double>(capacity()));
    const double rp = static_cast<double>(pos_) - d + static_cast<double>(buf_.size());
    const auto i0 = static_cast<std::size_t>(rp) % buf_.size();
    const std::size_t i1 = i0 + 1 == buf_.size() ? 0 : i0 + 1;
    const double frac = rp - std::floor(rp);
    const auto a = static_cast<double>(buf_[i0]), b = static_cast<double>(buf_[i1]);
    return a + (b - a) * frac;
}

void Biquad::lowShelf(double fs, double hz, double db) SUPPLE_RT {
    const double A = std::pow(10.0, db / 40.0), w = kTwoPi * hz / fs;
    const double c = std::cos(w), s = std::sin(w), alpha = s / 2.0 * std::sqrt(2.0);
    const double sq = 2.0 * std::sqrt(A) * alpha;
    const double a0 = (A + 1) + (A - 1) * c + sq;
    set(A * ((A + 1) - (A - 1) * c + sq) / a0, 2 * A * ((A - 1) - (A + 1) * c) / a0,
        A * ((A + 1) - (A - 1) * c - sq) / a0, -2 * ((A - 1) + (A + 1) * c) / a0,
        ((A + 1) + (A - 1) * c - sq) / a0);
}

void Biquad::highShelf(double fs, double hz, double db) SUPPLE_RT {
    const double A = std::pow(10.0, db / 40.0), w = kTwoPi * hz / fs;
    const double c = std::cos(w), s = std::sin(w), alpha = s / 2.0 * std::sqrt(2.0);
    const double sq = 2.0 * std::sqrt(A) * alpha;
    const double a0 = (A + 1) - (A - 1) * c + sq;
    set(A * ((A + 1) + (A - 1) * c + sq) / a0, -2 * A * ((A - 1) + (A + 1) * c) / a0,
        A * ((A + 1) + (A - 1) * c - sq) / a0, 2 * ((A - 1) - (A + 1) * c) / a0,
        ((A + 1) - (A - 1) * c - sq) / a0);
}

void Biquad::peak(double fs, double hz, double db, double q) SUPPLE_RT {
    const double A = std::pow(10.0, db / 40.0), w = kTwoPi * hz / fs;
    const double alpha = std::sin(w) / (2.0 * q), c = std::cos(w);
    const double a0 = 1 + alpha / A;
    set((1 + alpha * A) / a0, -2 * c / a0, (1 - alpha * A) / a0, -2 * c / a0, (1 - alpha / A) / a0);
}

void FxRack::prepare(double sampleRate) {
    fs_ = sampleRate;
    for (auto& l : chorus_.line) l.allocate(static_cast<std::size_t>(0.05 * fs_));
    for (auto& l : delay_.line) l.allocate(static_cast<std::size_t>(4.1 * fs_) + 8);
    // Mutually prime-ish base lengths (ms) scaled by size up to 2x.
    static constexpr double kBase[Reverb::kLines] = {29.7, 37.1, 41.1, 43.7,
                                                     53.3, 59.9, 67.7, 73.1};
    for (int i = 0; i < Reverb::kLines; ++i)
        reverb_.line[static_cast<std::size_t>(i)].allocate(
            static_cast<std::size_t>(kBase[i] * 2.0e-3 * fs_) + 8);
    for (auto& l : reverb_.pre) l.allocate(static_cast<std::size_t>(0.26 * fs_));
    reset();
}

void FxRack::reset() SUPPLE_RT {
    for (auto& l : chorus_.line) l.reset();
    chorus_.phase = 0.0;
    phaser_.ap = {};
    phaser_.fb = {};
    phaser_.phase = 0.0;
    for (auto& l : delay_.line) l.reset();
    delay_.lp = {};
    delay_.snap = true;
    for (auto& l : reverb_.line) l.reset();
    for (auto& l : reverb_.pre) l.reset();
    reverb_.lp = {};
    for (int ch = 0; ch < 2; ++ch) {
        eq_.low[static_cast<std::size_t>(ch)].reset();
        eq_.mid[static_cast<std::size_t>(ch)].reset();
        eq_.high[static_cast<std::size_t>(ch)].reset();
    }
}

void FxRack::set(FxKind kind, std::span<const double> v, double tempo) SUPPLE_RT {
    auto at = [&](auto f) { return v[static_cast<std::size_t>(f)]; };
    switch (kind) {
        case FxKind::Chorus:
            chorus_.rate = at(ChorusF::rate);
            chorus_.depth = at(ChorusF::depth) / 100.0;
            chorus_.voices = static_cast<int>(at(ChorusF::voices));
            chorus_.mix = at(ChorusF::mix) / 100.0;
            break;
        case FxKind::Phaser:
            phaser_.rate = at(PhaserF::rate);
            phaser_.depth = at(PhaserF::depth) / 100.0;
            phaser_.stages = std::clamp(static_cast<int>(at(PhaserF::stages)) / 2 * 2, 2, 12);
            phaser_.feedback = at(PhaserF::feedback) / 100.0;
            phaser_.mix = at(PhaserF::mix) / 100.0;
            break;
        case FxKind::Delay: {
            const auto sync = static_cast<NoteValue>(at(DelayF::sync));
            const double ms = sync == NoteValue::Off
                                  ? at(DelayF::time)
                                  : noteQuarters(sync) * 60000.0 / std::max(1.0, tempo);
            delay_.target =
                std::clamp(ms * 1e-3 * fs_, 1.0, static_cast<double>(delay_.line[0].capacity()));
            // The first time after reset the time starts at its value; glides
            // are for changes while running.
            if (delay_.snap) delay_.samples = delay_.target;
            delay_.snap = false;
            delay_.feedback = at(DelayF::feedback) / 100.0;
            delay_.pingPong = at(DelayF::ping_pong) > 0.5;
            delay_.damping = at(DelayF::damping) / 100.0;
            delay_.mix = at(DelayF::mix) / 100.0;
            break;
        }
        case FxKind::Reverb: {
            const double size = 0.5 + 1.5 * at(ReverbF::size) / 100.0;  // 0.5..2 x base lengths
            static constexpr double kBase[Reverb::kLines] = {29.7, 37.1, 41.1, 43.7,
                                                             53.3, 59.9, 67.7, 73.1};
            const double rt60 = at(ReverbF::decay) * 1e-3;
            for (int i = 0; i < Reverb::kLines; ++i) {
                const double len = kBase[i] * 1e-3 * size * fs_;
                reverb_.length[static_cast<std::size_t>(i)] = len;
                // -60 dB after rt60 seconds: g^(rt60·fs/len) = 1e-3.
                reverb_.gain[static_cast<std::size_t>(i)] =
                    std::pow(10.0, -3.0 * len / (rt60 * fs_));
            }
            reverb_.predelay = std::max(1.0, at(ReverbF::predelay) * 1e-3 * fs_);
            reverb_.damping = at(ReverbF::damping) / 100.0;
            reverb_.width = at(ReverbF::width) / 100.0;
            reverb_.mix = at(ReverbF::mix) / 100.0;
            break;
        }
        case FxKind::Eq:
            for (std::size_t ch = 0; ch < 2; ++ch) {
                eq_.low[ch].lowShelf(fs_, 120.0, at(EqF::low_gain));
                eq_.mid[ch].peak(fs_, at(EqF::mid_freq), at(EqF::mid_gain), at(EqF::mid_q));
                eq_.high[ch].highShelf(fs_, 8000.0, at(EqF::high_gain));
            }
            break;
    }
}

void FxRack::process(FxKind kind, float* l, float* r, std::uint32_t n) SUPPLE_RT {
    switch (kind) {
        case FxKind::Chorus: {
            auto& c = chorus_;
            const double inc = c.rate / fs_;
            for (std::uint32_t i = 0; i < n; ++i) {
                const double in[2] = {static_cast<double>(l[i]), static_cast<double>(r[i])};
                c.line[0].write(l[i]);
                c.line[1].write(r[i]);
                double wet[2] = {0.0, 0.0};
                for (int v = 0; v < c.voices; ++v) {
                    for (int ch = 0; ch < 2; ++ch) {
                        const double ph = c.phase + static_cast<double>(v) / c.voices + 0.25 * ch;
                        const double ms = 7.0 + 5.0 * c.depth * (1.0 + std::sin(kTwoPi * ph)) * 0.5;
                        wet[ch] += c.line[static_cast<std::size_t>(ch)].read(ms * 1e-3 * fs_);
                    }
                }
                c.phase = dsp::wrap01(c.phase + inc);
                const double norm = 1.0 / c.voices;
                l[i] = static_cast<float>(in[0] + (wet[0] * norm - in[0]) * c.mix);
                r[i] = static_cast<float>(in[1] + (wet[1] * norm - in[1]) * c.mix);
            }
            break;
        }
        case FxKind::Phaser: {
            auto& p = phaser_;
            const double inc = p.rate / fs_;
            for (std::uint32_t i = 0; i < n; ++i) {
                float* io[2] = {l + i, r + i};
                for (std::size_t ch = 0; ch < 2; ++ch) {
                    const double lfo =
                        0.5 + 0.5 * std::sin(kTwoPi * (p.phase + 0.25 * static_cast<double>(ch)));
                    const double hz = 200.0 * std::exp2(p.depth * 5.0 * lfo);
                    const double t = std::tan(kPi * std::min(hz, 0.45 * fs_) / fs_);
                    const double a = (t - 1.0) / (t + 1.0);
                    double x = static_cast<double>(*io[ch]) + p.fb[ch] * p.feedback;
                    for (int s = 0; s < p.stages; ++s) {
                        double& z = p.ap[ch][static_cast<std::size_t>(s)];
                        const double y = a * x + z;
                        z = x - a * y;
                        x = y;
                    }
                    p.fb[ch] = x;
                    // The notches come from summing dry and all-passed signals.
                    const auto dry = static_cast<double>(*io[ch]);
                    const double wet = 0.5 * (dry + x);
                    *io[ch] = static_cast<float>(dry + (wet - dry) * p.mix);
                }
                p.phase = dsp::wrap01(p.phase + inc);
            }
            break;
        }
        case FxKind::Delay: {
            auto& d = delay_;
            const double smooth = 1.0 - std::exp(-1.0 / (0.05 * fs_));  // 50 ms glide
            for (std::uint32_t i = 0; i < n; ++i) {
                d.samples += (d.target - d.samples) * smooth;
                double tap[2] = {d.line[0].read(d.samples), d.line[1].read(d.samples)};
                for (std::size_t ch = 0; ch < 2; ++ch)
                    d.lp[ch] += (tap[ch] - d.lp[ch]) * (1.0 - 0.9 * d.damping);
                const double fb = std::min(d.feedback, 1.1);
                const double in[2] = {static_cast<double>(l[i]), static_cast<double>(r[i])};
                double w[2];
                if (d.pingPong) {
                    w[0] = 0.5 * (in[0] + in[1]) + fb * d.lp[1];
                    w[1] = fb * d.lp[0];
                } else {
                    w[0] = in[0] + fb * d.lp[0];
                    w[1] = in[1] + fb * d.lp[1];
                }
                // Keep feedback above 100 % bounded.
                d.line[0].write(static_cast<float>(std::tanh(w[0])));
                d.line[1].write(static_cast<float>(std::tanh(w[1])));
                l[i] = static_cast<float>(in[0] + (tap[0] - in[0]) * d.mix);
                r[i] = static_cast<float>(in[1] + (tap[1] - in[1]) * d.mix);
            }
            break;
        }
        case FxKind::Reverb: {
            auto& v = reverb_;
            constexpr int N = Reverb::kLines;
            for (std::uint32_t i = 0; i < n; ++i) {
                v.pre[0].write(l[i]);
                v.pre[1].write(r[i]);
                const double inL = v.pre[0].read(v.predelay), inR = v.pre[1].read(v.predelay);
                std::array<double, N> out{};
                double sum = 0.0;
                for (int k = 0; k < N; ++k) {
                    const auto uk = static_cast<std::size_t>(k);
                    double y = v.line[uk].read(v.length[uk]);
                    v.lp[uk] += (y - v.lp[uk]) * (1.0 - 0.85 * v.damping);
                    out[uk] = v.lp[uk] * v.gain[uk];
                    sum += out[uk];
                }
                // Householder feedback: x_k = out_k - (2/N) Σ out, plus input.
                const double h = 2.0 / N * sum;
                for (int k = 0; k < N; ++k) {
                    const auto uk = static_cast<std::size_t>(k);
                    const double in = (k % 2 == 0 ? inL : inR) * 0.5;
                    v.line[uk].write(static_cast<float>(out[uk] - h + in));
                }
                double wl = 0.0, wr = 0.0;
                for (int k = 0; k < N; ++k)
                    (k % 2 == 0 ? wl : wr) += out[static_cast<std::size_t>(k)];
                wl *= 0.5;
                wr *= 0.5;
                const double mid = 0.5 * (wl + wr), side = 0.5 * (wl - wr) * v.width;
                wl = mid + side;
                wr = mid - side;
                const auto dl = static_cast<double>(l[i]), dr = static_cast<double>(r[i]);
                l[i] = static_cast<float>(dl + (wl - dl) * v.mix);
                r[i] = static_cast<float>(dr + (wr - dr) * v.mix);
            }
            break;
        }
        case FxKind::Eq:
            for (std::uint32_t i = 0; i < n; ++i) {
                l[i] = static_cast<float>(eq_.high[0].process(
                    eq_.mid[0].process(eq_.low[0].process(static_cast<double>(l[i])))));
                r[i] = static_cast<float>(eq_.high[1].process(
                    eq_.mid[1].process(eq_.low[1].process(static_cast<double>(r[i])))));
            }
            break;
    }
}

}  // namespace supple::orrery
