// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Differential tests: every example script, compiled three ways -- by the
// script compiler to machine code, to MIR's interpreter, and as ordinary C++
// by this test's own compiler (the way a source export builds it) -- must
// produce the same output, bit for bit, from the same input and parameters.
#include <doctest/doctest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>

#include "runner.h"
#include "supple/script_native.h"

// The examples, as a source export includes a script: inside the runtime's
// namespace, after <supple/script.h>.
namespace supple::script {
namespace ex_array_bounds {
#include "array_bounds.h"
}
namespace ex_lvalues {
#include "lvalues.h"
}
namespace ex_activity {
#include "activity.h"
}
namespace ex_routes {
#include "routes.h"
}
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
namespace ex_compressor {
#include "compressor.cpp"
}
namespace ex_spectral {
#include "spectral.cpp"
}
namespace ex_bell {
#include "bell.cpp"
}
namespace ex_meter {
#include "meter.cpp"
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

// A sidechain key: a decaying 60 Hz thump every 2400 frames, like a kick.
Audio key(int frames) {
    Audio a(2, frames);
    for (int i = 0; i < frames; ++i) {
        const double t = (i % 2400) / 48000.0;
        const double x = 0.9 * std::exp(-t * 30.0) * std::sin(6.283185307179586 * 60.0 * t);
        a.data[0][static_cast<size_t>(i)] = x;
        a.data[1][static_cast<size_t>(i)] = x;
    }
    return a;
}

// The frames [start, start + n) of `audio`.
Audio slice(const Audio& audio, int start, int n) {
    Audio piece(audio.channels(), n);
    for (int c = 0; c < audio.channels(); ++c)
        for (int i = 0; i < n; ++i)
            piece.data[static_cast<size_t>(c)][static_cast<size_t>(i)] =
                audio.data[static_cast<size_t>(c)][static_cast<size_t>(start + i)];
    return piece;
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
                int block, bool sidechain) {
    NativeRunner<T> run;
    for (size_t i = 0; i < initial.size(); ++i) run.set(i, initial[i]);
    run.prepare();
    run.update();
    Audio audio = signal(frames);
    const Audio keys = key(frames);
    Audio out(2, 0);
    for (int start = 0, b = 0; start < frames; start += block, ++b) {
        bool changed = false;
        for (const Change& c : changes)
            if (c.block == b) {
                run.set(c.index, c.value);
                changed = true;
            }
        if (changed) run.update();
        Audio piece = slice(audio, start, std::min(block, frames - start));
        const Audio keyPiece = slice(keys, start, piece.frames());
        run.key = sidechain ? &keyPiece : nullptr;
        run.process(piece, block);
        for (int c = 0; c < 2; ++c)
            out.data[static_cast<size_t>(c)].insert(out.data[static_cast<size_t>(c)].end(),
                                                    piece.data[static_cast<size_t>(c)].begin(),
                                                    piece.data[static_cast<size_t>(c)].end());
    }
    return out;
}

Audio runCompiled(const std::string& source, bool interpret, const std::vector<double>& initial,
                  const std::vector<Change>& changes, int frames, int block, bool sidechain) {
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
    const Audio keys = key(frames);
    Audio out(2, 0);
    for (int start = 0, b = 0; start < frames; start += block, ++b) {
        bool changed = false;
        for (const Change& c : changes)
            if (c.block == b) {
                run.setScript(c.index, c.value);
                changed = true;
            }
        if (changed) run.update();
        Audio piece = slice(audio, start, std::min(block, frames - start));
        const Audio keyPiece = slice(keys, start, piece.frames());
        run.key = sidechain ? &keyPiece : nullptr;
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
                  const std::vector<Change>& changes, int frames = 4800, int block = 128,
                  bool sidechain = false) {
    const std::string source = readExample(file);
    const Audio native = runNative<T>(initial, changes, frames, block, sidechain);
    const Audio jit = runCompiled(source, false, initial, changes, frames, block, sidechain);
    const Audio interpreted = runCompiled(source, true, initial, changes, frames, block, sidechain);
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

// What an editor hook does, as text: every Canvas call and Editor control in
// order, numbers to the last bit, so two paths can be compared.
class Recorder final : public script::detail::Painter,
                       public script::detail::EditorBuilder,
                       public script::detail::EditorServices {
  public:
    std::vector<std::string> calls;

    // --- EditorServices ---
    char* text(int bytes) override {
        texts_.emplace_back(static_cast<size_t>(bytes), '\0');
        return texts_.back().data();
    }
    const char* paramText(int slot, double value) override {
        texts_.push_back(std::to_string(slot) + ":" + number(value));
        return texts_.back().c_str();
    }
    const char* paramName(int slot) override {
        texts_.push_back("param" + std::to_string(slot));
        return texts_.back().c_str();
    }
    double paramNormalized(int slot, double value) override { return slot + value / 1024.0; }
    void editParam(int slot, Edit kind, double value) override {
        add("edit", {double(slot), double(int(kind)), value});
    }
    void playNote(int key, double velocity, bool on) override {
        add("playNote", {double(key), velocity, double(on)});
    }

    // --- Painter ---
    double width() const override { return 400; }
    double height() const override { return 300; }
    void save() override { add("save"); }
    void restore() override { add("restore"); }
    void translate(double x, double y) override { add("translate", {x, y}); }
    void rotate(double a) override { add("rotate", {a}); }
    void scale(double x, double y) override { add("scale", {x, y}); }
    void fillStyle(int c) override { add("fillStyle", {double(c)}); }
    void strokeStyle(int c) override { add("strokeStyle", {double(c)}); }
    void globalAlpha(double a) override { add("globalAlpha", {a}); }
    void lineWidth(double w) override { add("lineWidth", {w}); }
    void lineCap(script::LineCap c) override { add("lineCap", {double(int(c))}); }
    void lineJoin(script::LineJoin j) override { add("lineJoin", {double(int(j))}); }
    void linearGradient(double a, double b, double c, double d, int e, int f) override {
        add("linearGradient", {a, b, c, d, double(e), double(f)});
    }
    void radialGradient(double a, double b, double c, double d, int e, int f) override {
        add("radialGradient", {a, b, c, d, double(e), double(f)});
    }
    void fillRect(double x, double y, double w, double h) override {
        add("fillRect", {x, y, w, h});
    }
    void strokeRect(double x, double y, double w, double h) override {
        add("strokeRect", {x, y, w, h});
    }
    void clearRect(double x, double y, double w, double h) override {
        add("clearRect", {x, y, w, h});
    }
    void beginPath() override { add("beginPath"); }
    void closePath() override { add("closePath"); }
    void moveTo(double x, double y) override { add("moveTo", {x, y}); }
    void lineTo(double x, double y) override { add("lineTo", {x, y}); }
    void bezierCurveTo(double a, double b, double c, double d, double x, double y) override {
        add("bezierCurveTo", {a, b, c, d, x, y});
    }
    void quadraticCurveTo(double a, double b, double x, double y) override {
        add("quadraticCurveTo", {a, b, x, y});
    }
    void arc(double x, double y, double r, double a, double b) override {
        add("arc", {x, y, r, a, b});
    }
    void arcTo(double a, double b, double c, double d, double r) override {
        add("arcTo", {a, b, c, d, r});
    }
    void rect(double x, double y, double w, double h) override { add("rect", {x, y, w, h}); }
    void roundRect(double x, double y, double w, double h, double r) override {
        add("roundRect", {x, y, w, h, r});
    }
    void ellipse(double x, double y, double a, double b) override { add("ellipse", {x, y, a, b}); }
    void circle(double x, double y, double r) override { add("circle", {x, y, r}); }
    void fill() override { add("fill"); }
    void stroke() override { add("stroke"); }
    void clip(double x, double y, double w, double h) override { add("clip", {x, y, w, h}); }
    void font(Face f, double size) override { add("font", {double(int(f)), size}); }
    void textAlign(script::TextAlign a) override { add("textAlign", {double(int(a))}); }
    void textBaseline(script::TextBaseline b) override { add("textBaseline", {double(int(b))}); }
    void fillText(const char* t, double x, double y) override {
        add(std::string("fillText ") + t, {x, y});
    }
    double measureText(const char* t) override { return 7.0 * static_cast<double>(std::strlen(t)); }
    void panel(double x, double y, double w, double h, const char* t) override {
        add(std::string("panel ") + t, {x, y, w, h});
    }
    void well(double x, double y, double w, double h) override { add("well", {x, y, w, h}); }
    void meter(double db, double x, double y, double w, double h) override {
        add("meter", {db, x, y, w, h});
    }
    void frequencyGrid(double x, double y, double w, double h, double a, double b) override {
        add("frequencyGrid", {x, y, w, h, a, b});
    }
    void levelGrid(double x, double y, double w, double h, double a, double b, double c) override {
        add("levelGrid", {x, y, w, h, a, b, c});
    }

    // --- EditorBuilder ---
    void size(double w, double h) override { add("size", {w, h}); }
    void control(Control kind, int slot, double x, double y, double w, double h) override {
        add("control", {double(int(kind)), double(slot), x, y, w, h});
    }
    void xyPad(int a, int b, double x, double y, double w, double h) override {
        add("xyPad", {double(a), double(b), x, y, w, h});
    }
    void envelope(int a, int d, int s, int r, double x, double y, double w, double h) override {
        add("envelope", {double(a), double(d), double(s), double(r), x, y, w, h});
    }
    void file(const void*, double x, double y, double w, double h) override {
        add("file", {x, y, w, h});
    }
    void keyboard(int lo, int hi, double x, double y, double w, double h) override {
        add("keyboard", {double(lo), double(hi), x, y, w, h});
    }
    void meters(double x, double y, double w, double h) override { add("meters", {x, y, w, h}); }
    void routes(double x, double y, double w, double h) override { add("routes", {x, y, w, h}); }

  private:
    static std::string number(double v) {
        char b[40];
        std::snprintf(b, sizeof(b), "%.17g", v);
        return b;
    }
    void add(const std::string& name, std::initializer_list<double> args = {}) {
        std::string line = name;
        for (double a : args) line += " " + number(a);
        calls.push_back(std::move(line));
    }
    std::deque<std::string> texts_;
};

// The editor's hooks in each path after the same audio, and a hover, a
// drag, a turn of the wheel and a double click: the same calls.
template <class T>
void sameDrawing(const std::string& file, int frames = 4800) {
    const std::string source = readExample(file);
    auto drawn = [&](auto& run) {
        Recorder r;
        run.instance.editor = &r;
        run.prepare();
        run.update();
        Audio a = signal(frames);
        run.process(a, 128);
        run.editor(&r);
        run.draw(&r);
        using A = script::PointerAction;
        auto act = [&](A action, double x, double y, double wheel = 0.0, bool twice = false) {
            script::Pointer p;
            script::detail::Layout::set(p, action, x, y, 0.0, 0.0, wheel, twice, false, false,
                                        false);
            run.pointer(p);
            run.draw(&r);
        };
        act(A::Move, 240, 132);
        act(A::Press, 240, 132);
        act(A::Drag, 300, 80);
        act(A::Release, 300, 80);
        act(A::Wheel, 200, 100, 1.0);
        act(A::Press, 200, 100, 0.0, true);
        act(A::Release, 200, 100);
        act(A::Leave, 200, 100);
        return r.calls;
    };
    NativeRunner<T> native;
    const std::vector<std::string> expected = drawn(native);
    CHECK(expected.size() > 4);
    for (bool interpret : {false, true}) {
        scriptc::Options options;
        options.interpret = interpret;
        auto compiled = scriptc::compile(source, options);
        INFO(scriptc::toText(compiled.diagnostics));
        REQUIRE(compiled.program);
        JitRunner jit(compiled.program);
        CHECK(drawn(jit) == expected);
    }
}

// The script compiled as C++ describes the plugin the compiler describes:
// identity, kind and every parameter. Keys and ids too when the export's
// keys are given (keys: true), and otherwise those withKey() sets.
template <class T>
void sameDescription(const char* file,
                     std::vector<typename script::detail::NativeCode<T>::Key> keys = {}) {
    auto result = scriptc::compile(readExample(file));
    INFO(scriptc::toText(result.diagnostics));
    REQUIRE(result.program);
    const scriptc::Program& jit = *result.program;
    const bool withKeys = !keys.empty();
    const script::detail::NativeCode<T> native(std::move(keys));
    CHECK(native.factoryPatches() == jit.factoryPatches());
    CHECK(native.kind() == jit.kind());
    const auto &a = jit.info(), &b = native.info();
    CHECK(a.name == b.name);
    CHECK(a.id == b.id);
    CHECK(a.vendor == b.vendor);
    CHECK(a.version == b.version);
    CHECK(a.description == b.description);
    CHECK(a.url == b.url);
    CHECK(a.channels == b.channels);
    CHECK(a.sidechain == b.sidechain);
    CHECK(a.outputs == b.outputs);
    CHECK(a.features == b.features);
    CHECK(a.voices == b.voices);
    CHECK(a.latency == b.latency);
    CHECK(a.tail == b.tail);
    REQUIRE(jit.params().size() == native.params().size());
    for (size_t i = 0; i < jit.params().size(); ++i) {
        const auto &p = jit.params()[i], &q = native.params()[i];
        CAPTURE(p.key);
        if (withKeys) {
            CHECK(p.key == q.key);
            CHECK(p.id == q.id);
        }
        CHECK(p.kind == q.kind);
        CHECK(p.name == q.name);
        CHECK(p.min == q.min);
        CHECK(p.max == q.max);
        CHECK(p.def == q.def);
        CHECK(p.unit == q.unit);
        CHECK(p.curve == q.curve);
        CHECK(p.curveK == q.curveK);
        CHECK(p.choices == q.choices);
        CHECK(p.group == q.group);
        CHECK(p.description == q.description);
        CHECK(p.smoothingMs == q.smoothingMs);
        CHECK(p.automatable == q.automatable);
        CHECK(p.modulatable == q.modulatable);
        CHECK(p.polyModulatable == q.polyModulatable);
        CHECK(p.restarts == q.restarts);
        CHECK(p.offAtMinimum == q.offAtMinimum);
        CHECK(p.output == q.output);
        CHECK(p.bypass == q.bypass);
        CHECK(p.hidden == q.hidden);
        CHECK(p.roles == q.roles);
        CHECK(p.activeWhen == q.activeWhen);
    }
    REQUIRE(jit.shared().size() == native.shared().size());
    for (size_t i = 0; i < jit.shared().size(); ++i) {
        CHECK(jit.shared()[i].offset == native.shared()[i].offset);
        CHECK(jit.shared()[i].bytes == native.shared()[i].bytes);
    }
    CHECK((jit.editor() != nullptr) == (native.editor() != nullptr));
    CHECK((jit.draw() != nullptr) == (native.draw() != nullptr));
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

TEST_CASE("compressor: JIT, interpreter and C++ agree, with and without a key") {
    for (bool sidechain : {false, true}) {
        CAPTURE(sidechain);
        differential<script::ex_compressor::Compressor>(
            "compressor.cpp", {-30.0, 6.0, 2.0, 80.0, 3.0, 0.0}, {{10, 0, -40.0}, {20, 3, 400.0}},
            9600, 128, sidechain);
    }
    // The key, not the input, decides the turning down once connected.
    using script::ex_compressor::Compressor;
    const std::vector<double> settings = {-30.0, 6.0, 2.0, 80.0, 0.0, 0.0};
    CHECK_FALSE(runNative<Compressor>(settings, {}, 4800, 128, false) ==
                runNative<Compressor>(settings, {}, 4800, 128, true));
}

TEST_CASE("spectral: JIT, interpreter and C++ agree") {
    for (double mode : {0.0, 1.0, 2.0}) {
        CAPTURE(mode);
        differential<script::ex_spectral::Spectral>("spectral.cpp", {mode, -40.0, 1.0},
                                                    {{30, 1, -20.0}, {50, 2, 0.6}}, 9600, 128);
    }
}

TEST_CASE("meter: JIT, interpreter and C++ agree, and draw alike") {
    differential<script::ex_meter::Meter>("meter.cpp", {-3.0}, {{20, 0, 6.0}});
    sameDrawing<script::ex_meter::Meter>("meter.cpp");
}

TEST_CASE("bell: JIT, interpreter and C++ agree, and draw and edit alike") {
    differential<script::ex_bell::Bell>("bell.cpp", {2500.0, 9.0, 0.5}, {{20, 1, -12.0}});
    sameDrawing<script::ex_bell::Bell>("bell.cpp");
}

TEST_CASE("Stft: a sine reads its amplitude, and a spectrum left alone is a delay") {
    script::detail::Instance instance;
    script::detail::CurrentScope scope(&instance);
    script::Stft s;
    s.prepare(1024, 4);
    const int k = 64;  // 3000 Hz at 48 kHz
    CHECK(s.binHz(k) == 3000.0);
    std::vector<double> in(8192), out(8192);
    double read = 0.0;
    for (size_t n = 0; n < in.size(); ++n) {
        in[n] = 0.5 * std::sin(6.283185307179586 * k * static_cast<double>(n) / 1024.0);
        if (s.push(in[n])) read = s.magnitude(k);
        out[n] = s.pull();
    }
    CHECK(read == doctest::Approx(0.5).epsilon(1e-9));
    double worst = 0.0;
    for (size_t n = 2048; n < in.size(); ++n)
        worst = std::max(worst, std::abs(out[n] - in[n - static_cast<size_t>(s.latency())]));
    CHECK(worst < 1e-12);
}

TEST_CASE("Stft: one prepared for analysis reads the same bins with push() alone") {
    script::detail::Instance instance;
    script::detail::CurrentScope scope(&instance);
    script::Stft full, look;
    full.prepare(1024, 4);
    look.prepareAnalysis(1024, 4);
    CHECK(look.bins() == full.bins());
    CHECK(look.hop() == full.hop());
    bool same = true;
    int frames = 0;
    for (int n = 0; n < 8192; ++n) {
        const double x = 0.5 * std::sin(0.37 * n) + 0.25 * std::sin(0.05 * n);
        const bool ready = full.push(x);
        CHECK(look.push(x) == ready);
        if (ready) {
            ++frames;
            for (int k = 0; k < full.bins(); ++k)
                same = same && look.real(k) == full.real(k) && look.imag(k) == full.imag(k);
        }
        full.pull();
        // Still readable after a pull(), which gives nothing back.
        if (ready) {
            CHECK(look.pull() == 0.0);
            same = same && look.magnitude(5) != 0.0;
        }
    }
    CHECK(frames == 8192 / 256);
    CHECK(same);
}

TEST_CASE("whole-number conversions match the saturating libm results at boundaries") {
    const auto saturate = [](double x) {
        if (!(x == x)) return 0;
        if (x >= 2147483647.0) return std::numeric_limits<int>::max();
        if (x <= -2147483648.0) return std::numeric_limits<int>::min();
        return static_cast<int>(x);
    };
    const double tiny = std::numeric_limits<double>::denorm_min();
    const double largestSubnormal = std::nextafter(std::numeric_limits<double>::min(), 0.0);
    const double inf = std::numeric_limits<double>::infinity();
    for (double x : {1.0, 1.5, 2147483646.5, 2147483647.0, 2147483647.5, 0.5, 0.0, -0.0, -0.5, -1.5,
                     tiny, -tiny, largestSubnormal, -largestSubnormal,
                     std::numeric_limits<double>::quiet_NaN(), inf, -inf, -2147483648.5}) {
        CAPTURE(x);
        CHECK(script::toInt(x) == saturate(std::trunc(x)));
        CHECK(script::floorToInt(x) == saturate(std::floor(x)));
    }
}

TEST_CASE("Oversampler: 1x passes through, and linear phase is a pure delay") {
    script::detail::Instance instance;
    script::detail::CurrentScope scope(&instance);
    // A restart from 4x to 1x: the same object, prepared again.
    script::Oversampler o;
    o.prepare(4);
    CHECK(o.factor() == 4);
    CHECK(o.latency() > 0);
    o.prepare(1);
    CHECK(o.factor() == 1);
    CHECK(o.latency() == 0);
    o.up(0.25);
    CHECK(o.sample(0) == 0.25);
    CHECK(o.down() == 0.25);
    // Linear phase: everything below the passband's edge comes out as it went
    // in, latency() samples later.
    script::Oversampler linear;
    linear.prepare(4, script::Oversampler::Phase::Linear);
    const int lag = linear.latency();
    CHECK(lag > 0);
    std::vector<double> in(4800), out(4800);
    for (size_t n = 0; n < in.size(); ++n) {
        const double t = static_cast<double>(n) / 48000.0;
        in[n] = 0.4 * std::sin(6.283185307179586 * 100.0 * t) +
                0.3 * std::sin(6.283185307179586 * 15000.0 * t);
        linear.up(in[n]);
        out[n] = linear.down();
    }
    double worst = 0.0;
    for (size_t n = 1000; n < in.size(); ++n)
        worst = std::max(worst, std::abs(out[n] - in[n - static_cast<size_t>(lag)]));
    CHECK(worst < 1e-4);
}

TEST_CASE("a script compiled as C++ describes the plugin the compiler describes") {
    using script::ex_tilt::Tilt;
    sameDescription<Tilt>(
        "tilt.cpp", {{&Tilt::tilt, "tilt"}, {&Tilt::pivot, "pivot"}, {&Tilt::output, "output"}});
    sameDescription<script::ex_gain::Gain>("gain.cpp");
    sameDescription<script::ex_echo::Echo>("echo.cpp");
    sameDescription<script::ex_drive::Drive>("drive.cpp");
    sameDescription<script::ex_language::Tour>("language.cpp");
    sameDescription<script::ex_compressor::Compressor>("compressor.cpp");
    sameDescription<script::ex_spectral::Spectral>("spectral.cpp");
    sameDescription<script::ex_bell::Bell>("bell.cpp");
    sameDescription<script::ex_meter::Meter>("meter.cpp");
    // Without its keys, a Param's key is its name's: "Pivot" -> pivot.
    const script::detail::NativeCode<Tilt> keyless;
    CHECK(keyless.params()[1].key == "pivot");
}

TEST_CASE("lvalues preserve C++ sequencing and reference storage") {
    std::ifstream in(std::filesystem::path(__FILE__).parent_path() / "lvalues.h");
    REQUIRE(in.good());
    std::stringstream source;
    source << in.rdbuf();
    Audio expected(2, 12);
    NativeRunner<script::ex_lvalues::Lvalues> native;
    native.process(expected);
    CHECK(expected.data[0] == std::vector<double>{7, 1, 2, 1, 4, 3, 9, 9, 7, 0.5, 1, 3});
    for (bool interpret : {false, true}) {
        INFO(interpret);
        scriptc::Options options;
        options.interpret = interpret;
        auto result = scriptc::compile(source.str(), options);
        REQUIRE_MESSAGE(result.program, scriptc::toText(result.diagnostics));
        JitRunner run(result.program);
        Audio actual(2, 12);
        run.process(actual);
        CHECK(actual == expected);
    }
}

TEST_CASE("out-of-range aggregate spares have the same lifetime in every backend") {
    using T = script::ex_array_bounds::ArrayBounds;
    static_assert(std::is_trivially_copyable_v<T::Cell>);
    static_assert(!std::is_trivially_copyable_v<T::Resource>);
    std::ifstream in(std::filesystem::path(__FILE__).parent_path() / "array_bounds.h");
    REQUIRE(in.good());
    std::stringstream source;
    source << in.rdbuf();
    NativeRunner<T> native;
    Audio expected(1, 9);
    double* data[] = {expected.data[0].data()};
    double spare = 0.0;
    script::Block block;
    block.channels = 1;
    block.frames = 9;
    block.data_ = data;
    block.spare_ = &spare;
    const auto process = [&]()
#if defined(__clang__) && __has_cpp_attribute(clang::nonblocking)
                             [[clang::nonblocking]]
#endif
    { native.plugin().process(block); };
    script::detail::CurrentScope scope(&native.instance);
    process();
    CHECK(expected.data[0] == std::vector<double>{3.5, 1, 4, 3.5, 7, 9, 11, 3.5, 3.5});
    CHECK(native.instance.status.outOfRange.load() == 12);
    for (bool interpret : {false, true}) {
        INFO(interpret);
        scriptc::Options options;
        options.interpret = interpret;
        auto result = scriptc::compile(source.str(), options);
        REQUIRE_MESSAGE(result.program, scriptc::toText(result.diagnostics));
        JitRunner run(result.program);
        Audio actual(1, 9);
        run.process(actual);
        CHECK(actual.data[0] == std::vector<double>{3.5, 1, 4, 3.5, 7, 9, 11, 3.5, 3.5});
        CHECK(actual == expected);
        CHECK(run.instance.status.outOfRange.load() == 12);
    }
}

TEST_CASE("out-of-range enum array writes are discarded in every backend") {
    enum class Mode { Off, On };
    script::Array<Mode, 1> modes;
    const auto writePastEnd = [&]()
#if defined(__clang__) && __has_cpp_attribute(clang::nonblocking)
                                  [[clang::nonblocking]]
#endif
    {
        modes[0] = Mode::On;
        modes[1] = Mode::On;
        return modes[-1];
    };
    CHECK(writePastEnd() == Mode::Off);
    CHECK(modes[-1] == Mode::Off);
    CHECK(modes[1] == Mode::Off);
    CHECK(modes[0] == Mode::On);

    const std::string source = R"(
#include <supple/script.h>
struct Test : Effect {
    static constexpr Info info = {.name = "Test", .id = "com.example.test"};
    enum class Mode { Off, On };
    Array<Mode, 1> modes;
    void process(Block& io) {
        modes[0] = Mode::On;
        modes[1] = Mode::On;
        io[0][0] = static_cast<int>(modes[-1]);
        io[0][1] = static_cast<int>(modes[1]);
        io[0][2] = static_cast<int>(modes[0]);
    }
};
)";
    for (bool interpret : {false, true}) {
        INFO(interpret);
        scriptc::Options options;
        options.interpret = interpret;
        auto result = scriptc::compile(source, options);
        REQUIRE_MESSAGE(result.program, scriptc::toText(result.diagnostics));
        JitRunner run(result.program);
        Audio actual(1, 3);
        run.process(actual);
        CHECK(actual.data[0] == std::vector<double>{0, 0, 1});
        CHECK(run.instance.status.outOfRange.load() == 3);
    }
}

TEST_CASE("a local Array of text starts from its literals in every backend") {
    const std::string source = R"(
#include <supple/script.h>
struct Test : Effect {
    static constexpr Info info = {.name = "Test", .id = "com.example.test"};
    void process(Block& io) {}
    void draw(Canvas& g) {
        Array<const char*, 3> names = {"A", "B", "C"};
        for (int k = 0; k < 3; ++k) g.fillText(names[k], k, 0);
    }
};
)";
    for (bool interpret : {false, true}) {
        INFO(interpret);
        scriptc::Options options;
        options.interpret = interpret;
        auto result = scriptc::compile(source, options);
        REQUIRE_MESSAGE(result.program, scriptc::toText(result.diagnostics));
        JitRunner run(result.program);
        Recorder r;
        run.instance.editor = &r;
        run.draw(&r);
        CHECK(r.calls ==
              std::vector<std::string>{"fillText A 0 0", "fillText B 1 0", "fillText C 2 0"});
    }
}

TEST_CASE("a script compiled as C++ has the sources and routes the compiler sees") {
    std::ifstream in(std::filesystem::path(__FILE__).parent_path() / "routes.h");
    REQUIRE(in.good());
    std::stringstream source;
    source << in.rdbuf();
    auto result = scriptc::compile(source.str());
    REQUIRE_MESSAGE(result.program, scriptc::toText(result.diagnostics));
    using Routes = script::ex_routes::Routes;
    const script::detail::NativeCode<Routes> native(
        {}, {{&Routes::lfo, "lfo"}, {&Routes::env, "env"}, {&Routes::macro, "macro"}});
    const auto& jit = *result.program;
    REQUIRE(jit.sources().size() == 3);
    REQUIRE(native.sources().size() == 3);
    for (size_t i = 0; i < 3; ++i) {
        const auto &a = jit.sources()[i], &b = native.sources()[i];
        CAPTURE(i);
        CHECK(a.key == b.key);
        CHECK(a.name == b.name);
        CHECK(a.description == b.description);
        CHECK(a.perVoice == b.perVoice);
        CHECK(a.bipolar == b.bipolar);
        CHECK(a.offset == b.offset);
    }
    CHECK(jit.sources()[0].member == "lfo");
    CHECK(jit.sources()[1].key == "env2");  // withKey() wins over the member's name
    CHECK(jit.sources()[2].perVoice == false);
    CHECK(jit.sources()[2].bipolar == false);
    CHECK(jit.params()[1].routeMin == -1200);
    CHECK(native.params()[1].routeMax == 1200);
    CHECK(native.factoryPatches() == jit.factoryPatches());
    REQUIRE(jit.factoryPatches().size() == 1);
    using Route = script::detail::FactoryPatchModel::Route;
    CHECK(jit.factoryPatches()[0].routes ==
          std::vector<Route>{{"lfo>cutoff", "", 1.5}, {"env2>fine", "modwheel", 700}});
    using File = std::pair<std::uint32_t, std::string>;
    CHECK(jit.factoryPatches()[0].files ==
          std::vector<File>{{script::detail::keyId("sample"), "factory:keys.flac"}});
    REQUIRE(jit.files().size() == 1);
    REQUIRE(native.files().size() == 1);
    CHECK(jit.files()[0].def == "factory:pad.flac");
    CHECK(native.files()[0].def == jit.files()[0].def);
}

TEST_CASE("a script compiled as C++ says when its parameters apply as the compiler does") {
    std::ifstream in(std::filesystem::path(__FILE__).parent_path() / "activity.h");
    REQUIRE(in.good());
    std::stringstream source;
    source << in.rdbuf();
    auto result = scriptc::compile(source.str());
    REQUIRE_MESSAGE(result.program, scriptc::toText(result.diagnostics));
    using Condition = script::detail::ParamModel::Condition;
    using Conditions = std::vector<Condition>;
    auto of = [](int param, std::vector<int> values) {
        Condition c;
        c.param = param;
        c.values = std::move(values);
        return c;
    };
    auto range = [](int param, double above, double below) {
        Condition c;
        c.param = param;
        c.above = above;
        c.below = below;
        return c;
    };
    const double inf = std::numeric_limits<double>::infinity();
    const std::vector<Conditions> expected = {
        {},                                      // on
        {of(0, {1})},                            // shape: while on
        {of(0, {1}), of(1, {0, 1})},             // gain: while on, unless a cut
        {of(1, {0})},                            // width: a bell's
        {},                                      // freeze
        {of(4, {0})},                            // rate: unless frozen
        {},                                      // voices
        {range(6, 1, inf)},                      // detune: above one voice
        {},                                      // depth
        {range(8, 0, inf), range(8, -inf, 90)},  // speed: between 0 and 90 %
    };
    const script::detail::NativeCode<script::ex_activity::Activity> native;
    REQUIRE(result.program->params().size() == expected.size());
    REQUIRE(native.params().size() == expected.size());
    for (size_t i = 0; i < expected.size(); ++i) {
        CAPTURE(i);
        CHECK(result.program->params()[i].activeWhen == expected[i]);
        CHECK(native.params()[i].activeWhen == expected[i]);
    }
}
