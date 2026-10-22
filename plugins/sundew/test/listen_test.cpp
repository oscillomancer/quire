// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <vector>

#include "Check.h"
#include "core/Catalog.h"
#include "core/Engine.h"

using namespace supple;

namespace {

using Signal = std::vector<std::vector<double>>;
struct Change {
    int frame;
    int mode;
};

void setPlain(Engine& engine, std::string_view name, double value) {
    const auto& table = engine.definition()->params();
    for (size_t i = 0; i < table.count(); ++i) {
        if (name != table.at(i).name) continue;
        engine.store().set(i, toHost(table.at(i), value));
        return;
    }
    CHECK_MSG(false, "no parameter %.*s", static_cast<int>(name.size()), name.data());
}

Signal render(double rate, int channels, int ahead, int initialMode,
              const std::vector<Change>& changes, const std::vector<int>& blocks,
              bool neutral = false) {
    const int fade = static_cast<int>(std::lround(rate * 0.01));
    const int frames = 5 * fade + 83;
    Engine engine(catalog().list().front());
    setPlain(engine, "Listen", initialMode);
    setPlain(engine, "Lookahead", ahead);
    setPlain(engine, "Output", neutral ? 0.0 : -6.0);
    if (neutral) setPlain(engine, "Ratio", 1.0);
    engine.activate(rate, *std::max_element(blocks.begin(), blocks.end()), channels, channels);
    Signal output(static_cast<size_t>(channels), std::vector<double>(static_cast<size_t>(frames)));
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < frames; ++i)
            output[static_cast<size_t>(c)][static_cast<size_t>(i)] =
                0.25 + 0.05 * std::sin(0.031 * i + 0.37 * c);
    size_t change = 0, block = 0;
    for (int start = 0; start < frames;) {
        while (change < changes.size() && changes[change].frame == start) {
            setPlain(engine, "Listen", changes[change].mode);
            ++change;
        }
        int count = std::min(blocks[block++ % blocks.size()], frames - start);
        if (change < changes.size()) count = std::min(count, changes[change].frame - start);
        std::array<double*, 2> io{};
        for (int c = 0; c < channels; ++c)
            io[static_cast<size_t>(c)] = output[static_cast<size_t>(c)].data() + start;
        engine.process(io.data(), io.data(), count);
        start += count;
    }
    return output;
}

void expectSame(const Signal& actual, const Signal& expected, double tolerance) {
    double worst = 0.0;
    for (size_t c = 0; c < actual.size(); ++c)
        for (size_t i = 0; i < actual[c].size(); ++i)
            worst = std::max(worst, test::finiteDifference(actual[c][i], expected[c][i]));
    CHECK_MSG(worst <= tolerance, "peak difference %.17g, tolerance %g", worst, tolerance);
}

}  // namespace

TEST(all_listen_directions_crossfade_the_two_signals_over_ten_ms) {
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        const int fade = static_cast<int>(std::lround(rate * 0.01));
        const int at = 2 * fade + 23;
        for (int channels : {1, 2}) {
            for (int ahead : {0, 1, 2, 3}) {
                std::array<Signal, 3> reference;
                for (int mode = 0; mode < 3; ++mode)
                    reference[static_cast<size_t>(mode)] =
                        render(rate, channels, ahead, mode, {}, {512});
                for (int from = 0; from < 3; ++from) {
                    for (int to = 0; to < 3; ++to) {
                        if (from == to) continue;
                        Signal expected = reference[static_cast<size_t>(from)];
                        for (int c = 0; c < channels; ++c)
                            for (size_t i = static_cast<size_t>(at); i < expected[c].size(); ++i) {
                                const double progress =
                                    std::min(1.0, (static_cast<double>(i) - at + 1) / fade);
                                expected[c][i] +=
                                    progress *
                                    (reference[static_cast<size_t>(to)][c][i] - expected[c][i]);
                            }
                        const Signal actual =
                            render(rate, channels, ahead, from, {{at, to}}, {1, 7, 64, 513});
                        expectSame(actual, expected, 1e-12);
                        expectSame(actual, render(rate, channels, ahead, from, {{at, to}}, {512}),
                                   0.0);
                    }
                }
            }
        }
    }
}

TEST(interrupted_listen_fades_start_from_the_current_audible_mixture) {
    for (double rate : {44100.0, 48000.0, 96000.0}) {
        const int fade = static_cast<int>(std::lround(rate * 0.01));
        const int first = 2 * fade + 23, second = first + fade / 3, third = second + fade / 4;
        std::array<Signal, 3> reference;
        for (int mode = 0; mode < 3; ++mode)
            reference[static_cast<size_t>(mode)] = render(rate, 2, 3, mode, {}, {512});
        // Off -> Key, interrupted by Delta, interrupted by Off. Keep the
        // live source waveforms running: the fade must not hold one sample.
        const double a = static_cast<double>(second - first) / fade;
        const double b = static_cast<double>(third - second) / fade;
        Signal expected = reference[0];
        for (size_t c = 0; c < expected.size(); ++c) {
            for (size_t i = static_cast<size_t>(first); i < expected[c].size(); ++i) {
                const double normal = reference[0][c][i], key = reference[1][c][i];
                const double delta = reference[2][c][i];
                if (i < static_cast<size_t>(second)) {
                    const double t = (static_cast<double>(i) - first + 1) / fade;
                    expected[c][i] = normal + t * (key - normal);
                } else if (i < static_cast<size_t>(third)) {
                    const double before = normal + a * (key - normal);
                    const double t = (static_cast<double>(i) - second + 1) / fade;
                    expected[c][i] = before + t * (delta - before);
                } else {
                    const double before = (normal + a * (key - normal)) * (1.0 - b) + delta * b;
                    const double t = std::min(1.0, (static_cast<double>(i) - third + 1) / fade);
                    expected[c][i] = before + t * (normal - before);
                }
            }
        }
        const std::vector<Change> changes = {{first, 1}, {second, 2}, {third, 0}};
        const Signal actual = render(rate, 2, 3, 0, changes, {1, 7, 64, 513});
        expectSame(actual, expected, 1e-12);
        expectSame(actual, render(rate, 2, 3, 0, changes, {512}), 0.0);
    }
}

TEST(identical_normal_and_key_signals_never_dip_when_listen_is_turned_off) {
    const Signal normal = render(48000.0, 2, 0, 0, {}, {512}, true);
    const Signal switching =
        render(48000.0, 2, 0, 1, {{1000, 0}, {1200, 1}, {1400, 0}}, {1, 7, 64, 513}, true);
    expectSame(switching, normal, 1e-15);
}

SUPPLE_TEST_MAIN
