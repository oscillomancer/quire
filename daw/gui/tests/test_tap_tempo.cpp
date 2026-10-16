// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include "../app/tap_tempo.h"

using supple::gui::TapTempo;
using namespace std::chrono_literals;

TEST_CASE("tap tempo averages intervals and resets after a pause") {
    TapTempo taps;
    const TapTempo::TimePoint start{};
    CHECK_FALSE(taps.tap(start));
    for (int i = 1; i <= 4; ++i) {
        const auto bpm = taps.tap(start + i * 500ms);
        REQUIRE(bpm);
        CHECK(*bpm == doctest::Approx(120));
    }
    CHECK_FALSE(taps.tap(start + 4001ms));
    CHECK(taps.tap(start + 5001ms) == 60);
    // Exactly two seconds still belongs to the current sequence.
    CHECK(taps.tap(start + 7001ms) == 40);
}

TEST_CASE("tap tempo keeps only the latest eight taps and averages intervals") {
    TapTempo taps;
    TapTempo::TimePoint now{};
    CHECK_FALSE(taps.tap(now));
    REQUIRE(taps.tap(now += 1s) == 60);
    CHECK(taps.tap(now += 500ms) == 80);  // 60 / mean(1, .5), not mean(60, 120)
    for (int i = 0; i < 4; ++i) REQUIRE(taps.tap(now += 500ms));
    // Eight taps still include the initial one-second interval.
    CHECK(taps.tap(now += 500ms) == 105);
    // The ninth tap drops that interval; the remaining seven are all 500 ms.
    CHECK(taps.tap(now += 500ms) == 120);
}

TEST_CASE("tap tempo clamps fast taps and safely restarts nonincreasing timestamps") {
    TapTempo taps;
    const TapTempo::TimePoint start{};
    CHECK_FALSE(taps.tap(start));
    CHECK(taps.tap(start + 1ms) == 1000);
    CHECK_FALSE(taps.tap(start + 1ms));
    CHECK_FALSE(taps.tap(start));
    CHECK(taps.tap(start + 500ms) == 120);
}
