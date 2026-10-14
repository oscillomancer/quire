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
#include <sstream>

#include "runner.h"
#include "supple/script_native.h"

// The examples, as a source export includes a script: inside the runtime's
// namespace, after <supple/script.h>.
namespace supple::script {
namespace ex_lvalues {
#include "lvalues.h"
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
        CHECK(p.offAtMinimum == q.offAtMinimum);
        CHECK(p.output == q.output);
        CHECK(p.bypass == q.bypass);
        CHECK(p.hidden == q.hidden);
        CHECK(p.roles == q.roles);
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
