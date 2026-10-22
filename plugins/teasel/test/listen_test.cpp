// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>
#include <utility>
#include <vector>

#include "Check.h"
#include "core/Catalog.h"
#include "core/Engine.h"

using namespace supple;

namespace {
using Signal = std::array<std::vector<double>, 2>;
using Change = std::pair<int, int>;  // absolute frame, Listen choice

Signal render(double rate, int initial, const std::vector<Change>& changes = {},
              const std::vector<int>& blocks = {512}, bool reset = false) {
    Engine engine(catalog().list().front());
    const auto set = [&](std::string_view name, double value) {
        const auto& table = engine.definition()->params();
        for (size_t i = 0; i < table.count(); ++i)
            if (name == table.at(i).name) {
                engine.store().set(i, toHost(table.at(i), value));
                return;
            }
        CHECK_MSG(false, "missing parameter %.*s", static_cast<int>(name.size()), name.data());
    };
    // At 1x there is no resampling filter after the blend, so the expected
    // output is the crossfade of three independently rendered live signals.
    set("Quality", 0);
    set("Drive", 18);
    set("Mix", 35);
    set("Band Low", 200);
    set("Listen", initial);
    engine.activate(rate, *std::max_element(blocks.begin(), blocks.end()), 2, 2);
    if (reset) {
        set("Listen", (initial + 1) % 3);
        std::array<double, 17> left{}, right{};
        double* io[] = {left.data(), right.data()};
        engine.process(io, io, 17);
        set("Listen", initial);
        engine.reset();
    }
    const int frames = 4 * static_cast<int>(std::lround(rate * 0.01)) + 31;
    Signal output;
    for (size_t c = 0; c < output.size(); ++c) {
        output[c].resize(static_cast<size_t>(frames));
        for (int i = 0; i < frames; ++i)
            output[c][static_cast<size_t>(i)] =
                0.2 * std::sin(2.0 * dsp::kPi * 173.0 * i / rate + 0.37 * c);
    }
    size_t next = 0, block = 0;
    for (int frame = 0; frame < frames;) {
        if (next < changes.size() && changes[next].first == frame) {
            set("Listen", changes[next].second);
            ++next;
        }
        int n = std::min(blocks[block++ % blocks.size()], frames - frame);
        if (next < changes.size()) n = std::min(n, changes[next].first - frame);
        double* io[] = {output[0].data() + frame, output[1].data() + frame};
        engine.process(io, io, n);
        frame += n;
    }
    return output;
}

double difference(const Signal& a, const Signal& b) {
    double worst = 0.0;
    for (size_t c = 0; c < a.size(); ++c)
        for (size_t i = 0; i < a[c].size(); ++i)
            worst = std::max(worst, test::finiteDifference(a[c][i], b[c][i]));
    return worst;
}
}  // namespace

TEST(every_listen_change_crossfades_live_signals_over_exactly_ten_ms) {
    for (double rate : {8000.0, 44100.0, 48000.0, 96000.0}) {
        const int length = static_cast<int>(std::lround(rate * 0.01)), start = length + 13;
        const std::array<Signal, 3> reference = {render(rate, 0), render(rate, 1), render(rate, 2)};
        for (int from = 0; from < 3; ++from) {
            CHECK(difference(render(rate, from, {}, {512}, true), reference[from]) == 0.0);
            for (int to = 0; to < 3; ++to) {
                if (from == to) continue;
                Signal expected = reference[from];
                for (size_t c = 0; c < expected.size(); ++c)
                    for (size_t i = static_cast<size_t>(start); i < expected[c].size(); ++i) {
                        const double t =
                            std::min(1.0, (static_cast<double>(i) - start + 1) / length);
                        expected[c][i] += t * (reference[to][c][i] - expected[c][i]);
                    }
                const Signal actual = render(rate, from, {{start, to}}, {1, 7, 64, 513});
                CHECK_MSG(difference(actual, expected) < 1e-12, "%g Hz, Listen %d -> %d: %g", rate,
                          from, to, difference(actual, expected));
                CHECK(actual == render(rate, from, {{start, to}}));
            }
        }
    }
}

TEST(interrupted_listen_fade_starts_from_the_blend_currently_heard) {
    constexpr double rate = 48000.0;
    constexpr int length = 480, start = 493, interrupt = start + length / 2;
    const std::array<Signal, 3> reference = {render(rate, 0), render(rate, 1), render(rate, 2)};
    Signal expected = reference[1];
    for (size_t c = 0; c < expected.size(); ++c)
        for (size_t i = static_cast<size_t>(start); i < expected[c].size(); ++i) {
            const double band = reference[1][c][i], delta = reference[2][c][i];
            if (i < static_cast<size_t>(interrupt)) {
                const double t = (static_cast<double>(i) - start + 1) / length;
                expected[c][i] = band + t * (delta - band);
            } else {
                const double heard = 0.5 * band + 0.5 * delta;
                const double t = std::min(1.0, (static_cast<double>(i) - interrupt + 1) / length);
                expected[c][i] = heard + t * (reference[0][c][i] - heard);
            }
        }
    const std::vector<Change> changes = {{start, 2}, {interrupt, 0}};
    const Signal actual = render(rate, 1, changes, {1, 7, 64, 513});
    CHECK_NEAR(difference(actual, expected), 0.0, 1e-12);
    CHECK(actual == render(rate, 1, changes));
}

SUPPLE_TEST_MAIN
