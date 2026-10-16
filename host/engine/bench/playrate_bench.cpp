// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Device-free source-conversion benchmark: 30 stereo in-memory sources.
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <span>
#include <vector>

#include "playrate_dsp.h"

using namespace supple::engine::detail;
namespace {
struct Source {
    std::vector<float> data[2];
    static const float* fetch(const void* ctx, std::size_t channel, std::int64_t first,
                              std::span<float> scratch) noexcept {
        const auto& input = static_cast<const Source*>(ctx)->data[channel];
        if (first >= 0 && static_cast<std::size_t>(first) + scratch.size() <= input.size())
            return input.data() + first;
        for (std::size_t i = 0; i < scratch.size(); ++i) {
            const auto at = first + static_cast<std::int64_t>(i);
            scratch[i] = at >= 0 && static_cast<std::size_t>(at) < input.size()
                             ? input[static_cast<std::size_t>(at)]
                             : 0;
        }
        return scratch.data();
    }
};
}  // namespace
int main() {
    constexpr double sampleRate = 48000, rate = 1.137;
    constexpr std::size_t tracks = 30, quanta = 750;
    auto table = std::make_shared<TapeTable>();
    std::vector<Source> sources(tracks);
    std::vector<TapeReader> tape;
    std::vector<StretchReader> stretch;
    for (std::size_t track = 0; track < tracks; ++track) {
        for (auto& channel : sources[track].data) {
            channel.resize(240000);
            for (std::size_t i = 0; i < channel.size(); ++i)
                channel[i] =
                    static_cast<float>(.1 * std::sin(double(i) * (.02 + .001 * double(track))));
        }
        tape.emplace_back(table, 2);
        stretch.emplace_back(sampleRate, 2);
        const RateSource source{&sources[track], Source::fetch, 2};
        stretch.back().seek(source, 0, rate);
        stretch.back().prime(source, 1000);
    }
    std::array<float, 64> left{}, right{};
    float* outputs[] = {left.data(), right.data()};
    for (bool preserve : {false, true}) {
        double checksum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (std::size_t q = 0; q < quanta; ++q) {
            std::array<double, 65> positions{};
            for (std::size_t i = 0; i < positions.size(); ++i)
                positions[i] = double(q * 64 + i) * rate;
            for (std::size_t track = 0; track < tracks; ++track) {
                const RateSource source{&sources[track], Source::fetch, 2};
                const bool ok =
                    preserve ? stretch[track].process(
                                   source, stretch[track].inputPosition() + rate * 64, outputs)
                             : tape[track].process(source, positions, outputs);
                if (!ok) return 1;
                checksum += double(left[q % 64]);
            }
        }
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        const double duration = double(quanta * 64) / sampleRate;
        std::cout << (preserve ? "stretch" : "tape") << ": tracks=" << tracks << " rate=" << rate
                  << " audio_seconds=" << duration << " elapsed_seconds=" << elapsed
                  << " one_core_percent=" << elapsed / duration * 100 << " checksum=" << checksum
                  << '\n';
    }
}
