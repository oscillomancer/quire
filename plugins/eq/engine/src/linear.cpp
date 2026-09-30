#include "linear.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include "design.h"
namespace supple::eq {
void fft(std::span<Complex> x, bool inverse) {
    const auto n = x.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(x[i], x[j]);
    }
    for (std::size_t len = 2; len <= n; len *= 2) {
        const auto w =
            std::polar(1., (inverse ? 2. : -2.) * std::numbers::pi / static_cast<double>(len));
        for (std::size_t i = 0; i < n; i += len) {
            Complex z = 1.;
            for (std::size_t j = 0; j < len / 2; ++j) {
                const auto u = x[i + j], v = x[i + j + len / 2] * z;
                x[i + j] = u + v;
                x[i + j + len / 2] = u - v;
                z *= w;
            }
        }
    }
    if (inverse)
        for (auto& v : x) v /= static_cast<double>(n);
}
void Linear::prepare(double fs) {
    latency_ = static_cast<int>(std::lround(2048 * fs / 48000));
    fadeLength_ = static_cast<int>(std::lround(.02 * fs));
    const auto taps = static_cast<std::size_t>(2 * (latency_ - static_cast<int>(kBlock)) + 1);
    parts_ = (taps + kBlock - 1) / kBlock;
    history_.assign(2 * parts_ * kFft, {});
    current_.assign(4 * parts_ * kFft, {});
    target_ = current_;
    head_ = fill_ = 0;
    fade_ = 0;
    for (auto& a : input_) a.fill({});
    for (auto& a : output_) a.fill(0);
    for (auto& a : previous_) a.fill(0);
}
static std::unique_ptr<Kernel> makeKernel(const Values& v, double fs, int latency) {
    const int centre = latency - static_cast<int>(Linear::kBlock);
    const auto taps = static_cast<std::size_t>(2 * centre + 1);
    const auto parts_ = (taps + Linear::kBlock - 1) / Linear::kBlock;
    std::size_t n = 1;
    while (n < taps * 2) n *= 2;
    std::array<std::vector<Complex>, 4> response;
    for (auto& a : response) a.resize(n);
    Values minimum = v;
    minimum[kPhase] = 0;
    for (std::size_t bin = 0; bin <= n / 2; ++bin) {
        const double hz =
            std::min(fs * .499999, fs * static_cast<double>(bin) / static_cast<double>(n));
        std::array<double, 4> matrix = {1, 0, 0, 1};
        for (std::size_t b = 0; b < kBands; ++b)
            if (v[bandParam(b, Enabled)] >= .5) {
                const double m = std::pow(10., bandMagnitudeDb(minimum, b, hz, fs) / 20.);
                std::array<double, 4> a = {m, 0, 0, m};
                switch (static_cast<int>(v[bandParam(b, Channel)])) {
                    case Left:
                        a = {m, 0, 0, 1};
                        break;
                    case Right:
                        a = {1, 0, 0, m};
                        break;
                    case Mid:
                        a = {(m + 1) / 2, (m - 1) / 2, (m - 1) / 2, (m + 1) / 2};
                        break;
                    case Side:
                        a = {(m + 1) / 2, (1 - m) / 2, (1 - m) / 2, (m + 1) / 2};
                        break;
                    default:
                        break;
                }
                const auto old = matrix;
                matrix = {a[0] * old[0] + a[1] * old[2], a[0] * old[1] + a[1] * old[3],
                          a[2] * old[0] + a[3] * old[2], a[2] * old[1] + a[3] * old[3]};
            }
        for (std::size_t c = 0; c < 4; ++c) {
            response[c][bin] = matrix[c];
            if (bin > 0 && bin < n / 2) response[c][n - bin] = matrix[c];
        }
    }
    auto kernel = std::make_unique<Kernel>();
    kernel->spectra.resize(4 * parts_ * Linear::kFft);
    kernel->impulses.resize(4 * taps);
    for (std::size_t c = 0; c < 4; ++c) {
        fft(response[c], true);
        for (std::size_t p = 0; p < parts_; ++p) {
            std::array<Complex, Linear::kFft> block{};
            for (std::size_t i = 0; i < Linear::kBlock; ++i) {
                const auto tap = p * Linear::kBlock + i;
                if (tap < taps) {
                    const auto offset = static_cast<long long>(tap) - centre;
                    const auto index = static_cast<std::size_t>(
                        (offset + static_cast<long long>(n)) % static_cast<long long>(n));
                    // Symmetric Lanczos taper limits truncation ringing while preserving the
                    // centre.
                    const double x = static_cast<double>(offset) / static_cast<double>(centre + 1);
                    const double window =
                        x == 0 ? 1 : std::sin(std::numbers::pi * x) / (std::numbers::pi * x);
                    block[i] = response[c][index].real() * window;
                    kernel->impulses[c * taps + tap] = block[i].real();
                }
            }
            fft(block, false);
            std::copy(block.begin(), block.end(),
                      kernel->spectra.begin() +
                          static_cast<std::ptrdiff_t>((c * parts_ + p) * Linear::kFft));
        }
    }
    return kernel;
}
void Linear::publish(const Values& v, double fs) { handoff_.publish(makeKernel(v, fs, latency_)); }
double linearMagnitudeDb(const Values& values, double hz, double fs) {
    // The response display follows the affected-channel cascade; placement is
    // displayed by the band labels, as in the IIR response helper.
    Values v = values;
    for (std::size_t b = 0; b < kBands; ++b) v[bandParam(b, Channel)] = Stereo;
    struct Cache {
        Values values{};
        double fs = 0;
        std::unique_ptr<Kernel> kernel;
    };
    static thread_local Cache cache;
    if (!cache.kernel || cache.values != v || cache.fs != fs) {
        cache.kernel = makeKernel(v, fs, static_cast<int>(std::lround(2048 * fs / 48000)));
        cache.values = v;
        cache.fs = fs;
    }
    const auto taps = cache.kernel->impulses.size() / 4;
    const auto step = std::polar(1., -2 * std::numbers::pi * hz / fs);
    Complex z = 1., h = 0.;
    for (std::size_t i = 0; i < taps; ++i) {
        h += cache.kernel->impulses[i] * z;
        z *= step;
    }
    return 20 * std::log10(std::max(1e-300, std::abs(h)));
}
void Linear::reset() {
    std::fill(history_.begin(), history_.end(), Complex{});
    for (auto& a : input_) a.fill({});
    for (auto& a : output_) a.fill(0);
    for (auto& a : previous_) a.fill(0);
    head_ = fill_ = 0;
    fade_ = 0;
    if (auto* k = handoff_.acquire()) {
        std::copy(k->spectra.begin(), k->spectra.end(), current_.begin());
        std::copy(current_.begin(), current_.end(), target_.begin());
    }
}
void Linear::compute(std::size_t position, std::array<std::array<double, kFft>, 2>& out) {
    for (std::size_t c = 0; c < 2; ++c) {
        scratch_.fill({});
        for (std::size_t in = 0; in < 2; ++in)
            for (std::size_t p = 0; p < parts_; ++p) {
                const auto hp = (position + parts_ - p) % parts_;
                for (std::size_t k = 0; k < kFft; ++k)
                    scratch_[k] += history_[(in * parts_ + hp) * kFft + k] *
                                   current_[((c * 2 + in) * parts_ + p) * kFft + k];
            }
        fft(scratch_, true);
        for (std::size_t k = 0; k < kFft; ++k) out[c][k] = scratch_[k].real();
    }
}
void Linear::sample(double& l, double& r) {
    input_[0][fill_] = l;
    input_[1][fill_] = r;
    l = output_[0][fill_] + previous_[0][fill_ + kBlock];
    r = output_[1][fill_] + previous_[1][fill_ + kBlock];
    if (++fill_ < kBlock) return;
    fill_ = 0;
    previous_ = output_;
    const auto* old = handoff_.current();
    auto* next = handoff_.acquire();
    if (next != old && next) {
        std::copy(next->spectra.begin(), next->spectra.end(), target_.begin());
        fade_ = fadeLength_;
    }
    if (fade_ > 0) {
        const double a = std::min(1., static_cast<double>(kBlock) / fade_);
        for (std::size_t i = 0; i < current_.size(); ++i)
            current_[i] += a * (target_[i] - current_[i]);
        fade_ = std::max(0, fade_ - static_cast<int>(kBlock));
    }
    for (std::size_t c = 0; c < 2; ++c) {
        for (std::size_t i = kBlock; i < kFft; ++i) input_[c][i] = 0;
        fft(input_[c], false);
        std::copy(input_[c].begin(), input_[c].end(),
                  history_.begin() + static_cast<std::ptrdiff_t>((c * parts_ + head_) * kFft));
    }
    compute(head_, output_);
    head_ = (head_ + 1) % parts_;
}
}  // namespace supple::eq
