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

using Signal = std::vector<std::vector<double>>;
using Change = std::pair<int, int>;  // absolute frame, Listen choice

size_t parameter(const Definition& definition, std::string_view name) {
    for (size_t i = 0; i < definition.params().count(); ++i)
        if (name == definition.params().at(i).name) return i;
    CHECK_MSG(false, "parameter %.*s missing", static_cast<int>(name.size()), name.data());
    return 0;
}

Signal render(double rate, int channels, int initial, const std::vector<Change>& changes = {},
              const std::vector<int>& blocks = {512}) {
    auto definitions = product::makeCatalog()->list();
    CHECK(definitions.size() == 1);
    const auto& definition = definitions.front();
    Engine engine(definition);
    const auto set = [&](std::string_view name, double plain) {
        const size_t index = parameter(*definition, name);
        engine.store().set(index, toHost(definition->params().at(index), plain));
    };
    // Keep the gate closed so Off, Key and Delta are all positive, distinct
    // signals. Vary them independently to catch a frozen-sample crossfade.
    set("Threshold", 0.0);
    set("Range", -12.0);
    set("Key", 1.0);
    set("Lookahead", 3.0);
    set("Listen", initial);
    engine.activate(rate, *std::max_element(blocks.begin(), blocks.end()), channels, channels);
    const int frames = static_cast<int>(std::round(0.08 * rate));
    Signal output(static_cast<size_t>(channels), std::vector<double>(static_cast<size_t>(frames)));
    Signal key = output;
    for (int c = 0; c < channels; ++c)
        for (int i = 0; i < frames; ++i) {
            output[static_cast<size_t>(c)][static_cast<size_t>(i)] =
                0.7 - 0.1 * c + 0.05 * std::sin(2.0 * dsp::kPi * 71.0 * i / rate);
            key[static_cast<size_t>(c)][static_cast<size_t>(i)] =
                0.4 + 0.1 * c + 0.03 * std::cos(2.0 * dsp::kPi * 43.0 * i / rate);
        }
    std::array<double*, 2> audio{};
    std::array<const double*, 2> sidechain{};
    size_t next = 0, block = 0;
    for (int frame = 0; frame < frames; ++block) {
        // Listen is an editor control, not host-automatable. Deliver edits
        // through the store at their exact frame, splitting a block there.
        if (next < changes.size() && changes[next].first == frame) {
            set("Listen", changes[next].second);
            ++next;
        }
        int n = std::min(blocks[block % blocks.size()], frames - frame);
        if (next < changes.size()) n = std::min(n, changes[next].first - frame);
        for (int c = 0; c < channels; ++c) {
            audio[static_cast<size_t>(c)] = output[static_cast<size_t>(c)].data() + frame;
            sidechain[static_cast<size_t>(c)] = key[static_cast<size_t>(c)].data() + frame;
        }
        AudioBuses<double> io;
        io.in = audio.data();
        io.out = audio.data();
        io.sidechain = sidechain.data();
        NoEvents events;
        engine.process(io, n, events, nullptr);
        frame += n;
    }
    engine.deactivate();
    return output;
}

}  // namespace

TEST(every_listen_transition_crossfades_the_live_signals_over_ten_ms) {
    for (double rate : {8000.0, 44100.0, 48000.0, 96000.0})
        for (int channels : {1, 2}) {
            const std::array<Signal, 3> reference = {
                render(rate, channels, 0), render(rate, channels, 1), render(rate, channels, 2)};
            const int start = static_cast<int>(std::round(0.03 * rate));
            const int length = static_cast<int>(std::round(0.01 * rate));
            for (int from = 0; from < 3; ++from)
                for (int to = 0; to < 3; ++to) {
                    if (from == to) continue;
                    const auto output = render(rate, channels, from, {{start, to}});
                    double worst = 0.0;
                    for (size_t c = 0; c < output.size(); ++c)
                        for (size_t i = 0; i < output[c].size(); ++i) {
                            const double t =
                                std::clamp((static_cast<double>(i) - start + 1) / length, 0.0, 1.0);
                            const double a = reference[static_cast<size_t>(from)][c][i];
                            const double b = reference[static_cast<size_t>(to)][c][i];
                            worst = std::max(worst,
                                             test::finiteDifference(output[c][i], a + t * (b - a)));
                        }
                    CHECK_MSG(worst < 1e-12, "%g Hz, %d channels, %d -> %d: difference %g", rate,
                              channels, from, to, worst);
                }
        }
}

TEST(interrupted_listen_fades_start_from_the_mix_currently_heard_and_ignore_block_size) {
    constexpr double rate = 48000.0;
    constexpr int start = 1440, length = 480, interrupt = start + length / 2;
    const std::array<Signal, 3> reference = {render(rate, 2, 0), render(rate, 2, 1),
                                             render(rate, 2, 2)};
    for (int from = 0; from < 3; ++from)
        for (int to = 0; to < 3; ++to) {
            if (from == to) continue;
            const int third = 3 - from - to;
            const std::vector<Change> changes = {{start, to}, {interrupt, third}};
            const auto output = render(rate, 2, from, changes);
            const auto fragmented = render(rate, 2, from, changes, {1, 7, 64, 513});
            CHECK(output == fragmented);
            double worst = 0.0;
            for (size_t c = 0; c < output.size(); ++c)
                for (size_t i = static_cast<size_t>(interrupt); i < output[c].size(); ++i) {
                    const double a = reference[static_cast<size_t>(from)][c][i];
                    const double b = reference[static_cast<size_t>(to)][c][i];
                    const double d = reference[static_cast<size_t>(third)][c][i];
                    const double heard = 0.5 * a + 0.5 * b;
                    const double t =
                        std::clamp((static_cast<double>(i) - interrupt + 1) / length, 0.0, 1.0);
                    worst = std::max(worst,
                                     test::finiteDifference(output[c][i], heard + t * (d - heard)));
                }
            CHECK_MSG(worst < 1e-12, "%d -> %d -> %d: difference %g", from, to, third, worst);
        }
}

SUPPLE_TEST_MAIN
