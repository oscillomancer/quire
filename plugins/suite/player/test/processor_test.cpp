// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Scripts as the player runs them: through the framework's Engine, against
// the compiler's own runner, stopped when they spin, reset to what prepare
// left, and replaced while they play.
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "Catalog.h"
#include "Check.h"
#include "Processor.h"
#include "core/Catalog.h"
#include "core/Engine.h"
#include "runner.h"

using namespace supple;
using namespace supple::player;
using namespace supple::suite;

namespace {

using Engine = BasicEngine<ScriptProcessor>;

std::shared_ptr<const ScriptDefinition> compileText(const std::string& source) {
    std::string problems;
    auto d = compileScript("test.cpp", source, &problems);
    CHECK_MSG(d != nullptr, "%s", problems.c_str());
    return d;
}

std::string libraryScript(const char* name) {
    std::string text;
    for (const auto& folder : libraryFolders())
        if (readScript(folder / name / (std::string(name) + ".cpp"), text)) return text;
    CHECK_MSG(false, "no script %s in the library", name);
    return text;
}

// A noise burst, rendered in blocks of `block` through an activated engine.
std::vector<double> render(Engine& engine, int frames, int block = 64) {
    std::vector<double> left(static_cast<size_t>(frames)), right(static_cast<size_t>(frames));
    uint32_t seed = 1;
    for (int i = 0; i < frames; ++i) {
        seed = seed * 1664525u + 1013904223u;
        left[static_cast<size_t>(i)] = right[static_cast<size_t>(i)] =
            (seed >> 9) / 8388608.0 - 1.0;
    }
    for (int start = 0; start < frames; start += block) {
        const int n = std::min(block, frames - start);
        double* io[] = {left.data() + start, right.data() + start};
        engine.process(io, io, n);
    }
    return left;
}

void setPlain(Engine& engine, uint32_t id, double plain) {
    const ParamTable& t = engine.definition()->params();
    const int i = t.indexOf(id);
    engine.store().set(static_cast<size_t>(i), toHost(t.at(static_cast<size_t>(i)), plain));
}

const char* const kSpinner = R"(#include <supple/script.h>
struct Spin : Effect {
    static constexpr Info info = {.name = "Spin", .id = "com.example.spin"};
    Param go = toggle("Go", false);
    int n = 0;
    void process(Block& io) {
        while (go.on()) ++n;
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= 0.5;
    }
};
)";

}  // namespace

TEST(a_script_becomes_a_definition_with_its_keys_units_and_ids) {
    const auto d = compileText(libraryScript("tilt"));
    CHECK(d->info().id == "com.example.tilt" && d->info().name == "Tilt" &&
          d->info().vendor == "Example");
    CHECK(d->info().stateTag.size() == 4 && d->info().stateTag == stateTagFor("com.example.tilt"));
    CHECK(d->info().offers(2) && d->info().offers(1));
    const ParamTable& t = d->params();
    CHECK(t.count() == 3);
    const ParamDef& pivot = t.at(1);
    CHECK(pivot.unit == supple::Unit::Hertz && pivot.taper == Taper::Log && pivot.hostTapered());
    CHECK(t.at(0).hostTapered());  // linear, but normalized for semantic hosts
    CHECK(t.at(2).smoothing == 20.0);
    CHECK(d->meaning().params[1].key == "pivot");
    CHECK(d->meaning().patchType == "supple.tilt-patch/1.0");
    const auto percent = compileText(R"(#include <supple/script.h>
struct P : Effect {
    static constexpr Info info = {.name = "P", .id = "com.example.p"};
    Param mix = percent("Mix", 25);
    Param mode = choice("Mode", {"Soft Clip", "Hard"}, 1);
    void process(Block& io) { io[0][0] *= mix; }
};
)");
    CHECK(percent->params().at(0).max == 1.0 && percent->params().at(0).def == 0.25);
    CHECK(percent->meaning().params[1].choiceKeys ==
          (std::vector<std::string>{"soft_clip", "hard"}));
}

TEST(the_engine_plays_a_script_as_its_own_runner_does_bit_for_bit) {
    const auto d = compileText(libraryScript("tilt"));
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    setPlain(engine, d->params().at(0).id, 4.5);
    setPlain(engine, d->params().at(1).id, 700.0);
    const std::vector<double> played = render(engine, 4096);

    // The runner gets what the engine hands the script: the host value's
    // plain value, which a log taper rounds to its own grid.
    scripttest::JitRunner runner(std::static_pointer_cast<const scriptc::Program>(d->code()),
                                 48000.0, 64, 2);
    const ParamTable& t = d->params();
    runner.setScript(0, toPlain(t.at(0), toHost(t.at(0), 4.5)));
    runner.setScript(1, toPlain(t.at(1), toHost(t.at(1), 700.0)));
    runner.prepare();
    runner.update();
    scripttest::Audio audio(2, 4096);
    uint32_t seed = 1;
    for (int i = 0; i < 4096; ++i) {
        seed = seed * 1664525u + 1013904223u;
        audio.data[0][static_cast<size_t>(i)] = audio.data[1][static_cast<size_t>(i)] =
            (seed >> 9) / 8388608.0 - 1.0;
    }
    runner.process(audio, 64);
    size_t first = 0;
    while (first < played.size() && played[first] == audio.data[0][first]) ++first;
    CHECK_MSG(first == played.size(), "first difference at %zu: %.17g vs %.17g", first,
              first < played.size() ? played[first] : 0.0,
              first < played.size() ? audio.data[0][first] : 0.0);
}

TEST(a_reset_returns_the_script_to_what_prepare_left) {
    const auto d = compileText(libraryScript("tilt"));
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    setPlain(engine, d->params().at(0).id, -3.0);
    const std::vector<double> first = render(engine, 2048);
    engine.reset();
    CHECK(render(engine, 2048) == first);
}

TEST(a_script_that_spins_is_stopped_and_silent_and_says_where) {
    const auto d = compileText(kSpinner);
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    CHECK(std::fabs(render(engine, 256).back()) > 0.0);
    setPlain(engine, d->params().at(0).id, 1.0);
    const std::vector<double> out = render(engine, 256);
    CHECK(out.back() == 0.0);
    const auto& status = engine.processor().instance().status;
    CHECK(status.budgetExceeded.load() >= 1);
    CHECK(status.budgetLine.load() == 7);
    // Off again, it stays stopped until it is prepared again.
    setPlain(engine, d->params().at(0).id, 0.0);
    CHECK(render(engine, 256).back() == 0.0);
    engine.activate(48000.0, 64, 2, 2);
    CHECK(std::fabs(render(engine, 256).back()) > 0.0);
}

TEST(a_new_compile_replaces_the_running_one_with_a_crossfade) {
    // Stateless, so the new code's output is the reference's exactly.
    const std::string level = R"(#include <supple/script.h>
struct L : Effect {
    static constexpr Info info = {.name = "L", .id = "com.example.level"};
    Param level = decibels("Level", -12, 12, 0);
    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= dbToGain(level);
    }
};
)";
    std::string louder = level;
    louder.replace(louder.find("*= dbToGain"), 11, "*= 2.0 * dbToGain");
    const auto d = compileText(level);
    const auto next = compileText(louder);
    CHECK(sameShape(*next, *d));
    Engine engine(d);
    engine.activate(48000.0, 64, 2, 2);
    const std::vector<double> before = render(engine, 512);
    ParamValues values(d->params());
    engine.processor().swap(next->code(), values);
    // During the fade the output lies between the two; past it, the new code's.
    const std::vector<double> fading = render(engine, 64);
    CHECK(std::fabs(fading[32]) > std::fabs(before[32]) &&
          std::fabs(fading[32]) < 2.0 * std::fabs(before[32]));
    render(engine, 512);
    Engine reference(next);
    reference.activate(48000.0, 64, 2, 2);
    CHECK(render(engine, 512) == render(reference, 512));
    engine.processor().collect();
}

TEST(the_library_lists_a_plugin_per_script_that_compiles) {
    const std::vector<DefinitionPtr> plugins = catalog().list();
    CHECK(plugins.size() == 2);
    CHECK(catalog().find("com.example.tilt") != nullptr);
    CHECK(catalog().find("com.suppleaudio.script-player") != nullptr);
    CHECK(catalog().list()[0] == plugins[0]);  // unchanged, the same plugin
}

SUPPLE_TEST_MAIN
