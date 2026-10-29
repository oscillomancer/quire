// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <memory>

#include "supple/script_native.h"

namespace supple::script::test_marram {
#include "../../marram/script/marram.cpp"
}

TEST_CASE("Marram free LFOs do not depend on the block partition") {
    namespace S = supple::script;
    namespace D = S::detail;
    using T = S::test_marram::Marram;
    const D::NativeCode<T> code;
    D::Instance instance;
    instance.maxBlockSize = 1024;
    D::CurrentScope scope(&instance);
    auto make = [&] {
        auto p = std::make_unique<T>();
        D::Instance::bind(*p, &instance);
        for (const auto& spec : code.params()) {
            auto* param = reinterpret_cast<S::Param*>(reinterpret_cast<unsigned char*>(p.get()) +
                                                      spec.offset);
            D::ParamBinding::setValue(
                *param, spec.unit == D::ParamModel::Unit::Percent ? spec.def * 0.01 : spec.def,
                true);
        }
        D::ParamBinding::setValue(p->l1Mode, 1, true);
        p->prepare();
        p->update();
        return p;
    };
    auto a = make(), b = make();
    std::array<double, 1024> left{}, right{};
    double* data[] = {left.data(), right.data()};
    double spare = 0.0;
    S::Block io;
    io.channels = 2;
    io.data_ = data;
    io.spare_ = &spare;
    const auto process = [&](T& p, int partition)
#if defined(__clang__) && __has_cpp_attribute(clang::nonblocking)
                             [[clang::nonblocking]]
#endif
    {
        for (int at = 0; at < 1024; at += io.frames) {
            io.frames = std::min(partition, 1024 - at);
            p.process(io);
        }
    };
    for (double hz : {0.12, 3.5, 0.3, 5.5}) {
        D::ParamBinding::setValue(a->l1Rate, hz, true);
        D::ParamBinding::setValue(b->l1Rate, hz, true);
        for (int step = 0; step < 4; ++step) {
            process(*a, 1);
            process(*b, 511);
            const auto value = [](T& p) {
                return p.lfo(p.voice[0], 0, 0, 0, p.l1Shape, p.l1Sync, p.l1Rate, p.l1Division,
                             p.l1Mode, p.l1Phase, p.l1Delay);
            };
            CHECK(value(*a) == value(*b));
        }
    }
}

TEST_CASE("Marram transport sync follows jumps and preserves tempo-change phase") {
    namespace S = supple::script;
    namespace D = S::detail;
    using T = S::test_marram::Marram;
    const D::NativeCode<T> code;
    D::Instance instance;
    instance.sampleRate = 48000.0;
    instance.maxBlockSize = 512;
    D::CurrentScope scope(&instance);
    auto p = std::make_unique<T>();
    D::Instance::bind(*p, &instance);
    for (const auto& spec : code.params()) {
        auto* param =
            reinterpret_cast<S::Param*>(reinterpret_cast<unsigned char*>(p.get()) + spec.offset);
        D::ParamBinding::setValue(
            *param, spec.unit == D::ParamModel::Unit::Percent ? spec.def * 0.01 : spec.def, true);
    }
    D::ParamBinding::setValue(p->l1Mode, 1, true);
    D::ParamBinding::setValue(p->l1Sync, 1, true);
    D::ParamBinding::setValue(p->l1Shape, 2, true);  // saw up: phase * 2 - 1
    p->prepare();
    p->update();
    std::array<double, 512> left{}, right{};
    double* data[] = {left.data(), right.data()};
    double spare = 0.0;
    S::Block io;
    io.channels = 2;
    io.data_ = data;
    io.spare_ = &spare;
    io.transport.playing = true;
    io.transport.tempo = 120;
    io.transport.beats = 0.125;
    const auto process = [&](int frames)
#if defined(__clang__) && __has_cpp_attribute(clang::nonblocking)
                             [[clang::nonblocking]]
#endif
    {
        io.frames = frames;
        p->process(io);
    };
    const auto value = [&] {
        return p->lfo(p->voice[0], 0, 0, 0, p->l1Shape, p->l1Sync, p->l1Rate, p->l1Division,
                      p->l1Mode, p->l1Phase, p->l1Delay);
    };
    process(0);
    CHECK(value() == -0.75);
    // Exact whole frames, with host beat rounding below the seek tolerance.
    for (int at = 0; at < 12000; at += 500) {
        io.transport.beats = 0.125 + at / 24000.0;
        process(500);
    }
    io.transport.beats = 0.625 + 5e-10;
    process(0);
    CHECK(value() == 0.25);
    io.transport.tempo = 180;
    process(0);
    CHECK(value() == 0.25);  // tempo changes keep phase, including ignored host jitter
    for (int at = 0; at < 12000; at += 500) {
        io.transport.beats = 0.625 + 5e-10 + at / 16000.0;
        process(500);
    }
    io.transport.beats = 1.375 + 5e-10;
    process(0);
    CHECK(value() == -0.25);
    io.transport.beats = 2.2;  // seek forward
    process(0);
    CHECK(value() == doctest::Approx(-0.6));
    io.transport.beats = 0.1;  // loop back
    process(0);
    CHECK(value() == doctest::Approx(-0.8));
    D::ParamBinding::setValue(p->l1Division, 3, true);  // eighth notes
    io.transport.beats += 5e-10;
    process(0);
    CHECK(value() == 0.1 / 0.5 * 2.0 - 1.0);
    io.transport.playing = false;
    process(0);
    io.transport.playing = true;
    io.transport.beats = 0.35;
    process(0);
    CHECK(value() == doctest::Approx(0.4));
    io.transport.beats += 2e-9;  // above the tolerance: even a small seek is honored
    process(0);
    CHECK(value() == io.transport.beats / 0.5 * 2.0 - 1.0);
}

TEST_CASE("Marram's harmonic ramp and digital table start at its analog saw") {
    namespace S = supple::script;
    namespace D = S::detail;
    using T = S::test_marram::Marram;
    const D::NativeCode<T> code;
    D::Instance instance;
    instance.maxBlockSize = 64;
    D::CurrentScope scope(&instance);
    auto p = std::make_unique<T>();
    D::Instance::bind(*p, &instance);
    p->prepare();
    auto& s = p->voice[0];
    for (int table : {0, 5}) {
        const double dt = 0.013;
        p->table(s, 0, table, 0.0, dt);
        for (int i = 0; i < 200; ++i) {
            const double tp = (i + 0.37) / 200.0;
            CHECK(p->wave(s, 0, tp, dt) == S::test_marram::analogWave(0, tp, dt, 0.5));
        }
        // Past 0 % it leaves the saw.
        p->table(s, 0, table, 0.06, dt);
        CHECK(p->wave(s, 0, 0.3, dt) != S::test_marram::analogWave(0, 0.3, dt, 0.5));
    }
}
