// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Portable control-stage tests: no real device, plugins or native windowing.
#include <doctest/doctest.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>

#include "supple/audio/audio_file.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {
struct PlayrateHost {
    fs::path root = fs::temp_directory_path() /
                    ("supple-playrate-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    DummyBackend backend{
        DummyBackend::Options{.mode = DummyBackend::Mode::Manual, .captureFrames = 8192}};
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;
    PlayrateHost() {
        fs::create_directories(root / "plugins");
        AppOptions options;
        options.workdir = root;
        options.pluginPath = {root / "plugins"};
        auto created = EngineApp::create(options);
        REQUIRE_MESSAGE(created, (created ? "" : created.error().toString()));
        app = std::move(*created);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
    }
    ~PlayrateHost() {
        bus.reset();
        app.reset();  // stop the engine/writer before destroying backend and files
        fs::remove_all(root);
    }
    Result<json::Value> execute(const char* command, std::string_view args = "{}",
                                std::string gesture = {}) {
        auto parsed = json::parse(args);
        REQUIRE(parsed);
        return bus->execute({command, *parsed, {cmd::Origin::Type::Test, "playrate", {}, gesture}});
    }
    json::Value run(const char* command, std::string_view args = "{}", std::string gesture = {}) {
        auto result = execute(command, args, std::move(gesture));
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    void start() {
        REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 1},
                                 {.anticipative = false, .workers = 0}));
    }
    void arm(bool midi = false) {
        run("track.add", R"({"name":"Input"})");
        run("track.set", midi ? R"({"track":"tr_1","armed":true,"record_midi":"*"})"
                              : R"({"track":"tr_1","armed":true,"record_input":[1]})");
    }
    std::vector<fs::path> files() const {
        std::vector<fs::path> out;
        for (const auto& entry : fs::recursive_directory_iterator(root))
            if (entry.is_regular_file()) out.push_back(entry.path());
        std::sort(out.begin(), out.end());
        return out;
    }
};

void recordError(const Result<json::Value>& result) {
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "record.playrate");
    CHECK_FALSE(result.error().message.empty());
    CHECK_FALSE(result.error().path.empty());
    CHECK_FALSE(result.error().hint.empty());
}
}  // namespace

TEST_CASE("master playrate commands: schemas, queries, gesture undo and save/reopen") {
    PlayrateHost h;
    auto info = h.run("transport.playrate.get");
    CHECK(info["requested_rate"].asDouble() == 1);
    CHECK(info["effective_rate"].asDouble() == 1);
    CHECK_FALSE(info["preserve_pitch"].asBool());
    CHECK(info.find("envelope") == nullptr);
    const auto initial = h.app->head().version;
    h.run("transport.playrate.set");
    CHECK(h.app->head().version == initial);
    h.run("transport.playrate.set", R"({"rate":0.25})", "rate-drag");
    h.run("transport.playrate.set", R"({"rate":4,"preserve_pitch":true})", "rate-drag");
    CHECK(h.app->versions().history().size() == 2);
    info = h.run("transport.playrate.get");
    CHECK(info["requested_rate"].asDouble() == 4);
    CHECK(info["effective_rate"].asDouble() == 1);  // control stage does not alter DSP
    CHECK(info["preserve_pitch"].asBool());
    const auto content = model::serializeProject(h.app->head());
    for (const char* args :
         {R"({"rate":0})", R"({"rate":4.001})", R"({"rate":true})", R"({"preserve_pitch":1})"}) {
        CHECK_FALSE(h.execute("transport.playrate.set", args));
        CHECK(model::serializeProject(h.app->head()) == content);
    }
    h.run("project.save", R"({"path":"rate.json"})");
    h.run("transport.playrate.set", R"({"rate":1,"preserve_pitch":false})");
    h.run("project.open", R"({"path":"rate.json"})");
    CHECK(h.app->head().project->playrate == model::Playrate{4, true});
    CHECK(model::serializeProject(h.app->head()) == content);
}

TEST_CASE("master playrate commands: tempo bake is atomic and retains tempo point IDs") {
    PlayrateHost h;
    h.run("tempo.set", R"({"points":[{"beat":0,"bpm":120,"ramp":true},
        {"beat":8,"bpm":180},{"beat":16,"bpm":240}]})");
    h.run("transport.playrate.set", R"({"rate":1.5,"preserve_pitch":true})");
    const auto before = h.app->head();
    const auto steps = h.app->versions().history().size();
    h.run("transport.playrate.apply-to-tempo");
    CHECK(h.app->versions().history().size() == steps + 1);
    CHECK(h.app->head().project->playrate == model::Playrate{1, true});
    CHECK(h.app->head().meters.begin()->id == before.meters.begin()->id);
    for (const auto& point : before.tempo) {
        const auto* baked = h.app->head().tempo.find(point.id);
        REQUIRE(baked);
        CHECK(baked->bpm == point.value->bpm * 1.5);
        CHECK(baked->beat == point.value->beat);
        CHECK(baked->ramp == point.value->ramp);
    }
    REQUIRE(h.app->undo());
    CHECK(model::serializeProject(h.app->head()) == model::serializeProject(before));
    h.run("tempo.set", R"({"points":[{"beat":0,"bpm":900}]})");
    const auto bytes = model::serializeProject(h.app->head());
    auto rejected = h.execute("transport.playrate.apply-to-tempo");
    REQUIRE_FALSE(rejected);
    CHECK(rejected.error().code == "tempo.range");
    CHECK(rejected.error().path.find("/args/tempo/points/") == 0);
    CHECK_FALSE(rejected.error().hint.empty());
    CHECK(model::serializeProject(h.app->head()) == bytes);
}

TEST_CASE("master playrate envelope commands: target, bounds, items and bake guard") {
    PlayrateHost h;
    h.run("track.add");
    CHECK_FALSE(h.execute("env.add", R"({"track":"tr_1","param":"playrate"})"));
    for (const char* value : {"0", "4.01", "true"}) {
        const auto args =
            std::string(R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":)") +
            value + "}]}";
        CHECK_FALSE(h.execute("env.add", args));
    }
    h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":1}]})");
    CHECK(h.run("transport.playrate.get")["envelope"].asString() == "en_1");
    CHECK_FALSE(h.execute("env.add", R"({"track":"tr_0","param":"playrate"})"));
    const auto version = h.app->head().version;
    auto refused = h.execute("transport.playrate.apply-to-tempo");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "playrate.envelope");
    CHECK(h.app->head().version == version);
    CHECK_FALSE(h.execute("autoitem.add", R"({"env":"en_1","position":0,"length":4,
        "baseline":1,"amplitude":2,"points":[{"beat":0,"value":3}]})"));
    h.run("env.set", R"({"env":"en_1","active":false})");
    h.run("transport.playrate.apply-to-tempo");
    CHECK(h.run("transport.playrate.get").find("envelope") == nullptr);
    h.run("env.points.set", R"({"env":"en_1","points":[]})");
    h.run("env.set", R"({"env":"en_1","active":true})");
    h.run("transport.playrate.apply-to-tempo");  // empty envelope does not drive the rate
    h.run("autoitem.add", R"({"env":"en_1","position":0,"length":4,
        "points":[{"beat":0,"value":1},{"beat":4,"value":2}]})");
    CHECK(h.run("transport.playrate.get")["envelope"].asString() == "en_1");
    CHECK_FALSE(h.execute("transport.playrate.apply-to-tempo"));
}

TEST_CASE("master playrate recording guard: no files opened for audio or MIDI") {
    for (bool midi : {false, true}) {
        PlayrateHost h;
        h.arm(midi);
        h.run("transport.playrate.set", R"({"rate":2})");
        h.start();
        const auto files = h.files();
        const auto version = h.app->head().version;
        recordError(h.execute("transport.record"));
        CHECK(h.files() == files);
        CHECK(h.app->head().version == version);
        RecordTarget target;
        target.track = model::Id{model::Kind::Track, 1};
        target.inputs = {0};
        target.file = h.root / "must-not-exist.wav";
        auto direct = h.app->engine()->startRecording({target});
        REQUIRE_FALSE(direct);
        CHECK(direct.error().code == "record.playrate");
        CHECK_FALSE(fs::exists(target.file));
        h.run("transport.playrate.set", R"({"rate":1})");
        h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[
            {"beat":0,"value":1},{"beat":100,"value":2}]})");
        recordError(h.execute("transport.record"));
        CHECK(h.files() == files);
        h.run("env.set", R"({"env":"en_1","active":false})");
        h.run("transport.record");
        CHECK(h.app->engine()->recording());
        h.run("transport.stop");
    }
}

TEST_CASE("master playrate recording guard: edits and engine loads roll back atomically") {
    PlayrateHost h;
    h.arm();
    h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":1}]})");
    h.run("env.set", R"({"env":"en_1","mode":"latch"})");
    h.start();
    h.run("transport.record");
    h.backend.run(4);
    const auto before = model::serializeProject(h.app->head());
    recordError(h.execute("transport.playrate.set", R"({"rate":2})"));
    recordError(h.execute("env.points.set", R"({"env":"en_1","points":[{"beat":0,"value":2}]})"));
    recordError(h.execute("autoitem.add", R"({"env":"en_1","position":0,"length":4,
        "points":[{"beat":0,"value":2}]})"));
    CHECK(model::serializeProject(h.app->head()) == before);
    CHECK_FALSE(h.execute("batch", R"({"steps":[
        {"command":"project.set","args":{"title":"must roll back"}},
        {"command":"transport.playrate.set","args":{"rate":2}}]})"));
    CHECK(model::serializeProject(h.app->head()) == before);
    model::EditTxn edit(h.app->head(), h.app->head().version + 1);
    auto project = edit.project();
    project.playrate.rate = 2;
    edit.setProject(project);
    const auto disallowed = edit.commit().value();
    auto refused = h.app->engine()->load(disallowed);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "record.playrate");
    CHECK(h.app->engine()->recording());
    h.run("transport.stop");
    const auto* envelope = h.app->head().envelopes.find({model::Kind::Envelope, 1});
    REQUIRE(envelope);
    CHECK(std::all_of(envelope->points.begin(), envelope->points.end(), [](const auto& p) {
        return p.value == 1;
    }));  // rejected knob moves cannot become a later latch hold
}

TEST_CASE("master playrate recording guard: history and branches cannot bypass unity") {
    for (bool redo : {false, true}) {
        PlayrateHost h;
        h.arm();
        h.run("branch.create", R"({"name":"unsafe"})");
        h.run("transport.playrate.set", R"({"rate":2})");
        h.run("branch.switch", R"({"name":"main"})");
        h.run("transport.playrate.set", R"({"rate":2})");
        if (redo)
            REQUIRE(h.app->undo());
        else
            h.run("transport.playrate.set", R"({"rate":1})");
        h.start();
        h.run("transport.record");
        const auto before = model::serializeProject(h.app->head());
        const auto result = redo ? h.app->redo() : h.app->undo();
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "record.playrate");
        CHECK(model::serializeProject(h.app->head()) == before);
        recordError(h.execute("branch.switch", R"({"name":"unsafe"})"));
        auto merged = h.app->mergeBranch(h.registry, "unsafe", "main");
        CHECK_FALSE(merged.conflicts.empty());
        CHECK(h.app->versions().branch() == "main");
        CHECK(model::serializeProject(h.app->head()) == before);
        CHECK(h.app->engine()->recording());
        h.run("transport.stop");
    }
}

TEST_CASE("master playrate writing: touch latch and write use strip capture timestamps") {
    for (const char* mode : {"touch", "latch", "write"}) {
        PlayrateHost h;
        h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":1}]})");
        const auto args = std::string(R"({"env":"en_1","mode":")") + mode + "\"}";
        h.run("env.set", args);
        h.start();
        h.run("transport.play");
        h.backend.run(8);
        h.run("transport.playrate.set", R"({"rate":1.25})", "write-rate");
        h.backend.run(8);
        h.run("transport.pause");
        const auto* envelope = h.app->head().envelopes.find({model::Kind::Envelope, 1});
        REQUIRE(envelope);
        CHECK(std::any_of(envelope->points.begin(), envelope->points.end(),
                          [](const auto& p) { return p.pos > 0 && p.value == 1.25; }));
        CHECK(h.run("transport.playrate.get")["effective_rate"].asDouble() == 1);
    }
}

TEST_CASE("master playrate controls leave audio at unity pending engine integration") {
    PlayrateHost h;
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(4800));
    for (std::size_t i = 0; i < tone.frames(); ++i)
        tone.channels[0][i] = tone.channels[1][i] = i % 64 < 32 ? .2f : -.2f;
    REQUIRE(audio::writeWav(h.root / "tone.wav", tone));
    h.run("track.add");
    auto args = json::Value::object();
    args["track"] = "tr_1";
    args["audio_file"] = (h.root / "tone.wav").string();
    h.run("item.add", json::writeCompact(args));
    OfflineSettings settings;
    settings.endSeconds = .05;
    settings.threads = 1;
    const auto unity = h.app->renderMix(settings);
    REQUIRE(unity);
    h.run("transport.playrate.set", R"({"rate":2,"preserve_pitch":true})");
    h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":0.25}]})");
    const auto pending = h.app->renderMix(settings);
    REQUIRE(pending);
    CHECK(pending->audio.channels == unity->audio.channels);
    h.start();
    h.run("transport.play");
    h.backend.run(20);
    CHECK(h.app->engine()->transport().sample == 2560);
    for (std::size_t channel = 0; channel < 2; ++channel) {
        const auto& captured = h.backend.capture().channels[channel];
        const auto& expected = unity->audio.channels[channel];
        REQUIRE(captured.size() >= expected.size());
        double maxDiff = 0;
        for (std::size_t i = 0; i < expected.size(); ++i)
            maxDiff = std::max(maxDiff, std::abs(double(captured[i]) - double(expected[i])));
        CHECK(maxDiff == 0);
    }
    CHECK(h.run("transport.playrate.get")["effective_rate"].asDouble() == 1);
    auto refused = h.execute("transport.playrate.apply-to-tempo");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "playrate.playing");
    h.run("transport.stop");
}

TEST_CASE("master playrate recording guard distinguishes fast forward from safe replay") {
    for (bool safeReplay : {false, true}) {
        PlayrateHost h;
        h.arm();
        if (safeReplay) h.run("transport.playrate.set", R"({"rate":2})");
        h.run("branch.create", R"({"name":"source"})");
        if (safeReplay)
            h.run("project.set", R"({"title":"safe replay"})");
        else
            h.run("transport.playrate.set", R"({"rate":2})");
        h.run("branch.switch", R"({"name":"main"})");
        if (safeReplay) h.run("transport.playrate.set", R"({"rate":1})");
        h.start();
        h.run("transport.record");
        const auto before = model::serializeProject(h.app->head());
        const auto merged = h.app->mergeBranch(h.registry, "source", "main");
        if (safeReplay) {
            CHECK(merged.conflicts.empty());
            CHECK(h.app->head().project->title == "safe replay");
            CHECK(h.app->head().project->playrate.rate == 1);
        } else {
            CHECK_FALSE(merged.conflicts.empty());
            CHECK(model::serializeProject(h.app->head()) == before);
        }
        h.run("transport.stop");
    }
}

TEST_CASE("master playrate Lua bindings expose nested commands and tempo bake") {
    PlayrateHost h;
    h.app->attachCommands(*h.bus);
    auto scripted = h.run("script.run", R"json({"code":
        "q.transport.playrate.set{rate=1.5}; local before=q.transport.playrate.get(); q.transport.playrate['apply-to-tempo'](); return {before=before,after=q.transport.playrate.get()}"
    })json");
    CHECK(scripted["result"]["before"]["requested_rate"].asDouble() == 1.5);
    CHECK(scripted["result"]["after"]["requested_rate"].asDouble() == 1);
    CHECK(h.app->head().timeMap->bpmAt(0) == 180);
}

TEST_CASE("master playrate UI accessor reads cached curves without commands or history") {
    PlayrateHost h;
    auto state = h.app->playrateState();
    CHECK(state.requestedRate == 1);
    CHECK(state.effectiveRate == 1);
    CHECK_FALSE(state.envelope);
    CHECK_FALSE(state.envelopeValue);
    h.run("transport.playrate.set", R"({"rate":1.137,"preserve_pitch":true})");
    h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[
        {"beat":0,"value":0.5},{"beat":1,"value":2.5}]})");
    const auto version = h.app->head().version;
    const auto steps = h.app->versions().history().size();
    for (int i = 0; i < 1000; ++i) {
        state = h.app->playrateState();
        CHECK(state.requestedRate == 1.137);
        CHECK(state.preservePitch);
        CHECK(state.envelope == model::Id{model::Kind::Envelope, 1});
        CHECK(state.envelopeValue == .5);
    }
    CHECK(h.app->head().version == version);
    CHECK(h.app->versions().history().size() == steps);
    h.run("env.set", R"({"env":"en_1","active":false})");
    CHECK_FALSE(h.app->playrateState().envelopeValue);
    REQUIRE(h.app->undo());
    CHECK(h.app->playrateState().envelopeValue == .5);
}

TEST_CASE("master playrate writing: a failed batch cannot leave a later latch hold") {
    PlayrateHost h;
    h.run("env.add", R"({"track":"tr_0","param":"playrate","points":[{"beat":0,"value":1}]})");
    h.run("env.set", R"({"env":"en_1","mode":"latch"})");
    h.start();
    h.run("transport.play");
    h.backend.run(8);
    CHECK_FALSE(h.execute("batch", R"({"steps":[
        {"command":"transport.playrate.set","args":{"rate":1.25}},
        {"command":"track.set","args":{"track":"tr_999","mute":true}}]})"));
    CHECK(h.app->head().project->playrate.rate == 1);
    h.backend.run(8);
    h.run("transport.pause");
    const auto* envelope = h.app->head().envelopes.find({model::Kind::Envelope, 1});
    REQUIRE(envelope);
    CHECK(std::all_of(envelope->points.begin(), envelope->points.end(),
                      [](const auto& p) { return p.value == 1; }));
}
