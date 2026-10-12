// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's own claims, measured through the Engine the plugin runs.
#include <algorithm>
#include <cmath>
#include <memory>

#include "Check.h"
#include "Params.h"
#include "Processor.h"
#include "Testing.h"

using namespace supple;
using namespace product;

namespace {

constexpr int kFrames = 48000;

// A key that is loud for the first half second and silent after.
test::Signal burst(int channels, double level = 0.5) {
    test::Signal s = test::silence(channels, kFrames);
    for (auto& c : s)
        for (int i = 0; i < kFrames / 2; ++i) c[static_cast<size_t>(i)] = i % 2 ? level : -level;
    return s;
}

}  // namespace

TEST(an_idle_key_leaves_the_input_bit_for_bit) {
    const test::Signal in = test::noise(2, kFrames, 5);
    std::vector<test::Signal> aux;
    test::RenderOptions o;
    o.blocks = {1, 64, 777};
    o.sidechain = test::silence(2, kFrames);
    o.aux = &aux;
    CHECK(test::maxDifference(test::render(in, o), in) == 0.0);
    CHECK(aux.size() == 1 && test::peak(aux[0]) == 0.0);
    // And with nothing connected to the sidechain at all.
    test::RenderOptions none;
    CHECK(test::maxDifference(test::render(in, none), in) == 0.0);
}

TEST(a_loud_key_ducks_by_exactly_the_depth_then_lets_go) {
    const test::Signal in = test::sine(2, kFrames, 220.0, 48000.0, 0.5);
    test::RenderOptions o;
    o.initial = {{kDepth, -12.0}, {kAttack, 1.0}, {kRelease, 10.0}};
    o.sidechain = burst(2);
    Engine engine;
    const test::Signal out = test::render(in, o, &engine);
    // Well after the attack, inside the burst: exactly the depth.
    double worst = 0.0;
    for (int i = 4800; i < kFrames / 2; ++i)
        worst = std::max(worst, std::fabs(out[0][static_cast<size_t>(i)] - in[0][static_cast<size_t>(i)] * dsp::dbToGain(-12.0)));
    CHECK_MSG(worst < 1e-12, "%g", worst);
    // Long after the burst: the input again, exactly.
    CHECK(test::maxDifference({{out[0].begin() + 40000, out[0].end()}}, {{in[0].begin() + 40000, in[0].end()}}) == 0.0);
    CHECK(engine.processor().gain() == 1.0);
}

TEST(main_plus_the_ducked_output_is_the_input) {
    const test::Signal in = test::noise(2, kFrames, 11);
    std::vector<test::Signal> aux;
    test::RenderOptions o;
    o.blocks = {13, 512, 4096};
    o.initial = {{kDepth, -60.0}};  // off: all the way down
    o.sidechain = burst(2);
    o.aux = &aux;
    o.automation = {{9000, kDepth, -6.0}, {30000, kThreshold, -3.0}};
    const test::Signal out = test::render(in, o);
    double worst = 0.0, ducked = 0.0;
    for (size_t c = 0; c < 2; ++c)
        for (size_t i = 0; i < static_cast<size_t>(kFrames); ++i) {
            worst = std::max(worst, std::fabs(out[c][i] + aux[0][c][i] - in[c][i]));
            ducked = std::max(ducked, std::fabs(aux[0][c][i]));
        }
    CHECK_MSG(worst < 1e-15, "%g", worst);
    CHECK(ducked > 0.1);  // something was removed
}

TEST(off_at_the_bottom_of_depth_is_silence) {
    const test::Signal in = test::sine(2, kFrames, 330.0, 48000.0, 0.5);
    test::RenderOptions o;
    o.initial = {{kDepth, -60.0}, {kAttack, 0.1}};
    o.sidechain = burst(2);
    const test::Signal out = test::render(in, o);
    CHECK(test::peak({{out[0].begin() + 2400, out[0].begin() + kFrames / 2}}) == 0.0);
}

TEST(every_surround_channel_is_ducked_alike) {
    // 7.1.4: the LFE and the heights take the same gain as the fronts.
    const test::Signal in = test::noise(12, kFrames / 2, 2);
    test::RenderOptions o;
    o.initial = {{kAttack, 0.1}};
    o.sidechain = burst(12);
    const test::Signal out = test::render(in, o);
    for (size_t c = 1; c < 12; ++c)
        for (size_t i = 0; i < out[0].size(); ++i)
            if (in[0][i] != 0.0 && in[c][i] != 0.0 &&
                std::fabs(out[c][i] / in[c][i] - out[0][i] / in[0][i]) > 1e-12) {
                CHECK_MSG(false, "channel %zu, frame %zu", c, i);
                return;
            }
}

TEST(reduction_reports_the_gain_for_hosts_and_the_editor) {
    const test::Signal in = test::noise(2, kFrames, 4);
    test::RenderOptions o;
    o.initial = {{kDepth, -18.0}, {kAttack, 0.1}, {kRelease, 10.0}};
    o.sidechain = burst(2);
    o.frames = kFrames;
    Engine engine;
    test::RenderOptions during = o;
    test::render({{in[0].begin(), in[0].begin() + 12000}, {in[1].begin(), in[1].begin() + 12000}}, during, &engine);
    const size_t r = static_cast<size_t>(params::indexOf(kReduction));
    CHECK_NEAR(toPlain(params::at(r), engine.store().get(r)), -18.0, 1e-6);
}

SUPPLE_TEST_MAIN
