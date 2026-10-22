// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <array>

#include "Check.h"

#include "supple/script_runtime.h"

namespace supple::script::pollia_test {
#include "../script/pollia.cpp"
}  // namespace supple::script::pollia_test

using Pollia = supple::script::pollia_test::Pollia;

TEST(synced_motion_tracks_negative_and_positive_song_positions) {
    using Binding = supple::script::detail::ParamBinding;
    for (int mode = 0; mode < 3; ++mode) {
        for (double beats : {-1.25, -0.25, 0.0, 0.25, 4.125}) {
            Pollia p;
            p.prepare();
            Binding::setValue(p.sync, 6, true);  // quarter-note sync: one beat per cycle
            Binding::setValue(p.motion, mode, true);
            p.update();

            double sample = 0.0, spare = 0.0;
            double* samples[] = {&sample};
            Block block;
            block.channels = 1;
            block.frames = 1;
            block.data_ = samples;
            block.spare_ = &spare;
            block.transport.playing = true;
            block.transport.tempo = 120.0;
            block.transport.beats = beats;
            p.process(block);

            const int cycle = floorToInt(beats);
            const double phase = beats - cycle;
            const double turn = beats * (mode == 1 ? -1.0 : 1.0);
            CHECK_MSG(p.lfoCycle == cycle && p.lfoPhase == phase && p.turn == turn,
                      "mode %d at %.3f beats: cycle %d, phase %.3f, turn %.3f; "
                      "expected %d, %.3f, %.3f",
                      mode, beats, p.lfoCycle, p.lfoPhase, p.turn, cycle, phase, turn);
        }
    }
}

TEST(keep_below_tracks_slow_glides_on_both_channels_across_blocks) {
    using Binding = supple::script::detail::ParamBinding;
    for (int blockSize : {1, 7, 64, 512}) {
        Pollia p;
        p.prepare();
        Binding::setValue(p.keepBelow, 150.0, true);
        p.update();
        std::array<double, 512> left{}, right{}, glide{};
        double* samples[] = {left.data(), right.data()};
        double spare = 0.0;
        Block block;
        block.channels = 2;
        block.frames = 1;
        block.data_ = samples;
        block.spare_ = &spare;
        p.process(block);

        // Less than 0.01 Hz per sample, but four Hz over the whole glide.
        for (int direction : {1, -1}) {
            const double start = direction == 1 ? 150.0 : 154.0;
            for (int at = 0; at < 1000; at += block.frames) {
                block.frames = std::min(blockSize, 1000 - at);
                for (int i = 0; i < block.frames; ++i)
                    glide[i] = start + direction * (at + i + 1) * 0.004;
                Binding::setValue(p.keepBelow, glide[block.frames - 1], true);
                Binding::setGlide(p.keepBelow, glide.data(), block.frames);
                p.update();
                p.process(block);
            }
            const double cutoff = direction == 1 ? 154.0 : 150.0;
            Biquad low, high;
            low.lowpass(cutoff - 0.01, 0.5);
            high.lowpass(cutoff + 0.01, 0.5);
            for (int c = 0; c < 2; ++c) {
                const double response = p.ch[c].split.responseDb(390.0);
                CHECK_MSG(response >= low.responseDb(390.0) - 1e-10 &&
                              response <= high.responseDb(390.0) + 1e-10,
                          "block %d, direction %d, channel %d: response %.9f dB is stale",
                          blockSize, direction, c, response);
            }
        }
    }
}

TEST(keep_below_preserves_the_cumulative_cutoff_tolerance) {
    using Binding = supple::script::detail::ParamBinding;
    Pollia p;
    p.prepare();
    double sample = 0.0, spare = 0.0;
    double* samples[] = {&sample};
    Block block;
    block.channels = 1;
    block.frames = 1;
    block.data_ = samples;
    block.spare_ = &spare;
    Binding::setValue(p.keepBelow, 150.0, true);
    p.update();
    p.process(block);
    for (double cutoff : {150.004, 150.008, 150.012}) {
        Binding::setValue(p.keepBelow, cutoff, true);
        p.update();
        p.process(block);
        CHECK(p.lastKeep == (cutoff < 150.01 ? 150.0 : cutoff));
    }
}

SUPPLE_TEST_MAIN
