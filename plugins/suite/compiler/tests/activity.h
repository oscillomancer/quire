// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/script.h>

// Parameters that apply only sometimes: on a toggle, a choice's labels or
// keys, either way about, and through a parameter that applies only
// sometimes itself.
struct Activity : Effect {
    static constexpr Info info = {.name = "Activity", .id = "com.example.activity"};

    Param on = toggle("On", true);
    Param shape = choice("Shape", {"Bell", "Low Shelf", "Low Cut"}, 0)
                      .keys({"bell", "shelf", "cut"})
                      .activeWhen(on);
    Param gain = decibels("Gain", -12, 12, 0).activeWhen(on).activeUnless(shape, {"cut"});
    Param width = octaves("Width", 0.1, 4, 1).activeWhen(shape, {"Bell"});
    Param freeze = toggle("Freeze", false);
    Param rate = frequency("Rate", 0.1, 10, 1).activeUnless(freeze);

    void process(Block& io) {}
};
