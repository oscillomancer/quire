// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Link both DSP implementations: Windows rejects their old duplicate reset
// symbol; ELF could silently call core-dsp's reset on a template object.
#include <doctest/doctest.h>
#include <array>
#include "supple/dsp/oversampler.h"
#include "supple/script.h"

TEST_CASE("engine and script oversamplers coexist and reset their own state") {
    supple::dsp::Oversampler engine;
    engine.prepare(2);
    std::array<double, 2> hi{};
    engine.upsample(1, hi);
    (void)engine.downsample(hi);
    engine.reset();
    engine.upsample(0, hi);
    CHECK(engine.downsample(hi) == 0);

    supple::script::Oversampler script;
    script.prepare(2);
    script.up(1);
    (void)script.down();
    script.clear();
    CHECK(script.factor() == 2);
    script.up(0);
    CHECK(script.down() == 0);
    script.up(0.5);
    CHECK(script.down() != 0);
}
