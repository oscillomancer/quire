// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <memory>

#include "Check.h"
#include "supple/script_runtime.h"

namespace supple::script::restart_test {
#include "../script/lichen.cpp"
}  // namespace supple::script::restart_test

namespace {
namespace S = supple::script;
namespace D = supple::script::detail;

struct Fixture {
    D::Instance instance;
    S::restart_test::Lichen script;
    double left[256] = {}, right[256] = {}, spare = 0.0;
    int position = 0;

    Fixture() {
        D::CurrentScope current(&instance);
        D::Instance::bind(script, &instance);
        script.prepare();
    }

    void feed(int frames, bool playing, bool sound, int blockSize = 256) {
        D::CurrentScope current(&instance);
        double* data[] = {left, right};
        S::Block block;
        block.channels = 2;
        block.data_ = data;
        block.spare_ = &spare;
        block.transport.playing = playing;
        while (frames > 0) {
            block.frames = std::min(frames, blockSize);
            for (int i = 0; i < block.frames; ++i)
                left[i] = right[i] =
                    sound ? 0.25 * S::sin(S::twoPi * 1000.0 * (position + i) / 48000.0) : 0.0;
            script.process(block);
            frames -= block.frames;
            position += block.frames;
        }
    }

    void reset() {
        D::CurrentScope current(&instance);
        D::ParamBinding::setValue(script.reset, 1.0, true);
        script.update();
        D::ParamBinding::setValue(script.reset, 1.0, false);
    }

    void checkSilentPlay() const {
        CHECK(D::ParamBinding::reported(script.integrated) == -70.0);
        CHECK(script.gatedBlocks == 0);
        CHECK(script.monoBlocks == 0);
        CHECK(script.rangeBlocks == 0);
        CHECK(script.framesAll == 0);
        CHECK(script.centroidFrames == 0);
        CHECK(D::ParamBinding::reported(script.momentaryMax) == -70.0);
        CHECK(D::ParamBinding::reported(script.shortTermMax) == -70.0);
        CHECK(instance.status.outOfRange.load() == 0);
    }
};
}  // namespace

TEST(new_silent_play_excludes_the_previous_loud_play) {
    for (int blockSize : {1, 256}) {
        auto f = std::make_unique<Fixture>();
        f->feed(48000, true, true, blockSize);
        CHECK(D::ParamBinding::reported(f->script.integrated) > -20.0);
        f->feed(256, false, false, blockSize);
        f->feed(4800, true, false, blockSize);
        // "Now" still includes the preceding tone, while since-play windows
        // cannot use it. The restart also falls inside a 100 ms energy block.
        CHECK(f->script.momentaryNow > -20.0);
        f->checkSilentPlay();
        f->feed(14400, true, false, blockSize);
        f->checkSilentPlay();
    }
}

TEST(reset_keeps_rolling_readings_but_excludes_old_windows) {
    auto f = std::make_unique<Fixture>();
    auto rolling = std::make_unique<Fixture>();
    f->feed(153600, true, true);
    rolling->feed(153600, true, true);
    const double momentary = f->script.momentaryNow, shortTerm = f->script.shortNow;
    const auto spectrum = f->script.nowM;
    f->reset();
    CHECK(f->script.momentaryNow == momentary);
    CHECK(f->script.shortNow == shortTerm);
    for (int i = 0; i < spectrum.size(); ++i) CHECK(f->script.nowM[i] == spectrum[i]);
    f->feed(32, true, false);
    rolling->feed(32, true, false);
    CHECK(f->script.peakTrue == 0.0);
    f->feed(4800 - 32, true, false);
    rolling->feed(4800 - 32, true, false);
    CHECK(f->script.momentaryNow > -20.0);
    CHECK(f->script.momentaryNow == rolling->script.momentaryNow);
    CHECK(f->script.shortNow == rolling->script.shortNow);
    for (int i = 0; i < spectrum.size(); ++i) {
        CHECK(f->script.nowM[i] == rolling->script.nowM[i]);
        CHECK(f->script.nowS[i] == rolling->script.nowS[i]);
    }
    f->checkSilentPlay();
    f->feed(14400, true, false);
    f->checkSilentPlay();
}

TEST(only_complete_post_restart_loudness_windows_enter_the_histograms) {
    auto f = std::make_unique<Fixture>();
    f->feed(153600, true, true);
    f->feed(256, false, false);
    f->feed(19200, true, true);
    // The fourth completed energy block still straddles the restart. The
    // fifth one is the first whose entire 400 ms window belongs to this play.
    CHECK(f->script.gatedBlocks == 0);
    CHECK(f->script.monoBlocks == 0);
    f->feed(4800, true, true);
    CHECK(f->script.gatedBlocks == 1);
    CHECK(f->script.monoBlocks == 1);
    f->feed(120000, true, true);
    CHECK(f->script.rangeBlocks == 0);
    CHECK(D::ParamBinding::reported(f->script.shortTermMax) == -70.0);
    f->feed(4800, true, true);
    CHECK(f->script.rangeBlocks == 1);
    CHECK(D::ParamBinding::reported(f->script.shortTermMax) > -20.0);
}

SUPPLE_TEST_MAIN
