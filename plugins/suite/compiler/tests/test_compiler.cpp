// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The compiler end to end: scripts compile, run and compute what C++ would;
// what the subset refuses is refused with the diagnostic that teaches it.
#include <doctest/doctest.h>

#include <cmath>
#include <fstream>
#include <sstream>

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
    // Names as C++ looks them up: a member hides a factory of its name, a
    // local's name in its own initializer is that local, and the factories'
    // names are the runtime's.
    CHECK(firstError(plugin("Param ratio = ratio(\"Ratio\", 1, 20, 4);\n"
                            "void process(Block& io) {}")) == "script.name.hidden");
    CHECK(firstError(plugin("Param amount = ratio(\"Ratio\", 1, 20, 4).withKey(\"ratio\");\n"
                            "void process(Block& io) {}")) == "");
    CHECK(firstError(plugin("Param mix = percent(\"Mix\", 50);\n"
                            "void process(Block& io) { double mix = mix * 2.0; }")) ==
          "script.name.self");
    CHECK(firstError(plugin("void process(Block& io) { double sin = sin(0.5); }")) ==
          "script.name.self");
    CHECK(firstError(head + "double percent(double x) { return x * 0.01; }\n" +
                     "struct T : Effect {\n" + info + "void process(Block& io) {}\n};\n") ==
          "script.name.builtin");
    // Files: members of the plugin, declared with their kind, never copied
    // or assigned.
    CHECK(firstError(plugin("AudioFile s = audioFile(\"Sample\").help(\"A sound.\");\n"
                            "Convolver r = impulseResponse(\"Room\");\n"
                            "void process(Block& io) { double x = s.read(0, 1.5) + "
                            "r.process(0, 0.5); }")) == "");
    CHECK(firstError(plugin("AudioFile s;\nvoid process(Block& io) {}")) ==
          "script.file.declaration");
    CHECK(firstError(plugin("Convolver r = audioFile(\"Room\");\nvoid process(Block& io) {}")) ==
          "script.file.kind");
    CHECK(firstError(plugin("AudioFile s = audioFile(\"S\").smoothed(5);\n"
                            "void process(Block& io) {}")) == "script.file.modifier");
    CHECK(firstError(plugin("AudioFile s = audioFile(\"S\");\n"
                            "void process(Block& io) { auto t = s; }")) == "script.local.file");
    CHECK(firstError(plugin("AudioFile s = audioFile(\"S\");\nAudioFile t = audioFile(\"T\");\n"
                            "void process(Block& io) { s = t; }")) == "script.file.read_only");
    CHECK(firstError(plugin("Array<AudioFile, 2> kit;\nvoid process(Block& io) {}")) ==
          "script.file.array");
    CHECK(firstError(plugin("AudioFile a = audioFile(\"S\");\n"
                            "AudioFile b = audioFile(\"T\").withKey(\"a\");\n"
                            "void process(Block& io) {}")) == "script.file.key");
    CHECK(firstError(head + "struct V { AudioFile s = audioFile(\"S\"); };\nstruct T : Effect {\n" +
                     info + "void process(Block& io) {}\n};\n") == "script.file.location");
    // Identities every plugin format and the framework accept.
    auto withInfo = [&](const std::string& fields) {
        return head + "struct T : Effect {\nstatic constexpr Info info = {.name = \"T\", " +
               fields + "};\nvoid process(Block& io) {}\n};\n";
    };
    CHECK(firstError(withInfo(".id = \"com.example.tilt_eq\"")) == "script.plugin.id");
    CHECK(firstError(withInfo(".id = \"com..tilt\"")) == "script.plugin.id");
    CHECK(firstError(withInfo(".id = \"com.example.tilt-eq\"")) == "");
    CHECK(firstError(withInfo(".id = \"com.example.t\", .version = \"1.2\"")) ==
          "script.plugin.info");
    CHECK(firstError(withInfo(".id = \"com.example.t\", .version = \"1.2.10\"")) == "");
    // Instruments: notes reach voices through the voice hooks, which only an
    // instrument has; scripts send no notes, so no note effects.
    auto instrument = [&](const std::string& body) {
        return head + "struct T : Instrument {\n" + info + body + "\n};\n";
    };
    CHECK(firstError(instrument("void process(Block& io) {}")) == "script.plugin.voices");
    CHECK(firstError(instrument("void startVoice(int v) {}\nvoid process(Block& io) {}")) ==
          "script.plugin.voices");
    CHECK(firstError(instrument("void startVoice(double v) {}\nvoid releaseVoice(int v) {}\n"
                                "void process(Block& io) {}")) == "script.plugin.hook");
    CHECK(firstError(plugin("void releaseVoice(int v) {}\nvoid process(Block& io) {}")) ==
          "script.plugin.hook");
    CHECK(firstError(plugin("void process(Block& io) { int k = voiceKey(0); }")) ==
          "script.plugin.instrument");
    CHECK(firstError(head + "struct T : NoteEffect {\n" + info + "};\n") == "script.plugin.kind");
    // Per note: expressions and modulation, an instrument's only.
    CHECK(firstError(
              instrument("Param cutoff = frequency(\"Cutoff\", 20, 20000, 1000).polyModulated();\n"
                         "void startVoice(int v) {}\nvoid releaseVoice(int v) {}\n"
                         "void process(Block& io) { double f = cutoff.forVoice(0) * "
                         "(1.0 + voiceBrightness(0)) * voiceVolume(0); }")) == "");
    CHECK(
        firstError(plugin("Param cutoff = frequency(\"Cutoff\", 20, 20000, 1000).polyModulated();\n"
                          "void process(Block& io) {}")) == "script.param.poly");
    CHECK(firstError(plugin("void process(Block& io) { double p = voicePressure(0); }")) ==
          "script.plugin.instrument");
}

TEST_CASE("editor code reads parameters and Shared members, and keeps the rest apart") {
    const std::string head = "#include <supple/script.h>\n";
    auto plugin = [&](const std::string& body) {
        return head + "struct T : Effect {\n" +
               "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n" +
               "Param gain = decibels(\"Gain\", -24, 24, 0);\n" +
               "Param out = decibels(\"Out\", -60, 0, 0).meter();\n" + body + "\n};\n";
    };
    const std::string process = "void process(Block& io) {}\n";
    // What editor code may do: read parameters and Shared members, keep
    // members of its own, format text, place controls.
    CHECK(firstError(
              plugin("Shared<double> level;\nSharedArray<float, 8> scope;\n" + process +
                     "void draw(Canvas& g) {\n"
                     "  const char* text = format(\"%.1f dB\", level.get() + gain + scope[1]);\n"
                     "  g.fillText(text, 10, 20);\n"
                     "  g.fillStyle(Theme::accent);\n"
                     "  g.textAlign(TextAlign::Center);\n"
                     "  g.fillText(gain.text(), 10, 40);\n"
                     "}")) == "");
    CHECK(firstError(plugin("double zoom = 1.0;\nAudioFile s = audioFile(\"S\");\n" + process +
                            "void editor(Editor& e) {\n"
                            "  e.size(300, 200);\n"
                            "  e.knob(gain, 10, 10, 80, 90);\n"
                            "  e.readout(out, 100, 10, 120, 30);\n"
                            "  e.file(s, 10, 110, 200, 40);\n"
                            "  zoom = 2.0;\n"
                            "}\n"
                            "void draw(Canvas& g) { g.scale(zoom, zoom); }")) == "");
    // A member both sides use, and one only prepare() fills.
    CHECK(firstError(plugin("double peak = 0.0;\nvoid process(Block& io) { peak = 1.0; }\n"
                            "void draw(Canvas& g) { g.fillRect(0, 0, peak, 1); }")) ==
          "script.editor.member");
    CHECK(firstError(plugin("double table = 0.0;\nvoid prepare() { table = 2.0; }\n" + process +
                            "void draw(Canvas& g) { g.fillRect(0, 0, table, 1); }")) ==
          "script.editor.member");
    CHECK(firstError(plugin("AudioFile s = audioFile(\"S\");\n" + process +
                            "void draw(Canvas& g) { double n = s.frames(); }")) ==
          "script.editor.file");
    // What editor code may not change.
    CHECK(firstError(plugin("Shared<double> level;\n" + process +
                            "void draw(Canvas& g) { level.set(0.0); }")) ==
          "script.editor.shared_write");
    CHECK(firstError(plugin("SharedArray<float, 4> s;\n" + process +
                            "void draw(Canvas& g) { s[0] = 1; }")) == "script.editor.shared_write");
    CHECK(firstError(plugin(process + "void draw(Canvas& g) { out.set(1.0); }")) ==
          "script.editor.param_write");
    // Each side's built-ins.
    CHECK(firstError(plugin("void process(Block& io) { const char* t = format(\"%d\", 1); }")) ==
          "script.editor.call");
    CHECK(firstError(plugin("DelayLine d;\n" + process +
                            "void draw(Canvas& g) { d.prepare(10); }")) == "script.editor.call");
    // format()'s pattern is checked as it is written.
    CHECK(firstError(plugin(process +
                            "void draw(Canvas& g) { g.fillText(format(\"%s\", 1.0), 0, 0); }")) ==
          "script.format.conversion");
    CHECK(
        firstError(plugin(
            process + "void draw(Canvas& g) { g.fillText(format(\"%d and %d\", 1.0), 0, 0); }")) ==
        "script.format.count");
    // Shared state belongs to the plugin struct, starts at zero, holds numbers.
    CHECK(firstError(plugin(process + "void draw(Canvas& g) { Shared<double> x; }")) ==
          "script.shared.where");
    CHECK(firstError(plugin("Shared<double> level = 1;\n" + process)) == "script.shared.initial");
    CHECK(firstError(plugin("Shared<Biquad> f;\n" + process)) == "script.shared.type");
    CHECK(firstError(head + "struct V { Shared<double> x; };\nstruct T : Effect {\n" +
                     "static constexpr Info info = {.name = \"T\", .id = \"com.example.t\"};\n" +
                     process + "};\n") == "script.shared.where");
    // The hooks, as the runtime calls them.
    CHECK(firstError(plugin(process + "void draw() {}")) == "script.plugin.hook");
    CHECK(firstError(plugin(process + "void paint(Canvas& g) {}")) == "script.plugin.hook_name");
    // pointer() edits parameters, as a user would; nothing else in the
    // editor does, and it needs a draw() to act on.
    const std::string draw = "void draw(Canvas& g) {}\n";
    CHECK(firstError(plugin("bool held = false;\n" + process + draw +
                            "void pointer(const Pointer& p) {\n"
                            "  if (p.action() == PointerAction::Drag && held)\n"
                            "    move(xToHz(p.x(), 20, 20000, 0, 100));\n"
                            "  if (p.doubleClick()) gain.reset();\n"
                            "  held = p.within(1, 2, 3) || p.over(0, 0, 4, 4);\n"
                            "}\n"
                            "void move(double hz) { gain.editNormalized(hz / 20000); }")) == "");
    CHECK(firstError(plugin(process + "void draw(Canvas& g) { gain.edit(0); }")) ==
          "script.editor.edit");
    CHECK(firstError(plugin(process + "void draw(Canvas& g) { nudge(); }\n"
                                      "void nudge() { gain.reset(); }")) == "script.editor.edit");
    CHECK(firstError(plugin("void process(Block& io) { gain.edit(0); }\n" + draw)) ==
          "script.editor.call");
    CHECK(firstError(plugin(process + "void pointer(const Pointer& p) {}")) ==
          "script.editor.pointer");
    CHECK(firstError(plugin(process + draw + "int pointer(const Pointer& p) { return 0; }")) ==
          "script.plugin.hook");
    CHECK(firstError(plugin(process + draw + "void onMouseDown(const Pointer& p) {}")) ==
          "script.plugin.hook_name");
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

TEST_CASE("an instrument takes its notes through voice hooks") {
    const std::string source = R"(#include <supple/script.h>

struct Synth : Instrument {
    static constexpr Info info = {.name = "Synth", .id = "com.example.synth", .voices = 4};
    Param glide = milliseconds("Glide", 0, 500, 0);

    struct Voice {
        Oscillator osc;
        Adsr env;
    };
    Array<Voice, 4> voice;

    void update() {
        setVoiceMode(glide > 0 ? VoiceMode::Legato : VoiceMode::Poly);
        setGlide(glide);
    }
    void startVoice(int v, bool retrigger) {
        if (retrigger) voice[v].env.gate(true);
    }
    void releaseVoice(int v) { voice[v].env.gate(false); }
    void stopVoice(int v) { voice[v].env.kill(); }
    void process(Block& io) {
        for (int v = 0; v < voiceCount(); ++v) {
            if (!voicePlaying(v)) continue;
            for (int i = 0; i < io.frames; ++i) {
                voice[v].osc.setFrequency(voiceHz(v, i));
                const double tone = voice[v].osc.next();
                const double x = tone * voice[v].env.next() * voiceVelocity(v);
                for (int c = 0; c < io.channels; ++c) io[c][i] += x;
            }
            if (!voice[v].env.active()) voiceFinished(v);
        }
    }
};
)";
    auto r = compileOk(source);
    CHECK(r.program->kind() == scriptc::PluginKind::Instrument);
    CHECK(r.program->info().voices == 4);
    CHECK(r.program->startVoiceRetrigger() != nullptr);
    CHECK(r.program->startVoice() == nullptr);
    CHECK(r.program->releaseVoice() != nullptr);
    CHECK(r.program->stopVoice() != nullptr);
}

TEST_CASE("every whole script in the guides compiles") {
    // A guide's ```cpp blocks that are scripts (they include the runtime):
    // what an agent copies first must be right.
    int scripts = 0;
    for (const char* guide : {"dsp-language.md", "dsp-workflow.md"}) {
        std::ifstream in(std::string(SUPPLE_SKILL_GUIDES) + "/" + guide);
        REQUIRE_MESSAGE(in, guide);
        std::stringstream all;
        all << in.rdbuf();
        const std::string text = all.str();
        for (size_t at = text.find("```cpp\n"); at != std::string::npos;
             at = text.find("```cpp\n", at + 1)) {
            const size_t begin = at + 7, end = text.find("```", begin);
            REQUIRE(end != std::string::npos);
            const std::string block = text.substr(begin, end - begin);
            if (block.find("#include <supple/script.h>") == std::string::npos) continue;
            ++scripts;
            auto r = scriptc::compile(block);
            INFO(guide << ":\n" << block);
            CHECK_MESSAGE(r.program != nullptr, scriptc::toText(r.diagnostics));
        }
    }
    CHECK(scripts >= 3);
}
