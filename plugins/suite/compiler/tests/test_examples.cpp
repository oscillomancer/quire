// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Differential tests: every example script, compiled three ways -- by the
// script compiler to machine code, to MIR's interpreter, and as ordinary C++
// by this test's own compiler (the way a source export builds it) -- must
// produce the same output, bit for bit, from the same input and parameters.
#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <sstream>

#include "runner.h"

// The examples, as a source export includes a script: inside the runtime's
// namespace, after <supple/script.h>.
namespace supple::script {
namespace ex_gain {
#include "gain.cpp"
}
namespace ex_tilt {
#include "tilt.cpp"
}
namespace ex_echo {
#include "echo.cpp"
}
namespace ex_drive {
#include "drive.cpp"
}
namespace ex_language {
#include "language.cpp"
}
}  // namespace supple::script

using namespace supple;
using scripttest::Audio;
using scripttest::JitRunner;
using scripttest::NativeRunner;

namespace {

std::string readExample(const std::string& name) {
    std::ifstream in(std::string(SUPPLE_SCRIPT_EXAMPLES) + "/" + name);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

// A test signal: a few partials, a decaying click every 1000 frames, and
// pseudo-random noise; the right channel a little different.
Audio signal(int frames) {
    Audio a(2, frames);
    std::uint32_t seed = 12345;
    for (int i = 0; i < frames; ++i) {
        seed = seed * 1664525u + 1013904223u;
        const double noise = (static_cast<double>(seed >> 8) / 16777216.0 - 0.5) * 0.1;
        const double t = i / 48000.0;
        const double click = std::exp(-(i % 1000) / 50.0);
        const double x = 0.4 * std::sin(6.283185307179586 * 220.0 * t) +
                         0.2 * std::sin(6.283185307179586 * 1870.0 * t) + 0.3 * click + noise;
        a.data[0][static_cast<size_t>(i)] = x;
        a.data[1][static_cast<size_t>(i)] = 0.8 * x - noise;
    }
    return a;
}

// One parameter change: at block `block`, parameter `index` takes `value`
// (the value the script reads).
struct Change {
    int block;
    size_t index;
    double value;
};

template <class T>
Audio runNative(const std::vector<double>& initial, const std::vector<Change>& changes, int frames,
                int block) {
    NativeRunner<T> run;
    for (size_t i = 0; i < initial.size(); ++i) run.set(i, initial[i]);
    run.prepare();
    run.update();
    Audio audio = signal(frames);
    Audio out(2, 0);
    for (int start = 0, b = 0; start < frames; start += block, ++b) {
        bool changed = false;
        for (const Change& c : changes)
            if (c.block == b) {
                run.set(c.index, c.value);
                changed = true;
            }
        if (changed) run.update();
        Audio piece(2, std::min(block, frames - start));
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < piece.frames(); ++i)
                piece.data[static_cast<size_t>(c)][static_cast<size_t>(i)] =
                    audio.data[static_cast<size_t>(c)][static_cast<size_t>(start + i)];
        run.process(piece, block);
        for (int c = 0; c < 2; ++c)
            out.data[static_cast<size_t>(c)].insert(out.data[static_cast<size_t>(c)].end(),
                                                    piece.data[static_cast<size_t>(c)].begin(),
                                                    piece.data[static_cast<size_t>(c)].end());
    }
    return out;
}

Audio runCompiled(const std::string& source, bool interpret, const std::vector<double>& initial,
                  const std::vector<Change>& changes, int frames, int block) {
    scriptc::Options options;
    options.interpret = interpret;
    auto result = scriptc::compile(source, options);
    INFO(scriptc::toText(result.diagnostics));
    REQUIRE(result.program);
    JitRunner run(result.program);
    for (size_t i = 0; i < initial.size(); ++i) run.setScript(i, initial[i]);
    run.prepare();
    run.update();
    Audio audio = signal(frames);
    Audio out(2, 0);
    for (int start = 0, b = 0; start < frames; start += block, ++b) {
        bool changed = false;
        for (const Change& c : changes)
            if (c.block == b) {
                run.setScript(c.index, c.value);
                changed = true;
            }
        if (changed) run.update();
        Audio piece(2, std::min(block, frames - start));
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < piece.frames(); ++i)
                piece.data[static_cast<size_t>(c)][static_cast<size_t>(i)] =
                    audio.data[static_cast<size_t>(c)][static_cast<size_t>(start + i)];
        run.process(piece, block);
        for (int c = 0; c < 2; ++c)
            out.data[static_cast<size_t>(c)].insert(out.data[static_cast<size_t>(c)].end(),
                                                    piece.data[static_cast<size_t>(c)].begin(),
                                                    piece.data[static_cast<size_t>(c)].end());
    }
    return out;
}

template <class T>
void differential(const std::string& file, const std::vector<double>& initial,
                  const std::vector<Change>& changes, int frames = 4800, int block = 128) {
    const std::string source = readExample(file);
    const Audio native = runNative<T>(initial, changes, frames, block);
    const Audio jit = runCompiled(source, false, initial, changes, frames, block);
    const Audio interpreted = runCompiled(source, true, initial, changes, frames, block);
    bool finite = true;
    double energy = 0.0;
    for (const auto& ch : native.data)
        for (double x : ch) {
            finite = finite && std::isfinite(x);
            energy += x * x;
        }
    CHECK(finite);
    CHECK(energy > 0.0);
    CHECK(jit == native);
    CHECK(interpreted == native);
}

}  // namespace

TEST_CASE("gain: JIT, interpreter and C++ agree") {
    differential<script::ex_gain::Gain>("gain.cpp", {-6.0}, {{10, 0, 3.5}, {20, 0, -60.0}});
}

TEST_CASE("tilt: JIT, interpreter and C++ agree") {
    differential<script::ex_tilt::Tilt>("tilt.cpp", {4.0, 800.0, -3.0},
                                        {{5, 0, -5.0}, {12, 1, 3000.0}, {20, 2, 6.0}});
}

TEST_CASE("echo: JIT, interpreter and C++ agree") {
    differential<script::ex_echo::Echo>("echo.cpp", {12.5, 0.6, 0.5}, {{15, 0, 3.25}, {25, 1, 0.2}},
                                        9600, 100);
}

TEST_CASE("drive: JIT, interpreter and C++ agree") {
    for (double shape : {0.0, 1.0, 2.0}) {
        CAPTURE(shape);
        differential<script::ex_drive::Drive>("drive.cpp", {18.0, shape, 6000.0, 0.8},
                                              {{10, 0, 30.0}, {20, 2, 1500.0}});
    }
}

TEST_CASE("the language tour: JIT, interpreter and C++ agree") {
    differential<script::ex_language::Tour>("language.cpp", {0.5, 4.0, 0.0, 1.0},
                                            {{8, 2, 1.0}, {14, 1, 9.0}, {22, 3, 0.0}, {26, 3, 1.0}},
                                            4800, 37);
}
