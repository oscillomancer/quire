// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The compiler end to end: scripts compile, run and compute what C++ would;
// what the subset refuses is refused with the diagnostic that teaches it.
#include <doctest/doctest.h>

#include <cmath>

#include "runner.h"

using namespace supple;
using scripttest::Audio;
using scripttest::JitRunner;

namespace {

scriptc::Result compileOk(const std::string& source, const scriptc::Options& options = {}) {
    auto r = scriptc::compile(source, options);
    INFO(scriptc::toText(r.diagnostics));
    REQUIRE(r.program);
    return r;
}

// The first error's code, or "" when the script compiles. Every error says
// how to fix it.
std::string firstError(const std::string& source) {
    auto r = scriptc::compile(source);
    for (const auto& m : r.diagnostics) {
        CHECK_MESSAGE(!m.hint.empty(), m.code << ": " << m.message);
        if (m.error) return m.code;
    }
    return "";
}

const char* kGain = R"(#include <supple/script.h>

struct Gain : Effect {
    static constexpr Info info = {.name = "Gain", .id = "com.example.gain"};
    Param gain = decibels("Gain", -24, 24, 0);
    double g = 1.0;

    void update() { g = dbToGain(gain); }
    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= g;
    }
};
)";

}  // namespace

TEST_CASE("a gain script compiles and applies its gain") {
    for (bool interpret : {false, true}) {
        CAPTURE(interpret);
        scriptc::Options options;
        options.interpret = interpret;
        auto result = compileOk(kGain, options);
        const auto& program = *result.program;
        CHECK(program.info().name == "Gain");
        CHECK(program.info().id == "com.example.gain");
        CHECK(program.kind() == scriptc::PluginKind::Effect);
        REQUIRE(program.params().size() == 1);
        const auto& p = program.params()[0];
        CHECK(p.key == "gain");
        CHECK(p.name == "Gain");
        CHECK(p.unit == scriptc::ParamModel::Unit::Decibels);
        CHECK(p.min == -24.0);
        CHECK(p.max == 24.0);

        JitRunner run(result.program);
        run.prepare();
        run.set(0, -6.0);
        run.update();
        Audio audio(2, 100);
        for (int i = 0; i < 100; ++i)
            audio.data[0][static_cast<size_t>(i)] = audio.data[1][static_cast<size_t>(i)] = 0.5;
        run.process(audio, 32);
        const double expected = 0.5 * std::pow(10.0, -6.0 * 0.05);
        CHECK(audio.data[0][0] == expected);
        CHECK(audio.data[1][99] == expected);
    }
}

TEST_CASE("the first scripts' mistakes are refused with a hint") {
    CHECK(firstError("struct X : Effect {};") == "script.include.missing");
    const std::string head = "#include <supple/script.h>\n";
    const std::string info =
        "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n";
    auto plugin = [&](const std::string& body) {
        return head + "struct T : Effect {\n" + info + body + "\n};\n";
    };
    CHECK(firstError(plugin("void process(Block& io) { float buf[16]; }")) ==
          "script.unsupported.c_array");
    CHECK(firstError(plugin("void process(Block& io) { double* p = nullptr; }")) ==
          "script.unsupported.pointer");
    CHECK(firstError(plugin(
              "void process(Block& io) { int n = io.frames; double x = 1.5; int k = x; }")) ==
          "script.type.float_to_int");
    CHECK(firstError(plugin("void process(Block& io) { int a = io.frames / io.channels; }")) ==
          "script.ub.division");
    CHECK(firstError(plugin("double phase; void process(Block& io) {}")) ==
          "script.member.initial");
    CHECK(firstError(plugin("void process(Block& io) { double x; }")) ==
          "script.local.uninitialized");
    CHECK(firstError(plugin("Buffer<float> b; void process(Block& io) { b.resize(10); }")) ==
          "script.rt.allocation");
    CHECK(firstError(plugin("DelayLine d; void process(Block& io) { d.prepare(100); }")) ==
          "script.rt.allocation");
    CHECK(firstError(plugin("void process(Block& io) { double y = sin(1); }")) == "");
    CHECK(firstError(plugin("void process(Block& io) { double y = min(1, 0.5); }")) ==
          "script.call.ambiguous");
    CHECK(firstError(plugin("void process(Block& io) { double y = M_PI; }")) ==
          "script.name.unknown");
    CHECK(firstError(plugin("void processBlock(Block& io) {}")) == "script.plugin.hook_name");
    CHECK(firstError(
              plugin("Random r; void process(Block& io) { double y = r.next() - r.next(); }")) ==
          "script.ub.order");
    CHECK(firstError(plugin(
              "Random r; Random s; void process(Block& io) { double y = r.next() - s.next(); }")) ==
          "");
    CHECK(firstError(plugin(
              "int f(int n) { return n < 1 ? 0 : f(n - 1); } void process(Block& io) { f(3); }")) ==
          "script.call.recursion");
    CHECK(firstError(plugin("int f(int n) { if (n > 0) return 1; } void process(Block& io) {}")) ==
          "script.return.missing_path");
    CHECK(firstError(plugin("Param p = continuous(\"P\", 0, 1, 0, Unit::Furlongs);\n"
                            "void process(Block& io) {}")) == "script.param.unit");
    CHECK(firstError(plugin("Param p = decibels(\"P\", 0, 1, 0).curved(Unit::Hertz);\n"
                            "void process(Block& io) {}")) == "script.param.modifier");
    // Scripts receive no notes yet: an instrument would compile and stay silent.
    CHECK(firstError(head + "struct T : Instrument {\n" + info +
                     "void process(Block& io) {}\n};\n") == "script.plugin.kind");
}

TEST_CASE("continuous() takes a unit, for the units without a factory") {
    const std::string source = R"(#include <supple/script.h>

struct T : Effect {
    static constexpr Info info = {.name = "T", .id = "com.example.t"};
    Param tempo = continuous("Tempo", 40, 240, 120, Unit::Bpm);
    Param plain = continuous("Amount", 0, 1, 0.5);
    void process(Block& io) {}
};
)";
    auto r = compileOk(source);
    REQUIRE(r.program->params().size() == 2);
    CHECK(r.program->params()[0].unit == scriptc::ParamModel::Unit::Bpm);
    CHECK(r.program->params()[1].unit == scriptc::ParamModel::Unit::None);
}
