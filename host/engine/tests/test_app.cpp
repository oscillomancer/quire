#include <doctest/doctest.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

#include <supple/audio/audio_file.h>
#include <supple/base/files.h>
#include <supple/base/sha256.h>
#include <supple/engine/app.h>
#include <supple/host/clap_host.h>
#include <supple/model/schemas.h>

#include <supple/analysis/analysis.h>
#include <atomic>
#include "fake_jack.h"
#include "history_sequence.h"
#include "render_commands.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {

const fs::path kFixture = FIXTURE_CLAP_PATH;

struct Root {
    fs::path path = fs::temp_directory_path() / ("supple-engine-app-" + std::to_string(::getpid()));
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

std::unique_ptr<EngineApp> makeApp(const fs::path& dir) {
    AppOptions o;
    o.workdir = dir;
    o.chainDir = dir / "chains";  // never the user's config
    o.templateDir = dir / "templates";
    o.pluginPath = {kFixture};
    auto app = EngineApp::create(o, {.console9 = false});
    REQUIRE(app);
    return std::move(*app);
}

json::Value j(std::string_view text) { return json::parse(text).value(); }
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

}  // namespace

TEST_CASE("seeded command histories preserve invariants, undo and saved projects") {
    Root root;
    for (std::uint32_t seed : {1U, 7U, 42U, 2026U}) {
        cmd::Registry registry;
        registerEngineCommands(registry);
        auto app = makeApp(root.path / std::to_string(seed));
        cmd::Dispatcher bus(registry, *app);
        std::vector<std::uint8_t> stream;
        std::uint32_t state = seed;
        for (int i = 0; i < 64; ++i) {
            state = state * 1664525U + 1013904223U;
            stream.push_back(static_cast<std::uint8_t>(state >> 24));
        }
        // Ensure every command family is exercised even when a seeded stream
        // happens to miss it. Vary the high bits to select existing or bad IDs.
        for (int op = 0; op < 19; ++op) stream.push_back(static_cast<std::uint8_t>(op));
        auto result = test::historySequence(*app, bus, stream);
        INFO("seed: " << seed);
        INFO((result ? std::string() : result.error().toString()));
        CHECK(result.ok());
    }
}

TEST_CASE("every engine command example runs against its fixture") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    int n = 0;
    auto st = cmd::runExamples(registry, [&](std::string_view) -> std::unique_ptr<cmd::Host> {
        return makeApp(root.path / std::to_string(n++));
    });
    INFO((st ? std::string() : st.error().toString()));
    CHECK(st.ok());
    CHECK(n >= 25);
}

TEST_CASE("an LLM builds a song over the bus: batch, render, undo, save and reopen") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        return d.execute({std::move(command), j(args), kTest});
    };
    auto song = call("batch", R"({
      "label": "bass part",
      "steps": [
        {"command": "track.add", "args": {"name": "Bass"}, "as": "bass"},
        {"command": "fx.add", "args": {"track": "${bass.track}", "plugin": "Test Sine"}},
        {"command": "fx.add", "args": {"track": "${bass.track}", "plugin": "com.suppleaudio.test.gain",
                                        "patch": {"params": {"gain": "-6 dB"}}}, "as": "gain"},
        {"command": "item.add", "args": {"track": "${bass.track}", "bar": 1,
                                          "notation": "voice bass; 1: A2:q A2:q E3:q A2:q | D3:h A2:h"}},
        {"command": "session.describe", "args": {}}
      ]})");
    REQUIRE_MESSAGE(
        song,
        (song ? std::string()
              : song.error().toString() + " / " +
                    (song.error().details.empty() ? "" : song.error().details[0].toString())));
    const std::string described = (*song)["results"].at(4).find("text")->asString();
    CHECK(described.find("tr_1 Bass") != std::string::npos);
    CHECK(described.find("it_1 bar 1–bar 3") != std::string::npos);
    CHECK(app->versions().history(10).size() == 2);  // one batch = one version

    auto mix = call("render.mix", R"({"name": "take1"})");
    REQUIRE_MESSAGE(mix, (mix ? std::string() : mix.error().toString()));
    CHECK((*mix)["seconds"].asDouble() == doctest::Approx(4.0));  // two bars at 120 BPM
    // velocity 96 sine (0.25 * 96/127), centred (-3 dB), gain -6 dB.
    CHECK((*mix)["peak_dbfs"].asDouble() ==
          doctest::Approx(20 * std::log10(0.25 * 96 / 127 * 0.472135955) - 6).epsilon(0.01));
    CHECK(fs::exists((*mix)["path"].asString()));

    auto inlineMix = call("render.mix", R"({"name":"analyzed","analyze":"summary"})");
    REQUIRE(inlineMix);
    CHECK((*inlineMix)["analysis"]["schema"].asString() == "supple.analysis-report/1.0");
    CHECK((*inlineMix)["analysis"]["summary"].isString());
    auto byId = call("analyze.render", R"({"render":"rd_1"})");
    REQUIRE(byId);
    CHECK((*byId)["schema"].asString() == "supple.analysis-report/1.0");
    auto byPath =
        call("analyze.render", json::writeCompact(j(std::string("{\"render\":") +
                                                    json::writeCompact((*mix)["path"]) + "}")));
    REQUIRE(byPath);
    CHECK((*byPath)["loudness"] == (*byId)["loudness"]);
    REQUIRE(call("track.set", R"({"track":"tr_1","volume_db":-6})"));
    auto trackPost = call("analyze.track", R"({"track":"tr_1","tap":"post_fader"})");
    auto trackPre = call("analyze.track", R"({"track":"tr_1","tap":"pre_fader"})");
    REQUIRE(trackPost);
    REQUIRE(trackPre);
    CHECK((*trackPost)["schema"].asString() == "supple.analysis-report/1.0");
    CHECK((*trackPost)["loudness"]["integrated_lufs"].asDouble() <
          (*trackPre)["loudness"]["integrated_lufs"].asDouble() - 5.5);
    auto compared = call("analyze.compare", R"({"reference":"rd_1","test":"rd_1"})");
    REQUIRE(compared);
    CHECK((*compared)["notable"].size() == 0);
    CHECK((*compared)["ltas_rms_db"].asDouble() == doctest::Approx(0));
    auto masked = call("analyze.masking", R"({"masker_track":"tr_1","maskee_track":"tr_1"})");
    REQUIRE(masked);
    CHECK(masked->isObject());
    auto unknown = call("analyze.render", R"({"render":"rd_999"})");
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "engine.unknown_render");

    // An invalid step rolls back the whole batch.
    auto bad = call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Keys"}},
        {"command": "fx.param.set", "args": {"fx": "fx_2", "values": {"gian": 1}}}]})");
    REQUIRE(!bad);
    CHECK(bad.error().details[0].path == "/steps/1/args/values/gian");
    CHECK(app->head().tracks.size() == 2);

    // Parameter edits through the model, undo/redo.
    REQUIRE(call("fx.param.set", R"({"fx": "fx_2", "values": {"gain": -12}})"));
    CHECK(app->head()
              .fx.find(*model::Id::parse("fx_2"))
              ->patch->find("params")
              ->find("gain")
              ->asDouble() == -12.0);
    REQUIRE(call("history.undo", "{}"));
    CHECK(app->head()
              .fx.find(*model::Id::parse("fx_2"))
              ->patch->find("params")
              ->find("gain")
              ->asDouble() == -6.0);
    REQUIRE(call("history.redo", "{}"));

    auto notes = call("midi.get", R"({"item": "it_1"})");
    REQUIRE(notes);
    CHECK((*notes)["text"].asString().find("A2") != std::string::npos);

    REQUIRE(call("project.save", R"({"path": "song.json"})"));
    auto app2 = makeApp(root.path);
    cmd::Dispatcher d2(registry, *app2);
    REQUIRE(d2.execute({"project.open", j(R"({"path": "song.json"})"), kTest}));
    CHECK(model::serializeProject(app2->head()) == model::serializeProject(app->head()));
    auto errs = d2.execute({"track.set", j(R"({"track": "tr_9", "mute": true})"), kTest});
    REQUIRE(!errs);
    CHECK(errs.error().path == "/args/track");
    CHECK(errs.error().hint.find("tr_1") != std::string::npos);
}

TEST_CASE("track analysis excludes unrelated tracks and validates ranges") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string name, std::string_view args) {
        return d.execute({std::move(name), j(args), kTest});
    };
    REQUIRE(call("track.add", R"({"name":"A"})"));
    REQUIRE(call("fx.add", R"({"track":"tr_1","plugin":"Test Sine"})"));
    REQUIRE(
        call("item.add", R"({"track":"tr_1","bar":1,"notation":"voice a; 1: A2:q r:q A2:q r:q"})"));
    auto before = call("analyze.track", R"({"track":"tr_1"})");
    REQUIRE(before);
    REQUIRE(call("track.add", R"({"name":"B"})"));
    REQUIRE(call("fx.add", R"({"track":"tr_2","plugin":"Test Sine"})"));
    REQUIRE(
        call("item.add", R"({"track":"tr_2","bar":1,"notation":"voice b; 1: C4:q r:q C4:q r:q"})"));
    auto after = call("analyze.track", R"({"track":"tr_1"})");
    REQUIRE(after);
    CHECK((*before)["loudness"] == (*after)["loudness"]);
    auto masking = call("analyze.masking", R"({"masker_track":"tr_1","maskee_track":"tr_2"})");
    REQUIRE(masking);
    CHECK(masking->isObject());
    auto empty = call("analyze.track", R"({"track":"tr_1","start_beat":2,"end_beat":1})");
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == "engine.range");
}

TEST_CASE("agent branches: parallel edits merge by replay; conflicts are reported") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        return r;
    };
    REQUIRE(call("track.add", R"({"name": "Drums"})"));
    REQUIRE(call("branch.create", R"({"name": "agent/claude/bass"})"));
    // On the branch: a bass track (tr_2 here) and a level change on it.
    REQUIRE(call("track.add", R"({"name": "Bass"})"));
    REQUIRE(call("track.set", R"({"track": "tr_2", "volume_db": -4})"));
    // Meanwhile on main: a keys track (also tr_2 there) and a drums tweak.
    REQUIRE(call("branch.switch", R"({"name": "main"})"));
    REQUIRE(call("track.add", R"({"name": "Keys"})"));
    REQUIRE(call("track.set", R"({"track": "tr_1", "mute": true})"));
    auto diff = call("branch.diff", R"({"a": "main", "b": "agent/claude/bass"})");
    REQUIRE(diff);
    CHECK((*diff)["changes"].size() >= 2);

    auto merged = call("branch.merge", R"({"from": "agent/claude/bass"})");
    REQUIRE_MESSAGE(merged, (merged ? std::string() : merged.error().toString()));
    CHECK_FALSE((*merged)["fast_forward"].asBool());
    CHECK((*merged)["remapped"]["tr_2"].asString() == "tr_3");
    const auto& s = app->head();
    CHECK(s.tracks.find(*model::Id::parse("tr_2"))->name == "Keys");
    CHECK(s.tracks.find(*model::Id::parse("tr_3"))->name == "Bass");
    CHECK(s.tracks.find(*model::Id::parse("tr_3"))->volumeDb == -4.0);  // followed the remap
    CHECK(s.tracks.find(*model::Id::parse("tr_1"))->mute);

    // A conflict: both sides set the drums level.
    REQUIRE(call("branch.create", R"({"name": "agent/claude/drums"})"));
    REQUIRE(call("track.set", R"({"track": "tr_1", "volume_db": -2})"));
    REQUIRE(call("branch.switch", R"({"name": "main"})"));
    REQUIRE(call("track.set", R"({"track": "tr_1", "volume_db": -6})"));
    const auto before = app->versions().headVersion("main");
    auto conflict = call("branch.merge", R"({"from": "agent/claude/drums"})");
    REQUIRE(!conflict);
    CHECK(conflict.error().code == "branch.conflict");
    REQUIRE(conflict.error().details.size() == 1);
    CHECK(conflict.error().details[0].code == "merge.concurrent_edit");
    CHECK(conflict.error().details[0].message.find("volume_db") != std::string::npos);
    CHECK(app->versions().headVersion("main") == before);

    // Only ID-typed arguments follow a remap: a track *named* "tr_4" keeps its name.
    REQUIRE(call("branch.create", R"({"name": "agent/claude/names"})"));
    REQUIRE(call("track.add", R"({"name": "Pad"})"));  // tr_4 on the branch
    REQUIRE(call("track.set", R"({"track": "tr_4", "name": "tr_4", "notes": "copy of tr_1"})"));
    REQUIRE(call("branch.switch", R"({"name": "main"})"));
    REQUIRE(call("track.add", R"({"name": "Lead"})"));  // tr_4 on main
    auto names = call("branch.merge", R"({"from": "agent/claude/names"})");
    REQUIRE_MESSAGE(names, (names ? std::string() : names.error().toString()));
    CHECK((*names)["remapped"]["tr_4"].asString() == "tr_5");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_4"))->name == "Lead");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_5"))->name == "tr_4");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_5"))->notes == "copy of tr_1");
}

TEST_CASE("an LLM plays the song in real time over the bus (a stand-in libjack)") {
    FakeJack& fake = fakeJack();
    fake.reset(1024);
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    AppOptions o;
    o.workdir = root.path;
    o.pluginPath = {kFixture};
    o.jackLibrary = FakeJack::kPath;
    auto created = EngineApp::create(o, {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Bass"}, "as": "bass"},
        {"command": "fx.add", "args": {"track": "${bass.track}", "plugin": "Test Sine"}},
        {"command": "item.add", "args": {"track": "${bass.track}", "bar": 1,
                                          "notation": "voice bass; 1: A2:w | D3:w"}}]})");

    auto status = call("engine.start", "{}");  // jack by default
    CHECK(status["backend"].asString() == "jack");
    CHECK(status["period"].asInt() == 1024);
    CHECK(status["device_latency"].asInt() == 512);
    CHECK(call("transport.play", "{}")["playing"].asBool());
    for (int i = 0; i < 5000 && fake.outputPeak("") <= 0.01f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(fake.outputPeak("") > 0.01f);
    CHECK(call("transport.get", "{}")["sample"].asInt() > 0);

    call("track.set", R"({"track": "tr_1", "volume_db": -6})");  // a new plan while playing
    fake.xrun();
    status = call("engine.status", "{}");
    CHECK(status["xruns"].asInt() == 1);
    CHECK(status["process_errors"].asInt() == 0);
    CHECK_FALSE(status["restart_needed"].asBool());

    auto t = call("transport.stop", "{}");  // back to where play started
    CHECK_FALSE(t["playing"].asBool());
    CHECK(t["sample"].asInt() == 0);
    call("engine.stop", "{}");
    CHECK_FALSE(call("engine.status", "{}")["running"].asBool());
    CHECK(fake.open() == 0);

    // A device at another rate than the project: the engine runs at the device's.
    fake.setRate(44100);
    auto other = call("engine.start", "{}");
    CHECK(other["sample_rate"].asDouble() == 44100.0);
    call("engine.stop", "{}");
    CHECK(fake.open() == 0);
    fake.reset();
}

TEST_CASE("an isolated plugin that crashes is reported and restarted over the bus") {
    host::setBridgeExecutable(BRIDGE_PATH);
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Lead"}, "as": "t"},
        {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "Test Sine"}},
        {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "com.suppleaudio.test.crash"}, "as": "c"},
        {"command": "fx.set", "args": {"fx": "${c.fx}", "isolation": "dedicated"}},
        {"command": "item.add", "args": {"track": "${t.track}", "bar": 1, "notation": "voice lead; 1: C4:w"}}]})");
    const model::Id crashFx = *model::Id::parse("fx_2");
    call("engine.start", R"({"backend": "dummy"})");
    call("transport.play", "{}");
    host::Instance* crash = app->instanceOf(crashFx);
    REQUIRE(crash);
    CHECK(crash->isolated());
    REQUIRE(crash->setParam(1, 1.0).ok());  // crash in its next process()
    for (int i = 0; i < 2000 && !crash->failed(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    auto status = call("engine.status", "{}");
    REQUIRE(status["failed_fx"].size() == 1);
    CHECK(status["failed_fx"].at(0).asString() == "fx_2");
    CHECK(status["running"].asBool());  // the engine plays on
    bool reported = false;
    for (const auto& [topic, payload] : app->takeEvents())
        if (topic == "fx.failed") reported = payload.find("fx")->asString() == "fx_2";
    CHECK(reported);
    call("fx.restart", R"({"fx": "fx_2"})");
    CHECK(call("engine.status", "{}")["failed_fx"].size() == 0);
    host::Instance* again = app->instanceOf(crashFx);
    REQUIRE(again);
    CHECK(again != crash);
    CHECK_FALSE(again->failed());
    call("engine.stop", "{}");
}

TEST_CASE("shared bridge crash reports both fx and both restart in one new process") {
    host::setBridgeExecutable(BRIDGE_PATH);
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher dispatcher(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto result = dispatcher.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
        return *result;
    };
    call("batch", R"({"steps": [
        {"command":"track.add","args":{"name":"Shared"},"as":"t"},
        {"command":"fx.add","args":{"track":"${t.track}","plugin":"Test Sine"}},
        {"command":"fx.add","args":{"track":"${t.track}","plugin":"com.suppleaudio.test.gain"},"as":"a"},
        {"command":"fx.set","args":{"fx":"${a.fx}","isolation":"shared_bridge"}},
        {"command":"fx.add","args":{"track":"${t.track}","plugin":"com.suppleaudio.test.crash"},"as":"b"},
        {"command":"fx.set","args":{"fx":"${b.fx}","isolation":"shared_bridge"}},
        {"command":"fx.add","args":{"track":"${t.track}","plugin":"com.suppleaudio.test.gain"},"as":"d"},
        {"command":"fx.set","args":{"fx":"${d.fx}","isolation":"dedicated"}},
        {"command":"item.add","args":{"track":"${t.track}","bar":1,"notation":"voice lead; 1: C4:w"}}]})");
    const auto firstId = *model::Id::parse("fx_2");
    const auto crashId = *model::Id::parse("fx_3");
    const auto dedicatedId = *model::Id::parse("fx_4");
    call("engine.start", R"({"backend":"dummy"})");
    call("transport.play", "{}");
    auto* first = app->instanceOf(firstId);
    auto* crash = app->instanceOf(crashId);
    auto* dedicated = app->instanceOf(dedicatedId);
    REQUIRE(first);
    REQUIRE(crash);
    REQUIRE(dedicated);
    const int oldPid = first->bridgePid();
    CHECK(oldPid > 0);
    CHECK(crash->bridgePid() == oldPid);
    CHECK(dedicated->bridgePid() != oldPid);
    REQUIRE(crash->setParam(1, 1.0));
    for (int i = 0; i < 2000 && !crash->failed(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(crash->failed());
    CHECK(first->failed());
    CHECK_FALSE(dedicated->failed());
    auto status = call("engine.status", "{}");
    CHECK(status["running"].asBool());
    REQUIRE(status["failed_fx"].size() == 2);
    std::set<std::string> reported;
    for (const auto& [topic, payload] : app->takeEvents())
        if (topic == "fx.failed") reported.insert(payload.find("fx")->asString());
    CHECK((reported == std::set<std::string>{"fx_2", "fx_3"}));
    call("fx.restart", R"({"fx":"fx_2"})");
    call("fx.restart", R"({"fx":"fx_3"})");
    first = app->instanceOf(firstId);
    crash = app->instanceOf(crashId);
    REQUIRE(first);
    REQUIRE(crash);
    CHECK_FALSE(first->failed());
    CHECK_FALSE(crash->failed());
    CHECK(first->bridgePid() > 0);
    CHECK(first->bridgePid() == crash->bridgePid());
    CHECK(first->bridgePid() != oldPid);
    CHECK(dedicated->bridgePid() != first->bridgePid());
    CHECK(call("engine.status", "{}")["failed_fx"].size() == 0);
    call("engine.stop", "{}");
}

TEST_CASE("an LLM automates a plugin parameter over the bus and hears it in the render") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        return d.execute({std::move(command), j(args), kTest});
    };
    auto made = call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Pad"}, "as": "t"},
        {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "Test Sine"}},
        {"command": "fx.add", "args": {"track": "${t.track}", "plugin": "com.suppleaudio.test.gain"}, "as": "g"},
        {"command": "item.add", "args": {"track": "${t.track}", "bar": 1, "notation": "voice pad; 1: C4:w"}},
        {"command": "env.add", "args": {"fx": "${g.fx}", "param": "gain",
            "points": [{"beat": 0, "value": "0 dB", "shape": "square"}, {"beat": 2, "value": "-60 dB", "shape": "square"}]}}]})");
    REQUIRE_MESSAGE(made, (made ? std::string() : made.error().toString()));
    auto mix = call("render.mix", R"({"name": "automated", "end_beat": 4})");
    REQUIRE_MESSAGE(mix, (mix ? std::string() : mix.error().toString()));
    auto audio = audio::readAudio((*mix)["path"].asString());
    REQUIRE(audio);
    const auto& l = audio->channels[0];
    double before = 0, after = 0;
    for (std::size_t i = 40000; i < 48000; ++i) before = std::max(before, std::fabs(double(l[i])));
    for (std::size_t i = 48000; i < 90000; ++i) after = std::max(after, std::fabs(double(l[i])));
    CHECK(before > 0.05);
    CHECK(after == 0.0);  // silent from beat 2 (sample 48000), off at min

    auto listed = call("env.list", "{}");
    REQUIRE(listed);
    CHECK((*listed)["envelopes"].size() == 1);
    CHECK((*listed)["envelopes"].at(0).find("visible")->asBool());
    // Hiding the lane is UI state in the project; the envelope still plays.
    REQUIRE(call("env.set", R"({"env": "en_1", "visible": false})"));
    CHECK_FALSE(app->head().envelopes.find(*model::Id::parse("en_1"))->visible);
    auto hidden = call("env.list", "{}");
    REQUIRE(hidden);
    CHECK_FALSE((*hidden)["envelopes"].at(0).find("visible")->asBool());
    REQUIRE(call("env.set", R"({"env": "en_1", "visible": true})"));
    REQUIRE(call("history.undo", "{}"));  // both visibility edits, so the undo below
    REQUIRE(call("history.undo", "{}"));  // still takes the batch
    // Errors say what to do.
    auto twice = call("env.add", R"({"fx": "fx_2", "param": "gain"})");
    REQUIRE_FALSE(twice);
    CHECK(twice.error().code == "env.exists");
    auto typo = call("env.add", R"({"fx": "fx_2", "param": "gian"})");
    REQUIRE_FALSE(typo);
    CHECK(typo.error().code == "env.param");
    CHECK(typo.error().hint.find("gain") != std::string::npos);
    auto both = call("env.add", R"({"fx": "fx_2", "track": "tr_1", "param": "gain"})");
    REQUIRE_FALSE(both);
    CHECK(both.error().code == "env.target");
    auto badValue =
        call("env.points.set", R"({"env": "en_1", "points": [{"beat": 0, "value": "loud"}]})");
    REQUIRE_FALSE(badValue);
    CHECK(badValue.error().path == "/args/points/0/value");
    // Undo removes the envelope with the batch.
    REQUIRE(call("history.undo", "{}"));
    CHECK(app->head().envelopes.size() == 0);
}

TEST_CASE("an LLM records a take over the bus, hears it back and undoes it") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Vocal"}, "as": "v"},
        {"command": "track.set", "args": {"track": "${v.track}", "armed": true, "record_input": [1, 2]}}]})");
    // A device whose inputs carry a 0.5 sine at 441 Hz.
    DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                          .input = [](float* const* in, std::uint32_t channels, std::uint32_t n,
                                      std::int64_t frame) {
                              for (std::uint32_t c = 0; c < channels; ++c)
                                  for (std::uint32_t i = 0; i < n; ++i)
                                      in[c][i] =
                                          0.5f * static_cast<float>(std::sin(
                                                     2.0 * 3.14159265358979 * 441.0 *
                                                     static_cast<double>(frame + i) / 48000.0));
                          }});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2},
                             {.anticipative = false})
                .ok());
    auto started = call("transport.record", "{}");
    CHECK(started["recording"].asBool());
    backend.run(1);  // apply the queued transport command at the callback
    CHECK(call("transport.get", "{}")["playing"].asBool());
    backend.run(74);
    auto stopped = call("transport.stop", "{}");
    CHECK_FALSE(stopped["recording"].asBool());
    REQUIRE(stopped["recorded"].size() == 1);
    const json::Value& take = stopped["recorded"].at(0);
    CHECK(take.find("track")->asString() == "tr_1");
    CHECK(take.find("seconds")->asDouble() == doctest::Approx(0.4));
    CHECK(take.find("lost_frames")->asInt() == 0);
    CHECK(fs::exists(take.find("file")->asString()));
    CHECK(app->head().items.size() == 1);
    // The committed source carries the recording's hash: the recorder took the
    // bytes when it closed the take, so the commit read nothing back.
    REQUIRE(app->head().media.size() == 1);
    CHECK(app->head().media.begin()->value->hash ==
          sha256File(fs::path(take.find("file")->asString())).value());
    app->stopEngine();

    // The take plays back in a render: the sine, centred (-3 dB).
    auto mix = call("render.mix", R"({"name": "take", "end_beat": 0.5})");
    auto audio = audio::readAudio(mix["path"].asString());
    REQUIRE(audio);
    double peak = 0;
    for (float x : audio->channels[0]) peak = std::max(peak, std::fabs(double(x)));
    CHECK(peak == doctest::Approx(0.5 * 0.472135955).epsilon(0.01));

    // One version: undo removes the take.
    call("history.undo", "{}");
    CHECK(app->head().items.size() == 0);
}

TEST_CASE("recordings a crash left out are listed and recovered in place, whole") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    // Record a take for real (the Recorder writes the sidecar), then lose it
    // the way a crash before saving would: undo, save, reopen.
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Vocal"}, "as": "v"},
        {"command": "track.set", "args": {"track": "${v.track}", "armed": true, "record_input": [1]}}]})");
    call("project.save", R"({"path": "song.json"})");
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
             for (std::uint32_t c = 0; c < channels; ++c)
                 for (std::uint32_t i = 0; i < n; ++i) in[c][i] = 0.5f;
         }});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2},
                             {.anticipative = false})
                .ok());
    call("transport.seek", R"({"seconds": 0.5})");
    call("transport.record", "{}");
    // While it records, its file is not offered.
    backend.run(19);
    CHECK(call("project.recoverable", "{}")["recordings"].size() == 0);
    backend.run(38);
    auto stopped = call("transport.stop", "{}");
    app->stopEngine();
    REQUIRE(stopped["recorded"].size() == 1);
    const json::Value take = stopped["recorded"].at(0);
    const model::Session recorded = app->head();
    REQUIRE(recorded.items.size() == 1);
    const model::Item original = *recorded.items.begin()->value;
    CHECK(call("project.get", "{}")["recoverable"].asInt() == 0);
    call("history.undo", "{}");
    CHECK(call("project.get", "{}")["recoverable"].asInt() == 0);  // redo still has it
    call("project.save", "{}");
    auto opened = call("project.open", R"({"path": "song.json"})");
    CHECK(opened["recoverable"].asInt() == 1);

    auto list = call("project.recoverable", "{}");
    REQUIRE(list["recordings"].size() == 1);
    const json::Value& rec = list["recordings"].at(0);
    CHECK(rec.find("file")->asString() == "rec/Vocal-1.wav");
    CHECK(rec.find("track")->asString() == "tr_1");
    CHECK(rec.find("channels")->asInt() == 1);
    CHECK(rec.find("seconds")->asDouble() == doctest::Approx(take.find("seconds")->asDouble()));

    auto bad = d.execute({"project.recover", j(R"({"files": ["rec/Other-1.wav"]})"), kTest});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "recover.unknown_file");
    CHECK(bad.error().path == "/args/files/0");

    auto recovered = call("project.recover", R"({"files": ["rec/Vocal-1.wav"]})");
    REQUIRE(recovered["recovered"].size() == 1);
    const model::Session s = app->head();
    REQUIRE(s.items.size() == 1);
    const model::Item& back = *s.items.begin()->value;
    CHECK(back.track == original.track);
    CHECK(back.position == doctest::Approx(original.position));
    CHECK(back.length == doctest::Approx(original.length));
    CHECK(call("project.recoverable", "{}")["recordings"].size() == 0);
    auto nothing = d.execute({"project.recover", j("{}"), kTest});
    REQUIRE_FALSE(nothing);
    CHECK(nothing.error().code == "recover.nothing");
    // One undo step.
    call("history.undo", "{}");
    CHECK(app->head().items.size() == 0);
}

TEST_CASE("recovering a crashed recording repairs its header and plays all of it") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "recording_interrupted");  // tr_1 + rec/Vocal-1.wav: 1 s at 1 s
    const fs::path file = app->recordDir() / "Vocal-1.wav";
    // As the crash left it, a reader sees only up to the last header rewrite.
    auto before = audio::readAudio(file);
    REQUIRE(before);
    CHECK(before->frames() == 24000);

    // The track it was recorded on is gone: it must be told where to go.
    REQUIRE(d.execute({"track.remove", j(R"({"track": "tr_1"})"), kTest}));
    REQUIRE(d.execute({"track.add", j(R"({"name": "Vocal 2"})"), kTest}));
    auto gone = d.execute({"project.recover", j("{}"), kTest});
    REQUIRE_FALSE(gone);
    CHECK(gone.error().code == "recover.no_track");
    auto r = d.execute({"project.recover", j(R"({"track": "tr_2"})"), kTest});
    REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
    CHECK((*r)["recovered"].at(0).find("seconds")->asDouble() == doctest::Approx(1.0));
    auto after = audio::readAudio(file);
    REQUIRE(after);
    CHECK(after->frames() == 48000);

    // It plays from 1 s to 2 s (120 BPM: beats 2 to 4), at 0.25 panned centre.
    auto mix = d.execute({"render.mix", j(R"({"name": "recovered", "end_beat": 5})"), kTest});
    REQUIRE(mix);
    auto audio = audio::readAudio((*mix)["path"].asString());
    REQUIRE(audio);
    const auto& left = audio->channels[0];
    REQUIRE(left.size() >= 110000);
    CHECK(double(left[47000]) == 0.0);
    CHECK(double(left[49000]) == doctest::Approx(0.25 * 0.472135955).epsilon(0.01));
    CHECK(double(left[95000]) == doctest::Approx(0.25 * 0.472135955).epsilon(0.01));
    CHECK(double(left[97000]) == 0.0);
}

TEST_CASE("an unsaved project records into its scratch folder; the first save moves the files") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto open = [&](const fs::path& scratch) {
        AppOptions o;
        o.workdir = root.path / "work";
        o.scratchDir = scratch;
        o.chainDir = root.path / "chains";
        o.templateDir = root.path / "templates";
        o.pluginPath = {kFixture};
        auto app = EngineApp::create(o, {.console9 = false});
        REQUIRE(app);
        return std::move(*app);
    };
    // Saved: the recording moves beside the project, under the same relative path.
    {
        const fs::path scratch = root.path / "unsaved-1";
        auto app = open(scratch);
        cmd::Dispatcher d(registry, *app);
        app->setupFixture(d, "recording_interrupted");  // rec/Vocal-1.wav in the scratch media
        CHECK(app->recordDir() == scratch / "media" / "rec");
        REQUIRE(d.execute({"project.recover", j("{}"), kTest}));
        const auto& media = app->head().media;
        REQUIRE(media.size() == 1);
        CHECK(media.begin()->value->path == "rec/Vocal-1.wav");  // relative to the media folder
        REQUIRE(app->save(root.path / "song" / "song.json"));
        CHECK(fs::exists(root.path / "song" / "media" / "rec" / "Vocal-1.wav"));
        CHECK_FALSE(fs::exists(scratch));
        auto mix = d.execute({"render.mix", j(R"({"name": "after-save", "end_beat": 5})"), kTest});
        REQUIRE(mix);
        CHECK((*mix)["peak_dbfs"].asDouble() > -20.0);  // the moved file still plays
        app->clearScratch();  // quitting a saved project deletes nothing of it
        CHECK(fs::exists(root.path / "song" / "media" / "rec" / "Vocal-1.wav"));
    }
    // Discarded: quitting, or starting a new project, deletes the recordings.
    {
        const fs::path scratch = root.path / "unsaved-2";
        auto app = open(scratch);
        cmd::Dispatcher d(registry, *app);
        app->setupFixture(d, "recording_interrupted");
        REQUIRE(fs::exists(scratch / "media" / "rec" / "Vocal-1.wav"));
        REQUIRE(app->newProject(model::Project{}));
        CHECK_FALSE(fs::exists(scratch));
        app->setupFixture(d, "recording_interrupted");
        REQUIRE(fs::exists(scratch / "media" / "rec" / "Vocal-1.wav"));
        app->clearScratch();
        CHECK_FALSE(fs::exists(scratch));
        CHECK(fs::exists(root.path / "work"));
    }
}

TEST_CASE("two armed tracks with one name record to two files (review round 8)") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Vocal"}, "as": "a"},
        {"command": "track.add", "args": {"name": "Vocal"}, "as": "b"},
        {"command": "track.set", "args": {"track": "${a.track}", "armed": true, "record_input": [1]}},
        {"command": "track.set", "args": {"track": "${b.track}", "armed": true, "record_input": [2]}}]})");
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
             for (std::uint32_t c = 0; c < channels; ++c)
                 for (std::uint32_t i = 0; i < n; ++i) in[c][i] = c ? -0.5f : 0.25f;
         }});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2},
                             {.anticipative = false})
                .ok());
    call("transport.record", "{}");
    backend.run(38);
    auto stopped = call("transport.stop", "{}");
    app->stopEngine();
    REQUIRE(stopped["recorded"].size() == 2);
    const std::string f0 = stopped["recorded"].at(0).find("file")->asString();
    const std::string f1 = stopped["recorded"].at(1).find("file")->asString();
    CHECK(f0 != f1);
    for (const auto& [file, value] : {std::pair{f0, 0.25f}, std::pair{f1, -0.5f}}) {
        auto audio = audio::readAudio(file);
        REQUIRE(audio);
        REQUIRE(audio->frames() > 1000);
        CHECK(audio->channels[0][500] == value);  // each file holds its own input
        CHECK(fs::exists(fs::path(file).replace_extension(".rec.json")));
    }
}

TEST_CASE("recovery touches only files in rec/, once each (review round 8)") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "recording_interrupted");  // rec/Vocal-1.wav, recoverable
    const fs::path rec = app->recordDir();
    // An eligible WAV outside the project, linked into rec/ with a sidecar.
    const fs::path outside = root.path / "elsewhere.wav";
    fs::copy_file(rec / "Vocal-1.wav", outside);
    fs::copy_file(rec / "Vocal-1.rec.json", rec / "Link-1.rec.json");
    fs::create_symlink(outside, rec / "Link-1.wav");
    auto bytes = [](const fs::path& p) {
        std::ifstream f(p, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), {});
    };
    const std::string before = bytes(outside);

    auto list = d.execute({"project.recoverable", j("{}"), kTest});
    REQUIRE(list);
    REQUIRE((*list)["recordings"].size() == 1);
    CHECK((*list)["recordings"].at(0).find("file")->asString() == "rec/Vocal-1.wav");
    auto link = d.execute({"project.recover", j(R"({"files": ["rec/Link-1.wav"]})"), kTest});
    REQUIRE_FALSE(link);
    CHECK(link.error().code == "recover.unknown_file");
    auto twice = d.execute(
        {"project.recover", j(R"({"files": ["rec/Vocal-1.wav", "rec/Vocal-1.wav"]})"), kTest});
    REQUIRE_FALSE(twice);
    CHECK(twice.error().code == "recover.duplicate_file");
    CHECK(app->head().items.size() == 0);
    auto all = d.execute({"project.recover", j("{}"), kTest});
    REQUIRE(all);
    CHECK((*all)["recovered"].size() == 1);
    CHECK(bytes(outside) == before);  // never rewritten through the link
}

TEST_CASE("an LLM loop-records over the bus: one item, a take per pass, punch trims it") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Vocal"}, "as": "v"},
        {"command": "track.set", "args": {"track": "${v.track}", "armed": true, "record_input": [1]}}]})");
    // Loop beats 1..2 (0.5 s .. 1 s at 120 BPM), set before the engine runs.
    auto loop = call("transport.loop", R"({"start_beat": 1, "end_beat": 2})");
    CHECK(loop["enabled"].asBool());
    CHECK(loop["start_seconds"].asDouble() == 0.5);
    auto bad = d.execute({"transport.loop", j(R"({"start_beat": 2, "end_beat": 1})"), kTest});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "transport.range");

    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
             for (std::uint32_t c = 0; c < channels; ++c)
                 for (std::uint32_t i = 0; i < n; ++i) in[c][i] = 0.25f;
         }});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2},
                             {.anticipative = false})
                .ok());
    call("transport.seek", R"({"seconds": 0.5})");
    auto got = call("transport.get", "{}");
    CHECK(got["loop"].find("enabled")->asBool());
    call("transport.record", "{}");
    backend.run(244);  // two whole passes and a bit
    auto stopped = call("transport.stop", "{}");
    REQUIRE(stopped["recorded"].size() == 1);
    const json::Value& rec = stopped["recorded"].at(0);
    const std::size_t passes = rec.find("takes")->size();
    CHECK(passes == 3);  // two whole passes and one partial
    CHECK(rec.find("seconds")->asDouble() == doctest::Approx(0.5));
    const model::Session s = app->head();
    REQUIRE(s.items.size() == 1);
    const model::Item& item = *s.items.begin()->value;
    CHECK(item.position == doctest::Approx(1.0));
    CHECK(item.length == doctest::Approx(1.0));
    REQUIRE(item.takes.size() == passes);
    for (std::size_t k = 0; k < passes; ++k)  // pass k starts k * 0.5 s into the file
        CHECK(s.takes.find(item.takes[k])->startOffset == doctest::Approx(0.5 * double(k)));
    // The newest whole pass is active: the last, or the one before it when
    // the stop cut the last one short.
    const std::string active = rec.find("take")->asString();
    CHECK((active == rec.find("takes")->at(passes - 1).asString() ||
           active == rec.find("takes")->at(passes - 2).asString()));
    CHECK(item.activeTake->str() == active);

    // With a punch range, the same kind of recording keeps 0.6 s .. 0.8 s.
    call("history.undo", "{}");
    call("transport.punch", R"({"start_seconds": 0.6, "end_seconds": 0.8})");
    call("transport.seek", R"({"seconds": 0.5})");
    call("transport.record", "{}");
    backend.run(150);
    auto punched = call("transport.stop", "{}");
    app->stopEngine();
    REQUIRE(punched["recorded"].size() == 1);
    CHECK(punched["recorded"].at(0).find("seconds")->asDouble() == doctest::Approx(0.2));
    const model::Session p = app->head();
    REQUIRE(p.items.size() == 1);
    const model::Item& pi = *p.items.begin()->value;
    CHECK(pi.position == doctest::Approx(1.2));  // 0.6 s
    const model::Take* first = p.takes.find(pi.takes[0]);
    CHECK(first->startOffset == doctest::Approx(0.1));  // 0.1 s into the first pass
}

TEST_CASE("an LLM records a MIDI performance over the bus and reads it back as notes") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Keys"}, "as": "k"},
        {"command": "fx.add", "args": {"track": "${k.track}", "plugin": "com.suppleaudio.test.sine"}},
        {"command": "track.set", "args": {"track": "${k.track}", "armed": true, "record_midi": "*", "midi_channel": 1}}]})");
    const model::Session armed = app->head();
    const model::Route* route =
        armed.routes.find(*armed.tracks.find(model::Id{model::Kind::Track, 1})->record.input);
    REQUIRE(route);
    CHECK(route->from.midiDevice == std::optional<std::string>("*"));
    CHECK(route->midi->fromChannel == 1);
    // A keyboard: every 24000 device frames (0.5 s) a note on, off 12000 later.
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .midiPorts = {"USB Keyboard"},
         .midi = [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
             for (std::int64_t f = frame; f < frame + n; ++f) {
                 if (f % 12000 != 0) continue;
                 MidiInputEvent m;
                 m.frame = static_cast<std::uint32_t>(f - frame);
                 m.size = 3;
                 const bool on = f % 24000 == 0;
                 m.bytes[0] = on ? 0x90 : 0x80;
                 m.bytes[1] = 60;
                 m.bytes[2] = on ? 100 : 0;
                 out.push_back(m);
             }
         }});
    REQUIRE(
        app->startEngine(backend,
                         {.sampleRate = 48000, .blockSize = 256, .inputs = 2, .midiInputs = true},
                         {.anticipative = false})
            .ok());
    call("transport.record", "{}");
    backend.run(263);
    auto stopped = call("transport.stop", "{}");
    app->stopEngine();
    REQUIRE(stopped["recorded"].size() == 1);
    const std::string itemId = stopped["recorded"].at(0).find("item")->asString();
    const model::Session s = app->head();
    const model::Item& item = *s.items.find(*model::Id::parse(itemId));
    const model::Take& take = *s.takes.find(*item.activeTake);
    REQUIRE(take.source);
    const midi::Sequence& seq = s.midi.find(*take.source)->sequence;
    REQUIRE(seq.notes.size() >= 2);
    for (std::size_t i = 0; i < seq.notes.size(); ++i) {
        CHECK(seq.notes[i].pitch == 60);
        CHECK(seq.notes[i].velocity == 100);
        if (i + 1 < seq.notes.size()) {
            CHECK(seq.notes[i].length == 480);                          // half a beat
            CHECK(seq.notes[i + 1].start - seq.notes[i].start == 960);  // one beat apart
        }
    }
    // Read back as notation.
    auto text = call("midi.get", std::string(R"({"item": ")") + itemId + R"("})");
    CHECK(text["text"].asString().find("C4") != std::string::npos);
}

TEST_CASE("a batch keeps text positions of nested errors (not pointer-prefixed)") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto r = d.execute({"batch", j(R"({"steps": [
        {"command": "track.add", "args": {"name": "Bass"}, "as": "b"},
        {"command": "item.add", "args": {"track": "${b.track}", "bar": 1, "notation": "voice v; 1: C4:q | 2: D4:q | 3: E4:q"}}]})"),
                        kTest});
    REQUIRE_FALSE(r);
    REQUIRE(r.error().details.size() == 1);
    const Error& step = r.error().details[0];
    CHECK(step.path == "/steps/1/args/notation");
    REQUIRE_FALSE(step.details.empty());
    CHECK(step.details[0].path.front() != '/');  // "1:16": a position in the notation text
    CHECK(step.details[0].path.find("/steps") == std::string::npos);
}

TEST_CASE("recorded MIDI notes are clipped at punch and overdub item edges") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto result = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
        return *result;
    };
    call("batch", R"({"steps":[
        {"command":"track.add","args":{"name":"Keys"},"as":"k"},
        {"command":"fx.add","args":{"track":"${k.track}","plugin":"com.suppleaudio.test.sine"}}]})");
    const model::Id track{model::Kind::Track, 1};
    RecordedTake r;
    r.track = track;
    r.isMidi = true;
    r.sampleRate = 48000;
    r.frames = 48000;
    r.segments = {{0, 0}};
    r.midi = {{9600, 0, {0x90, 72, 100}}, {38400, 0, {0x80, 72, 0}}};
    app->setPunch({true, 0.4, 0.6});
    REQUIRE(app->commitTakes({r}));
    const auto& punched = app->head();
    REQUIRE(punched.items.size() == 1);
    const model::Item& item = *punched.items.begin()->value;
    const midi::Sequence& seq =
        punched.midi.find(*punched.takes.find(*item.activeTake)->source)->sequence;
    REQUIRE(seq.notes.size() == 1);
    CHECK(seq.notes[0].start == 0);
    CHECK(seq.notes[0].length == 384);  // 0.2 seconds at 120 bpm and 960 PPQ
    auto rendered = app->renderMix({.startSeconds = 0, .endSeconds = 0.8});
    REQUIRE(rendered);
    const auto& audio = rendered->audio.channels[0];
    double during = 0, before = 0, after = 0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        const double v = std::fabs(double(audio[i]));
        if (i < 19200)
            before = std::max(before, v);
        else if (i < 28800)
            during = std::max(during, v);
        else
            after = std::max(after, v);
    }
    CHECK(before == 0.0);
    CHECK(during > 0.1 * (0.472135955 / 0.70710678118654752));
    CHECK(after == 0.0);

    auto overdub = makeApp(root.path / "overdub");
    cmd::Dispatcher overdubBus(registry, *overdub);
    auto setup = overdubBus.execute({"batch", j(R"({"steps":[
        {"command":"track.add","args":{"name":"Keys"},"as":"k"},
        {"command":"item.add","args":{"track":"${k.track}","bar":1,"notation":"voice v; 1: C4:w"}},
        {"command":"track.set","args":{"track":"${k.track}","record_mode":"midi_overdub"}}]})"),
                                     kTest});
    REQUIRE_MESSAGE(setup, (setup ? std::string() : setup.error().toString()));
    RecordedTake edge;
    edge.track = track;
    edge.isMidi = true;
    edge.sampleRate = 48000;
    edge.frames = 144000;
    edge.segments = {{0, 0}};
    edge.midi = {{91200, 0, {0x90, 74, 100}}, {105600, 0, {0x80, 74, 0}}};
    REQUIRE(overdub->commitTakes({edge}));
    const auto& merged = overdub->head();
    REQUIRE(merged.items.size() == 1);
    const model::Item& oldItem = *merged.items.begin()->value;
    const midi::Sequence& notes =
        merged.midi.find(*merged.takes.find(*oldItem.activeTake)->source)->sequence;
    const auto clipped = std::find_if(notes.notes.begin(), notes.notes.end(),
                                      [](const midi::Note& n) { return n.pitch == 74; });
    REQUIRE(clipped != notes.notes.end());
    CHECK(clipped->start == 3648);
    CHECK(clipped->length == 192);
}

TEST_CASE("midi_overdub merges a performance into the item under it, as a new take") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    // Four bars of a bass line on beat 1 of each bar (at 120 BPM a bar is 2 s).
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Keys"}, "as": "k"},
        {"command": "fx.add", "args": {"track": "${k.track}", "plugin": "com.suppleaudio.test.sine"}},
        {"command": "item.add", "args": {"track": "${k.track}", "bar": 1, "notation": "voice v; 1: C3:w | C3:w | C3:w | C3:w"}},
        {"command": "track.set", "args": {"track": "${k.track}", "armed": true, "record_midi": "*", "record_mode": "midi_overdub"}}]})");
    call("take.set", R"({"take":"tk_1","playrate":2})");
    call("fx.add", R"({"take":"tk_1","plugin":"Test Gain"})");
    call("env.add", R"({"fx":"fx_2","param":"gain","points":[{"beat":0,"value":-6}]})");
    const model::Session before = app->head();
    REQUIRE(before.items.size() == 1);
    const model::Id item = before.items.begin()->id;
    const model::Id oldTake = *before.items.begin()->value->activeTake;
    const std::size_t oldNotes =
        before.midi.find(*before.takes.find(oldTake)->source)->sequence.notes.size();
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .midiPorts = {"Keyboard"},
         .midi = [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
             for (std::int64_t f = frame; f < frame + n; ++f) {
                 if (f % 12000 != 0) continue;
                 MidiInputEvent m;
                 m.frame = static_cast<std::uint32_t>(f - frame);
                 m.size = 3;
                 const bool on = f % 24000 == 0;
                 m.bytes[0] = on ? 0x90 : 0x80;
                 m.bytes[1] = 72;
                 m.bytes[2] = on ? 90 : 0;
                 out.push_back(m);
             }
         }});
    REQUIRE(
        app->startEngine(backend,
                         {.sampleRate = 48000, .blockSize = 256, .inputs = 2, .midiInputs = true},
                         {.anticipative = false})
            .ok());
    call("transport.record", "{}");
    backend.run(225);  // exactly 1.2 s at 48 kHz/256, no clock criterion
    auto stopped = call("transport.stop", "{}");
    app->stopEngine();
    REQUIRE(stopped["recorded"].size() == 1);
    CHECK(stopped["recorded"].at(0).find("item")->asString() == item.str());  // no new item
    const model::Session s = app->head();
    CHECK(s.items.size() == 1);
    const model::Item& it = *s.items.find(item);
    REQUIRE(it.takes.size() == 2);
    CHECK(it.takes[0] == oldTake);  // the old take stays
    const midi::Sequence& merged = s.midi.find(*s.takes.find(*it.activeTake)->source)->sequence;
    std::size_t c5 = 0, c3 = 0;
    for (const midi::Note& n : merged.notes) (n.pitch == 72 ? c5 : c3) += 1;
    CHECK(c3 == oldNotes);
    CHECK(c5 >= 2);
    for (const auto& note : merged.notes)
        if (note.pitch == 72 && note.start == 0) CHECK(note.length == 960);
    const auto* fresh = s.takes.find(*it.activeTake);
    REQUIRE(fresh->chain.size() == 1);
    CHECK(fresh->chain[0] != before.takes.find(oldTake)->chain[0]);
    CHECK(s.envelopes.size() == before.envelopes.size() + 1);
    call("history.undo", "{}");
    CHECK(app->head().items.find(item)->takes.size() == 1);
}

TEST_CASE("linear Console 9 stems sum to the master-less mix, and regions match dry run") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_region");
    auto call = [&](std::string name, std::string_view args) {
        auto r = d.execute({std::move(name), j(args), kTest});
        INFO((r ? std::string() : r.error().toString()));
        REQUIRE(r);
        return *r;
    };
    call("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.sine"})");
    call("track.add", R"({"name":"Lead"})");
    call("item.add", R"({"track":"tr_2","bar":1,"notation":"voice lead; 1: E3:q r:q E3:q r:q"})");
    call("fx.add", R"({"track":"tr_2","plugin":"com.suppleaudio.test.sine"})");
    auto mix = call("render.mix", R"({"name":"sum","end_beat":4})");
    auto plannedMix = call("render.dry-run", R"({"operation":"mix","name":"sum","end_beat":4})");
    CHECK(plannedMix.find("files")->at(0).find("path")->asString() == mix.find("path")->asString());
    auto stems = call("render.stems", R"({"end_beat":4,"pattern":"stem-{track_number:02}"})");
    const auto& files = *stems.find("files");
    REQUIRE(files.size() == 2);
    auto whole = audio::readAudio(mix.find("path")->asString());
    auto left = audio::readAudio(files.at(0).find("path")->asString());
    auto right = audio::readAudio(files.at(1).find("path")->asString());
    REQUIRE(whole);
    REQUIRE(left);
    REQUIRE(right);
    REQUIRE(whole->frames() == left->frames());
    REQUIRE(whole->frames() == right->frames());
    for (std::size_t c = 0; c < whole->channelCount(); ++c)
        for (std::size_t i = 0; i < whole->frames(); ++i)
            CHECK(static_cast<double>(whole->channels[c][i]) ==
                  doctest::Approx(static_cast<double>(left->channels[c][i]) +
                                  static_cast<double>(right->channels[c][i]))
                      .epsilon(1e-5));
    auto through = call("render.stems", R"({"tracks":["tr_1"],"mode":"through_master",
        "pattern":"through-{track}"})");
    auto mastered = audio::readAudio(through.find("files")->at(0).find("path")->asString());
    REQUIRE(mastered);
    CHECK(mastered->channels == left->channels);
    auto dry =
        call("render.dry-run", R"({"regions":["rg_1"],"pattern":"region-{region_number:02}"})");
    auto rendered =
        call("render.regions", R"({"regions":["rg_1"],"pattern":"region-{region_number:02}"})");
    CHECK(dry.find("files")->at(0).find("path")->asString() ==
          rendered.find("files")->at(0).find("path")->asString());
    auto regionAudio = audio::readAudio(rendered.find("files")->at(0).find("path")->asString());
    REQUIRE(regionAudio);
    CHECK(regionAudio->frames() == 96000);
    call(
        "project.set",
        R"({"render":{"matrix":{"project":["tr_0"],"rg_1":["tr_1","tr_0"]},"pattern":"matrix-{region}-{track}-{counter:02}"}})");
    auto matrix = call("render.regions", R"({})");
    CHECK(matrix.find("files")->size() == 3);
}

TEST_CASE("branch render uses its own head and leaves the current branch alone") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_item");
    auto call = [&](std::string name, std::string_view args) {
        auto r = d.execute({std::move(name), j(args), kTest});
        INFO((r ? std::string() : r.error().toString()));
        REQUIRE(r);
        return *r;
    };
    call("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.sine"})");
    call("branch.create", R"({"name":"alternate","switch":true})");
    call("track.set", R"({"track":"tr_1","volume_db":-12})");
    call("branch.switch", R"({"name":"main"})");
    auto other = call("render.mix", R"({"branch":"alternate","name":"other","end_beat":4})");
    CHECK(app->versions().branch() == "main");
    call("branch.switch", R"({"name":"alternate"})");
    auto direct = call("render.mix", R"({"name":"direct","end_beat":4})");
    auto a = audio::readAudio(other.find("path")->asString());
    auto b = audio::readAudio(direct.find("path")->asString());
    REQUIRE(a);
    REQUIRE(b);
    CHECK(a->channels == b->channels);
    auto compared = call("branch.render-ab", R"({"a":"main","b":"alternate","end_beat":4})");
    CHECK(compared.find("a")->isString());
    CHECK(compared.find("b")->isString());
}

TEST_CASE("render job cancellation is observable before worker renders") {
    Root root;
    std::atomic<bool> entered{false}, release{false};
    AppOptions options;
    options.workdir = root.path;
    options.pluginPath = {kFixture};
    options.jobStartedHook = [&] {
        entered.store(true);
        entered.notify_one();
        release.wait(false);
    };
    auto created = EngineApp::create(std::move(options));
    REQUIRE(created);
    auto app = std::move(*created);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_item");
    auto started = d.execute({"render.mix", j(R"({"end_beat":4,"job":true})"), kTest});
    REQUIRE(started);
    entered.wait(false);
    const auto id = started->find("job")->asString();
    json::Value args = json::Value::object();
    args["job"] = id;
    auto cancelled = d.execute({"job.cancel", std::move(args), kTest});
    release.store(true);
    release.notify_one();
    REQUIRE(cancelled);
    CHECK(d.jobs().status(id)->find("status")->asString() == "cancelled");
}

TEST_CASE("add_to_project puts renders on new tracks as one undo step (sync and job)") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_item");
    // Items whose take plays `path` (and where the last one starts).
    double position = -1;
    auto itemsWithFile = [&](const std::string& path) {
        const model::Session& h = app->head();
        int n = 0;
        for (const auto& e : h.items)
            for (const model::Id& takeId : e.value->takes)
                if (const auto* take = h.takes.find(takeId); take && take->source)
                    if (const auto* src = h.media.find(*take->source);
                        src && fs::path(src->path).filename() == fs::path(path).filename()) {
                        ++n;
                        position = e.value->position;
                    }
        return n;
    };
    const std::size_t tracks = app->head().tracks.size();
    auto mix = d.execute(
        {"render.mix", j(R"({"end_beat":2,"start_beat":1,"name":"bounce","add_to_project":true})"),
         kTest});
    REQUIRE(mix);
    // The follow-up ran before execute() returned: a new track, the file on it at start_beat.
    CHECK(app->head().tracks.size() == tracks + 1);
    const std::string path = mix->find("path")->asString();
    CHECK(itemsWithFile(path) == 1);
    CHECK(position == doctest::Approx(1.0));
    CHECK(app->versions().history(1).front()->txn.label == "Add renders to project");
    REQUIRE(d.execute({"history.undo", json::Value::object(), kTest}));
    CHECK(app->head().tracks.size() == tracks);  // the import alone is undone
    CHECK(fs::exists(path));

    auto started = d.execute(
        {"render.stems",
         j(R"({"end_beat":2,"job":true,"pattern":"stem-{track}","add_to_project":true})"), kTest});
    REQUIRE(started);
    json::Value args = json::Value::object();
    args["job"] = started->find("job")->asString();
    args["timeout_s"] = 30;
    auto done = d.execute({"job.wait", args, kTest});
    REQUIRE(done);
    // Whoever sees the job done sees its files in the project.
    const auto& files = *done->find("result")->find("files");
    REQUIRE(files.size() >= 1);
    for (const auto& f : files.asArray()) CHECK(itemsWithFile(f.find("path")->asString()) == 1);
}

TEST_CASE("stem render job publishes files and analysis summaries") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_item");
    auto started = d.execute(
        {"render.stems", j(R"({"end_beat":4,"job":true,"pattern":"queued-{track}"})"), kTest});
    REQUIRE(started);
    const auto id = started->find("job")->asString();
    json::Value args = json::Value::object();
    args["job"] = id;
    args["timeout_s"] = 30;
    auto done = d.execute({"job.wait", args, kTest});
    INFO((done ? std::string() : done.error().toString()));
    REQUIRE(done);
    CHECK(done->find("status")->asString() == "done");
    const auto& file = done->find("result")->find("files")->at(0);
    CHECK(fs::exists(file.find("path")->asString()));
    CHECK(file.find("analysis") != nullptr);
    json::Value get = json::Value::object();
    get["job"] = id;
    auto status = d.execute({"job.get", get, kTest});
    REQUIRE(status);
    CHECK(status->find("status")->asString() == "done");
}

TEST_CASE("offline render reports progress and honors cancellation between blocks") {
    Root root;
    auto app = makeApp(root.path);
    OfflineSettings settings;
    settings.endSeconds = 1.0;
    settings.progressFlag = std::make_shared<std::atomic<double>>(0.0);
    auto rendered = app->renderMix(settings);
    REQUIRE(rendered);
    CHECK(settings.progressFlag->load() == doctest::Approx(1.0));
    auto cancel = std::make_shared<std::atomic<bool>>(true);
    settings.cancelFlag = cancel;
    auto stopped = app->renderMix(settings);
    CHECK_FALSE(stopped);
    if (!stopped) CHECK(stopped.error().code == "job.cancelled");
}

TEST_CASE("render patterns cover the closed tokens, filters, empty separators and escapes") {
    RenderPatternContext c;
    c.project = "Big Song";
    c.track = "Lead Vocal";
    c.folderPath = "Band/Vocals";
    c.region = "Verse";
    c.marker = "Start";
    c.item = "it_1";
    c.take = "Main";
    c.trackNumber = 4;
    c.regionNumber = 2;
    c.counter = 7;
    c.startBeats = 8;
    c.startSeconds = 4;
    c.sampleRate = 48000;
    c.bitDepth = 24;
    const std::vector<std::pair<std::string, std::string>> cases = {
        {"{project}", "Big Song"},
        {"{track}", "Lead Vocal"},
        {"{track_number:02}", "04"},
        {"{folder_path}", "Band/Vocals"},
        {"{region}", "Verse"},
        {"{region_number:02}", "02"},
        {"{marker}", "Start"},
        {"{item}", "it_1"},
        {"{take}", "Main"},
        {"{start:beats}", "8"},
        {"{start:seconds}", "4"},
        {"{start:timecode}", "00-00-04"},
        {"{format}", "wav"},
        {"{sample_rate}", "48000"},
        {"{bit_depth}", "24"},
        {"{counter:03}", "007"},
        {"{track|lower}", "lead vocal"},
        {"{region|upper}", "VERSE"},
        {"{track|replace(\" \",\"_\")}", "Lead_Vocal"},
        {"{track|max(4)}", "Lead"},
    };
    for (const auto& [pattern, expected] : cases) {
        auto out = expandRenderPattern(pattern, c);
        INFO(pattern);
        REQUIRE(out);
        CHECK(*out == expected);
    }
    CHECK(expandRenderPattern("x-{marker}-y", RenderPatternContext{}).value() == "x-y");
    CHECK_FALSE(expandRenderPattern("../escape", c));
    CHECK_FALSE(expandRenderPattern("{unknown}", c));
    CHECK_FALSE(expandRenderPattern("{track|bogus}", c));
    CHECK(expandRenderPattern("{date:YYYY-MM-DD}", c)->size() == 10);
    CHECK(expandRenderPattern("{time:HH-mm-ss}", c)->size() == 8);
}

TEST_CASE("render dry run rejects paths through symlinks outside renders") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    fs::create_directories(app->rendersDir());
    fs::create_directory_symlink(root.path.parent_path(), app->rendersDir() / "escape");
    auto result =
        d.execute({"render.dry-run", j(R"({"end_beat":1,"pattern":"escape/out"})"), kTest});
    CHECK_FALSE(result);
    if (!result) CHECK(result.error().code == "render.path");
}

TEST_CASE("render mix honors normalization and PCM export settings") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_fx");
    auto render = d.execute({"render.mix", j(R"({"name":"mastered","end_beat":4,
        "sample_rate":44100,"channels":1,"bit_depth":16,
        "normalize":{"mode":"lufs_i","target":-20,"limit_true_peak_db":-1}})"),
                             kTest});
    INFO((render ? std::string() : render.error().toString()));
    REQUIRE(render);
    auto audio = audio::readAudio(render->find("path")->asString());
    REQUIRE(audio);
    CHECK(audio->sampleRate == 44100);
    CHECK(audio->channelCount() == 1);
    const float* mono[] = {audio->channels[0].data()};
    auto report = analysis::analyze({mono, static_cast<std::int64_t>(audio->frames()),
                                     audio->sampleRate, analysis::Layout::Mono});
    REQUIRE(report);
    CHECK(report->loudness->integrated == doctest::Approx(-20).epsilon(0.04));
    CHECK(report->loudness->truePeak <= -0.9);
}

TEST_CASE("true peak limiting keeps distant audio at the normalization gain") {
    audio::AudioData original;
    original.sampleRate = 48000;
    original.channels.assign(1, std::vector<float>(96000));
    for (std::size_t i = 0; i < original.frames(); ++i)
        original.channels[0][i] = static_cast<float>(
            0.05 * std::sin(2.0 * M_PI * 440.0 * static_cast<double>(i) / 48000.0));
    original.channels[0][48000] = 1.0f;
    auto unlimited = original, limited = original;
    REQUIRE(normalizeRenderAudio(unlimited, "lufs_i", -20).ok());
    REQUIRE(normalizeRenderAudio(limited, "lufs_i", -20, -1).ok());
    const std::size_t far = 10000;
    CHECK(std::fabs(static_cast<double>(limited.channels[0][far])) >=
          0.8 * std::fabs(static_cast<double>(unlimited.channels[0][far])));
    const float* channels[] = {limited.channels[0].data()};
    auto report = analysis::analyze({channels, static_cast<std::int64_t>(limited.frames()),
                                     limited.sampleRate, analysis::Layout::Mono});
    REQUIRE(report);
    CHECK(report->loudness->truePeak <= -0.95);
}

TEST_CASE("the tick autosaves the project, rotates five, and open reports a newer one") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    app->setFixedClock(1'000'000);
    call("project.set", R"({"name": "Autosaved"})");
    const fs::path project =
        call("project.save", R"({"path": "autosave-project.json"})")["path"].asString();
    REQUIRE(fs::exists(project));
    // The project file is older than the autosaves written below (mtime
    // granularity must not decide the report).
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(project, now - std::chrono::seconds(60));

    // 60 s with no change: nothing is written.
    app->setFixedClock(1'060'000);
    app->tick();
    CHECK_FALSE(fs::exists(app->autosaveDir()));

    // A change plus an interval: one autosave, then six more (rotating to five).
    for (int i = 0; i < 6; ++i) {
        call("project.set", std::string(R"({"name": "Take )") + std::to_string(i) + "\"}");
        app->setFixedClock(1'120'000 + static_cast<std::uint64_t>(i) * 60'000);
        app->tick();
    }
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(app->autosaveDir()))
        names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end());
    CHECK(names == std::vector<std::string>{"autosave-2.json", "autosave-3.json", "autosave-4.json",
                                            "autosave-5.json", "autosave-6.json"});
    // The newest autosave is a valid project file with the last name.
    auto text = readTextFile(app->autosaveDir() / "autosave-6.json");
    REQUIRE(text);
    auto parsed = model::parseProject(*text);
    REQUIRE_MESSAGE(parsed, (parsed ? std::string() : parsed.error().toString()));
    CHECK(parsed->project->name == "Take 5");

    // A fresh app opening the file is told a newer autosave exists, and does
    // not load it: the head is still what was saved.
    auto other = makeApp(root.path / "fresh");
    cmd::Dispatcher d2(registry, *other);
    auto opened =
        d2.execute({"project.open", j(R"({"path": ")" + project.string() + R"("})"), kTest});
    REQUIRE_MESSAGE(opened, (opened ? std::string() : opened.error().toString()));
    CHECK((*opened)["autosave_available"].asBool());
    CHECK(other->head().project->name == "Autosaved");
}

TEST_CASE("the tick restarts the engine after a device change, then gives up and reports") {
    FakeJack& fake = fakeJack();
    fake.reset(1024);
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    AppOptions o;
    o.workdir = root.path;
    o.pluginPath = {kFixture};
    o.jackLibrary = FakeJack::kPath;
    auto created = EngineApp::create(o, {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    app->setFixedClock(1'000'000);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("batch", R"({"steps": [
        {"command": "track.add", "args": {"name": "Bass"}, "as": "bass"},
        {"command": "item.add", "args": {"track": "${bass.track}", "bar": 1,
                                         "notation": "voice bass; 1: A2:w | D3:w"}}]})");
    app->takeEvents();
    call("engine.start", R"({"backend": "jack", "block": 256})");
    call("transport.seek", R"({"seconds": 1.0})");

    // The device changes rate: the tick stops the engine, announces it, and
    // starts it again at the new rate, stopped, at the same position.
    fake.setRate(44100);
    app->tick();
    bool announced = false;
    for (auto& [topic, payload] : app->takeEvents())
        if (topic == "engine.restarting") {
            announced = true;
            CHECK(payload.find("reason")->asString().find("rate") != std::string::npos);
        }
    CHECK(announced);
    CHECK_FALSE(call("engine.status", "{}")["running"].asBool());
    app->tick();  // the retry fires at once (backoff starts at zero)
    auto status = call("engine.status", "{}");
    REQUIRE(status["running"].asBool());
    CHECK(status["sample_rate"].asDouble() == 44100.0);
    // The meter rings report their drops (P5.3c); a quiet engine drops nothing.
    REQUIRE(status.find("meter_drops") != nullptr);
    CHECK(status["meter_drops"]["renderer"].asInt() == 0);
    CHECK(status["meter_drops"]["callback"].asInt() == 0);
    auto t = call("transport.get", "{}");
    CHECK_FALSE(t["playing"].asBool());
    CHECK(t["seconds"].asDouble() == doctest::Approx(1.0).epsilon(0.2));

    // The server goes away and no reopen succeeds: after 30 s the tick gives
    // up and reports engine.error.
    fake.shutdown();
    app->tick();
    fake.failOpen(1);
    app->setFixedClock(1'031'000);
    app->tick();
    bool reported = false;
    for (auto& [topic, payload] : app->takeEvents())
        if (topic == "engine.error" && payload.isObject()) reported = true;
    CHECK(reported);
    CHECK_FALSE(call("engine.status", "{}")["running"].asBool());
    app->tick();  // it stays given up, it does not retry forever
    app->setFixedClock(1'100'000);
    app->tick();
    CHECK_FALSE(call("engine.status", "{}")["running"].asBool());
    fake.reset();
}

TEST_CASE("a UI gesture is one undo step: edits with the same gesture coalesce") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, *app);
    app->attachCommands(dispatcher);
    auto run = [&](const char* command, json::Value args, cmd::Origin origin) {
        auto r = dispatcher.execute({command, std::move(args), std::move(origin)});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    const std::string track = run("track.add", j(R"({"name": "Bass"})"), kTest)["track"].asString();
    const auto versions = [&] {
        return run("history.list", j("{}"), kTest)["versions"].asArray().size();
    };
    const std::size_t before = versions();
    cmd::Origin drag{cmd::Origin::Type::Ui, "gui", "", "fader:" + track + ":1"};
    for (double db : {-1.0, -2.5, -4.0, -6.0}) {
        json::Value args = json::Value::object();
        args["track"] = track;
        args["volume_db"] = db;
        run("track.set", std::move(args), drag);
    }
    CHECK(versions() == before + 1);  // four edits, one step
    CHECK(app->head().tracks.find(*model::Id::parse(track))->volumeDb == doctest::Approx(-6.0));
    // A new gesture is a new step; another origin's same key does not merge.
    drag.gesture = "fader:" + track + ":2";
    run("track.set", j(R"({"track": ")" + track + R"(", "volume_db": -3})"), drag);
    CHECK(versions() == before + 2);
    cmd::Origin other{cmd::Origin::Type::Mcp, "agent", "", drag.gesture};
    run("track.set", j(R"({"track": ")" + track + R"(", "volume_db": -9})"), other);
    CHECK(versions() == before + 3);
    // Undo removes the whole gesture.
    run("history.undo", j("{}"), kTest);
    run("history.undo", j("{}"), kTest);
    run("history.undo", j("{}"), kTest);
    CHECK(app->head().tracks.find(*model::Id::parse(track))->volumeDb == doctest::Approx(0.0));
}

TEST_CASE("midi.notes / midi.edit: ops by original index, one undo step, selection kept") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    app->setupFixture(d, "song_with_item");
    REQUIRE(
        d.execute({"midi.write",
                   j(R"({"item":"it_1","notation":"voice v; 1: C4:q D4:q E4:q F4:q"})"), kTest}));
    auto notes = [&] {
        auto r = d.execute({"midi.notes", j(R"({"item":"it_1"})"), kTest});
        REQUIRE(r);
        return *r->find("notes");
    };
    auto n0 = notes();
    REQUIRE(n0.size() == 4);
    CHECK(n0.at(0).find("pitch")->asInt() == 60);
    CHECK(n0.at(1).find("start")->asDouble() == doctest::Approx(1.0));
    CHECK(n0.at(1).find("length")->asDouble() == doctest::Approx(1.0));
    const auto before = app->head().version;

    // Remove D (1), move E (2) up an octave and later, add a note at 0.5: all
    // indices refer to the list before the command.
    auto edit = d.execute({"midi.edit", j(R"({"item":"it_1","ops":[
        {"op":"remove","notes":[1]},
        {"op":"move","notes":[2],"semitones":12,"beats":0.25},
        {"op":"add","pitch":67,"start":0.5,"length":0.5,"velocity":80},
        {"op":"velocity","notes":[0],"velocity_delta":-127}]})"),
                           kTest});
    INFO((edit ? std::string() : edit.error().toString()));
    REQUIRE(edit);
    auto n1 = notes();
    REQUIRE(n1.size() == 4);  // C, G (added), E' (moved), F
    CHECK(n1.at(0).find("pitch")->asInt() == 60);
    CHECK(n1.at(0).find("velocity")->asInt() == 1);  // clamped
    CHECK(n1.at(1).find("pitch")->asInt() == 67);
    CHECK(n1.at(2).find("pitch")->asInt() == 76);
    CHECK(n1.at(2).find("start")->asDouble() == doctest::Approx(2.25));
    // The touched notes' new indices: C (velocity), G (added), E' (moved).
    const auto& sel = *edit->find("notes");
    REQUIRE(sel.size() == 3);
    CHECK(sel.at(0).asInt() == 0);
    CHECK(sel.at(1).asInt() == 1);
    CHECK(sel.at(2).asInt() == 2);
    CHECK(app->head().version == before + 1);  // one undo step

    // Quantize to the beat: E' at 2.25 snaps to 2.
    REQUIRE(
        d.execute({"midi.edit",
                   j(R"({"item":"it_1","ops":[{"op":"quantize","notes":[2],"grid":1}]})"), kTest}));
    CHECK(notes().at(2).find("start")->asDouble() == doctest::Approx(2.0));
    // Resize the end and the start.
    REQUIRE(d.execute(
        {"midi.edit",
         j(R"({"item":"it_1","ops":[{"op":"resize","notes":[3],"length_delta":1},{"op":"resize","notes":[0],"start_delta":0.5}]})"),
         kTest}));
    auto n2 = notes();
    CHECK(n2.at(3).find("length")->asDouble() == doctest::Approx(2.0));
    // CC points in and out.
    REQUIRE(d.execute(
        {"midi.edit",
         j(R"({"item":"it_1","ops":[{"op":"cc.set","number":1,"points":[{"beat":0,"value":10},{"beat":2,"value":100}]}]})"),
         kTest}));
    auto r = d.execute({"midi.notes", j(R"({"item":"it_1"})"), kTest});
    REQUIRE(r->find("cc")->size() == 2);
    REQUIRE(
        d.execute({"midi.edit",
                   j(R"({"item":"it_1","ops":[{"op":"cc.clear","number":1,"from":1}]})"), kTest}));
    r = d.execute({"midi.notes", j(R"({"item":"it_1"})"), kTest});
    CHECK(r->find("cc")->size() == 1);

    // Errors are structured and change nothing.
    const auto v = app->head().version;
    auto bad = d.execute(
        {"midi.edit", j(R"({"item":"it_1","ops":[{"op":"move","notes":[9],"beats":1}]})"), kTest});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "midi.note_index");
    CHECK(bad.error().path == "/args/ops/0/notes");
    CHECK(app->head().version == v);
    REQUIRE(d.execute({"history.undo", json::Value::object(), kTest}));  // the cc.clear
    r = d.execute({"midi.notes", j(R"({"item":"it_1"})"), kTest});
    CHECK(r->find("cc")->size() == 2);
}

TEST_CASE("route.matrix: type rules, cycles and existing sends") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto add = [&](const char* args) {
        auto r = d.execute({"track.add", j(args), kTest});
        REQUIRE(r);
        return r->find("track")->asString();
    };
    const std::string a = add(R"({"type":"buss","name":"A"})");
    const std::string b = add(R"({"type":"buss","name":"B"})");
    const std::string ch = add(R"({"name":"Vox"})");
    REQUIRE(d.execute(
        {"track.move", j(R"({"track":")" + ch + R"(","parent":")" + a + R"("})"), kTest}));
    auto send =
        d.execute({"route.add", j(R"({"from":")" + a + R"(","to":")" + b + R"("})"), kTest});
    REQUIRE(send);
    auto m = d.execute({"route.matrix", json::Value::object(), kTest});
    REQUIRE(m);
    // Sources in tree order with depth; the master's rule; a channel's rule as a destination.
    const auto& sources = *m->find("sources");
    CHECK(sources.at(0).find("id")->asString() == "tr_0");
    CHECK(sources.at(0).find("rule")->find("code")->asString() == "route.strip_type");
    bool vox = false;
    for (const auto& src : sources.asArray())
        if (src.find("id")->asString() == ch) {
            vox = true;
            CHECK(src.find("depth")->asInt() == 2);
            CHECK(src.find("parent")->asString() == a);
        }
    CHECK(vox);
    for (const auto& dst : m->find("destinations")->asArray())
        if (dst.find("id")->asString() == ch) CHECK(dst.find("rule") != nullptr);
    // A -> B exists, so B -> A (and Vox -> B -> A is fine; B -> A feeds back) is listed.
    REQUIRE(m->find("sends")->size() == 1);
    CHECK(m->find("sends")->at(0).find("from")->asString() == a);
    bool cycle = false;
    for (const auto& c : m->find("cycles")->asArray())
        if (c.find("from")->asString() == b && c.find("to")->asString() == a) cycle = true;
    CHECK(cycle);
    // What the matrix allows, route.add accepts; what it lists as a cycle, route.add refuses.
    CHECK(d.execute({"route.add", j(R"({"from":")" + ch + R"(","to":")" + b + R"("})"), kTest}));
    auto refused =
        d.execute({"route.add", j(R"({"from":")" + b + R"(","to":")" + a + R"("})"), kTest});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "route.cycle");
}

TEST_CASE("log.list: who did what, queries left out, since, origin filter, bounded") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    const cmd::Origin gui{cmd::Origin::Type::Ui, "gui", {}};
    const cmd::Origin agent{cmd::Origin::Type::Agent, "panel", "claude-x"};
    REQUIRE(d.execute({"track.add", j(R"({"name":"A"})"), gui}));
    REQUIRE(d.execute({"track.list", j("{}"), gui}));  // a query
    REQUIRE(d.execute({"track.add", j(R"({"name":"B"})"), agent}));
    REQUIRE_FALSE(d.execute({"track.set", j(R"({"track":"tr_99","mute":true})"), agent}));
    auto log = d.execute({"log.list", j("{}"), kTest});
    REQUIRE(log);
    const auto& e = *log->find("entries");
    REQUIRE(e.size() == 3);  // the query and log.list itself are left out
    CHECK(e.at(0).find("origin")->asString() == "ui:gui");
    CHECK(e.at(1).find("origin")->asString() == "agent:panel/claude-x");
    CHECK(e.at(2).find("ok")->asBool() == false);
    CHECK(e.at(2).find("error")->asString() == "tr.unknown");
    // since, origin, queries.
    const auto seq = e.at(1).find("seq")->asInt();
    json::Value since = json::Value::object();
    since["since"] = seq;
    CHECK(d.execute({"log.list", since, kTest})->find("entries")->size() == 1);
    CHECK(d.execute({"log.list", j(R"({"origin":"agent"})"), kTest})->find("entries")->size() == 2);
    CHECK(d.execute({"log.list", j(R"({"queries":true})"), kTest})->find("entries")->size() == 4);
    // Bounded.
    for (int i = 0; i < 2100; ++i) (void)d.execute({"project.get", j("{}"), kTest});
    CHECK(app->records().size() == engine::EngineApp::kMaxRecords);
}

TEST_CASE("track.set parent_send: off, a loop it would close is refused, back on") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto ok = [&](const char* command, const std::string& args) {
        auto r = d.execute({command, j(args), kTest});
        INFO(command << " " << args << ": " << (r ? std::string() : r.error().toString()));
        REQUIRE(r);
        return *r;
    };
    const std::string a = ok("track.add", R"({"type":"buss","name":"A"})")["track"].asString();
    const std::string b = ok("track.add", R"({"type":"buss","name":"B"})")["track"].asString();
    ok("track.move", R"({"track":")" + b + R"(","parent":")" + a + R"("})");
    // With B -> A as the parent send, A -> B would loop.
    CHECK_FALSE(
        d.execute({"route.add", j(R"({"from":")" + a + R"(","to":")" + b + R"("})"), kTest}));
    ok("track.set", R"({"track":")" + b + R"(","parent_send":false})");
    CHECK_FALSE(app->head().tracks.find(*model::Id::parse(b))->parentSend.enabled);
    const std::string send =
        ok("route.add", R"({"from":")" + a + R"(","to":")" + b + R"("})")["id"].asString();
    auto loop =
        d.execute({"track.set", j(R"({"track":")" + b + R"(","parent_send":true})"), kTest});
    REQUIRE_FALSE(loop);
    CHECK(loop.error().code == "route.cycle");
    CHECK_FALSE(app->head().tracks.find(*model::Id::parse(b))->parentSend.enabled);  // unchanged
    ok("route.remove", R"({"route":")" + send + R"("})");
    ok("track.set", R"({"track":")" + b + R"(","parent_send":true})");
    CHECK(app->head().tracks.find(*model::Id::parse(b))->parentSend.enabled);
    auto master = d.execute({"track.set", j(R"({"track":"tr_0","parent_send":false})"), kTest});
    REQUIRE_FALSE(master);
    CHECK(master.error().code == "track.parent_send");
}

TEST_CASE("engine.inputs: the record inputs while running, empty while stopped") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto stopped = d.execute({"engine.inputs", j("{}"), kTest});
    REQUIRE(stopped);
    CHECK_FALSE(stopped->find("running")->asBool());
    CHECK(stopped->find("audio")->size() == 0);
    REQUIRE(d.execute({"engine.start", j(R"({"backend":"dummy"})"), kTest}));
    auto inputs = d.execute({"engine.inputs", j("{}"), kTest});
    REQUIRE(inputs);
    CHECK(inputs->find("backend")->asString() == "dummy");
    REQUIRE(inputs->find("audio")->size() == 2);
    CHECK(inputs->find("audio")->at(1).find("channel")->asInt() == 2);
    REQUIRE(d.execute({"engine.stop", j("{}"), kTest}));
}

TEST_CASE(
    "automation writing: touch and latch record a strip's control while the transport plays") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("track.add", R"({"name": "Lead"})");
    call("env.add",
         R"({"track": "tr_1", "param": "volume", "points": [{"beat": 0, "value": -6}]})");
    call("env.add", R"({"track": "tr_1", "param": "pan", "points": [{"beat": 0, "value": 0}]})");
    call("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.gain"})");
    call("env.add", R"({"fx": "fx_1", "param": "gain", "points": [{"beat": 0, "value": "0 dB"}]})");
    auto points = [&](const char* env) {
        return app->head().envelopes.find(*model::Id::parse(env))->points;
    };
    // Read mode, or stopped: nothing is written.
    call("track.set", R"({"track": "tr_1", "volume_db": -3})");
    CHECK(points("en_1").size() == 1);
    call("env.set", R"({"env": "en_1", "mode": "touch"})");
    call("env.set", R"({"env": "en_2", "mode": "latch"})");
    call("env.set", R"({"env": "en_3", "mode": "latch"})");
    call("track.set", R"({"track": "tr_1", "volume_db": -3})");  // stopped
    CHECK(points("en_1").size() == 1);

    call("engine.start", R"({"backend": "dummy"})");
    call("transport.play", "{}");
    auto waitBeats = [&](double beats) {
        const double until = call("transport.get", "{}")["beat"].asDouble() + beats;
        for (int i = 0; i < 4000 && call("transport.get", "{}")["beat"].asDouble() < until; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    waitBeats(0.5);
    call("track.set", R"({"track": "tr_1", "volume_db": -12, "pan": -50})");
    call("fx.param.set", R"({"fx": "fx_1", "values": {"gain": "-12 dB"}})");  // text, in its unit
    waitBeats(0.5);
    call("track.set", R"({"track": "tr_1", "volume_db": -20, "pan": 40})");
    waitBeats(0.5);
    const double stopBeat = call("transport.get", "{}")["beat"].asDouble();
    call("transport.stop", "{}");

    // Touch: the hold point before the first write, then the written values.
    const auto vol = points("en_1");
    REQUIRE(vol.size() == 4);  // 0 dB start point, hold, -12, -20
    CHECK(vol[1].value == doctest::Approx(-6));
    CHECK(vol[2].value == doctest::Approx(-12));
    CHECK(vol[3].value == doctest::Approx(-20));
    CHECK(vol[2].pos > 0.4);
    CHECK(vol[3].pos > vol[2].pos);
    // Latch: holds the last value to where the transport stopped.
    const auto pan = points("en_2");
    REQUIRE(pan.size() == 5);
    CHECK(pan.back().value == doctest::Approx(40));
    CHECK(pan.back().pos >= stopBeat - 1e-6);
    CHECK(pan.back().pos > pan[pan.size() - 2].pos);
    // A plugin parameter set through fx.param.set is recorded too (in its unit).
    const auto gain = points("en_3");
    REQUIRE(gain.size() == 4);  // start, hold, -12 dB, held to the stop
    CHECK(gain[2].value == doctest::Approx(-12));
    CHECK(gain.back().value == doctest::Approx(-12));
    // The stop's completion is one undo step of its own.
    auto history = call("history.list", "{}");
    CHECK(history["versions"].at(0).find("label")->asString() ==
          "Automation written");  // newest first
    call("engine.stop", "{}");
}

TEST_CASE("item.paste copies items as they were in a version, even after a cut") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("track.add", R"({"name": "Keys"})");
    call("track.add", R"({"name": "Bass"})");
    call("item.add", R"({"track": "tr_1", "bar": 1, "notation": "voice v; 1: C4:q E4:q G4:h"})");
    call("item.set", R"({"item": "it_1", "fade_in": 0.05})");
    const std::uint64_t copied = app->head().version;  // "Ctrl+C" remembers this
    call("item.remove", R"({"item": "it_1"})");        // the cut
    REQUIRE_FALSE(app->head().items.find(*model::Id::parse("it_1")));
    auto pasted =
        call("item.paste", std::string(R"({"version": )") + std::to_string(copied) +
                               R"(, "items": [{"item": "it_1", "track": "tr_2", "position": 8}]})");
    REQUIRE(pasted["items"].size() == 1);
    const model::Session& s = app->head();
    const model::Item& item = *s.items.find(*model::Id::parse(pasted["items"].at(0).asString()));
    CHECK(item.track == *model::Id::parse("tr_2"));
    CHECK(item.position == doctest::Approx(8));
    CHECK(item.fadeIn.length == doctest::Approx(0.05));
    const model::Take& take = *s.takes.find(*item.activeTake);
    REQUIRE(take.source);
    CHECK(s.midi.find(*take.source)->sequence.notes.size() == 3);
    // Errors: a version that does not exist, a buss, an item not in that version.
    CHECK_FALSE(d.execute({"item.paste", j(R"({"version": 999999, "items": []})"), kTest}));
    call("track.add", R"({"name": "Bus", "type": "buss"})");
    auto onBus = d.execute({"item.paste",
                            j(std::string(R"({"version": )") + std::to_string(copied) +
                              R"(, "items": [{"item": "it_1", "track": "tr_3", "position": 0}]})"),
                            kTest});
    REQUIRE_FALSE(onBus);
    CHECK(onBus.error().code == "track.strip_type");
}

TEST_CASE("track templates: a strip's settings and chains saved, then added as a new strip") {
    Root root;
    cmd::Registry registry;
    registerEngineCommands(registry);
    auto app = makeApp(root.path);
    cmd::Dispatcher d(registry, *app);
    auto call = [&](std::string command, std::string_view args) {
        auto r = d.execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("track.add", R"({"name": "Lead Vox"})");
    call(
        "track.set",
        R"({"track": "tr_1", "volume_db": -4.5, "pan": -20, "color": "#ff8800", "monitor": "auto"})");
    call(
        "fx.add",
        R"({"track": "tr_1", "plugin": "com.suppleaudio.test.gain", "patch": {"params": {"gain": -6}}})");
    call(
        "fx.add",
        R"({"track": "tr_1", "plugin": "com.suppleaudio.test.gain", "chain": "input", "patch": {"params": {"gain": -3}}})");
    auto saved = call("track.template.save", R"({"track": "tr_1", "name": "Vocal"})");
    CHECK(saved["effects"].asInt() == 2);
    CHECK(call("track.template.list", "{}")["templates"].size() == 1);
    // Saving again without replace is refused.
    CHECK_FALSE(
        d.execute({"track.template.save", j(R"({"track": "tr_1", "name": "Vocal"})"), kTest}));
    const auto before = app->versions().history(1000).size();
    auto loaded = call("track.template.load", R"({"name": "Vocal"})");
    CHECK(app->versions().history(1000).size() == before + 1);  // one undo step
    const model::Session& s = app->head();
    const model::Track& strip = *s.tracks.find(*model::Id::parse(loaded["track"].asString()));
    CHECK(strip.name == "Lead Vox");
    CHECK(strip.volumeDb == doctest::Approx(-4.5));
    CHECK(strip.pan == doctest::Approx(-20));
    CHECK(strip.color == "#ff8800");
    CHECK(strip.record.monitor == model::Monitor::Auto);
    REQUIRE(strip.chain.size() == 1);
    REQUIRE(strip.inputChain.size() == 1);
    CHECK(loaded["effects"].size() == 2);
    auto gainOf = [&](model::Id fx) {
        auto patch = call("fx.patch.get", std::string(R"({"fx": ")") + fx.str() + R"("})");
        return patch["patch"]["params"]["gain"].asDouble();
    };
    CHECK(gainOf(strip.chain[0]) == doctest::Approx(-6));
    CHECK(gainOf(strip.inputChain[0]) == doctest::Approx(-3));
    // Errors: neither or both sources, an unknown name.
    CHECK_FALSE(d.execute({"track.template.load", j("{}"), kTest}));
    CHECK_FALSE(d.execute({"track.template.load", j(R"({"name": "Nope"})"), kTest}));
}

TEST_CASE("undo and redo reload the running plan and emit object version events") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    auto call = [&](std::string name, std::string_view args) {
        auto r = bus.execute({std::move(name), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    call("track.add", "{}");
    call("fx.add", R"({"track":"tr_1","plugin":"Test Gain"})");
    DummyBackend backend({});
    REQUIRE(app->startEngine(backend, {}, {.anticipative = false}));
    CHECK(app->engine()->outputLatency() == 0);
    call("fx.param.set", R"({"fx":"fx_1","values":{"latency":"short"}})");
    const auto latency = app->engine()->outputLatency();
    REQUIRE(latency > 0);
    app->takeEvents();
    call("history.undo", "{}");
    CHECK(app->engine()->outputLatency() == 0);
    call("history.redo", "{}");
    CHECK(app->engine()->outputLatency() == latency);
    int changes = 0;
    for (const auto& [topic, payload] : app->takeEvents()) {
        if (topic != "session.changed") continue;
        REQUIRE(payload.isObject());
        CHECK(*payload.find("branch") == "main");
        CHECK(payload.find("version")->isInt());
        ++changes;
    }
    CHECK(changes == 2);
    app->stopEngine();
}

TEST_CASE("project replacement joins jobs and drops stale render completions") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    REQUIRE(bus.execute({"track.add", j(R"({"name":"Vocal"})"), kTest}));
    REQUIRE(bus.execute(
        {"track.set", j(R"({"track":"tr_1","armed":true,"record_input":[1]})"), kTest}));
    REQUIRE(app->save(root.path / "old.json"));
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    REQUIRE(app->startEngine(backend, {.blockSize = 128, .inputs = 1}, {.anticipative = false}));
    REQUIRE(bus.execute({"transport.record", j("{}"), kTest}));
    backend.run(8);
    auto target = renderTarget(*app);
    const auto id = bus.jobs().create();
    std::atomic<bool> joined{false};
    app->startJob([&](std::stop_token stop) {
        while (!stop.stop_requested()) std::this_thread::yield();
        joined.store(true);
    });
    bool published = false;
    json::Value result = json::Value::object();
    result["path"] = "retained.wav";
    completeRenderJob(*app, bus, bus.jobs(), id, target, result, [&] { published = true; });
    REQUIRE(app->newProject({.name = "Fresh"}));
    auto recording = audio::readAudio(root.path / "media" / "rec" / "Vocal-1.wav");
    REQUIRE(recording);
    CHECK(recording->frames() == 1024);
    CHECK(app->head().items.empty());
    REQUIRE(app->engine());
    CHECK_FALSE(app->engine()->recording());
    CHECK(joined.load());
    CHECK(app->projectGeneration() == target.generation + 1);
    CHECK(app->engine()->config().blockSize == 128);
    bus.flushJobEvents();
    CHECK_FALSE(published);
    auto replaced = bus.jobs().status(id);
    REQUIRE(replaced);
    CHECK(*replaced->find("status") == "cancelled");
    REQUIRE(replaced->find("result"));
    CHECK(*replaced->find("result")->find("status") == "render.stale_target");
    bool stale = false;
    for (const auto& [topic, payload] : app->takeEvents())
        if (topic == "render.stale_target") stale = true;
    CHECK(stale);
    const auto branchTarget = renderTarget(*app);
    REQUIRE(app->versions().createBranch("other", "main"));
    const auto branchJob = bus.jobs().create();
    completeRenderJob(*app, bus, bus.jobs(), branchJob, branchTarget, result,
                      [&] { published = true; });
    REQUIRE(app->switchBranch("other"));
    bus.flushJobEvents();
    CHECK_FALSE(published);
    auto status = bus.jobs().status(branchJob);
    REQUIRE(status);
    CHECK(*status->find("result")->find("status") == "render.stale_target");
    CHECK(*status->find("result")->find("path") == "retained.wav");
    app->stopEngine();
}

TEST_CASE("first save rolls back staged media when project publication fails") {
    Root root;
    AppOptions options;
    options.workdir = root.path / "work";
    options.scratchDir = root.path / "scratch";
    auto created = EngineApp::create(options, {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    REQUIRE(writeFileAtomic(app->mediaDir() / "a.wav", "audio bytes"));
    const auto project = root.path / "saved" / "project.json";
    fs::create_directories(project);  // rename over a directory must fail
    REQUIRE_FALSE(app->save(project));
    CHECK(app->projectPath().empty());
    CHECK(fs::exists(app->mediaDir() / "a.wav"));
    CHECK_FALSE(fs::exists(project.parent_path() / "media" / "a.wav"));
    fs::remove(project);
    REQUIRE(app->save(project));
    CHECK(app->projectPath() == project);
    CHECK(fs::exists(project.parent_path() / "media" / "a.wav"));
    CHECK_FALSE(fs::exists(options.scratchDir / "media" / "a.wav"));
}

TEST_CASE("latency probes restore semantic patch and standard params") {
    Root root;
    auto app = makeApp(root.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    REQUIRE(bus.execute({"track.add", j("{}"), kTest}));
    REQUIRE(bus.execute({"fx.add", j(R"({"track":"tr_1","plugin":"Test Gain"})"), kTest}));
    REQUIRE(
        bus.execute({"fx.param.set", j(R"({"fx":"fx_1","values":{"latency":"short"}})"), kTest}));
    auto latency = app->fxLatencySamples(*model::Id::parse("fx_1"), *app->head().fx.begin()->value);
    REQUIRE(latency);
    CHECK(*latency > 0);
}

TEST_CASE("plain plugin parameter setters validate names ids text and saved state") {
    Root root;
    AppOptions options;
    options.workdir = root.path;
    options.pluginPath = {kFixture, FIXTURE_VST3_PATH};
    auto made = EngineApp::create(options, {.console9 = false});
    REQUIRE(made);
    auto app = std::move(*made);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    REQUIRE(bus.execute({"track.add", j("{}"), kTest}));
    REQUIRE(bus.execute({"fx.add", j(R"({"track":"tr_1","plugin":"Test VST3 Gain"})"), kTest}));
    REQUIRE(bus.execute({"fx.param.set", j(R"({"fx":"fx_1","values":{"Gain":0.25}})"), kTest}));
    const auto* fx = app->head().fx.find(*model::Id::parse("fx_1"));
    REQUIRE(fx->state);
    CHECK(fx->params.at(1) == doctest::Approx(0.25));
    auto info = app->findPlugin(fx->plugin.id);
    REQUIRE(info);
    auto instance = host::Instance::create(*info, {});
    REQUIRE(instance);
    REQUIRE((*instance)->loadState(app->head().blobs.at(*fx->state)->data));
    CHECK((*instance)->paramValue(1).value() == doctest::Approx(0.25));
    auto text = (*instance)->paramText(1, 0.5);
    REQUIRE(text);
    json::Value args = j(R"({"fx":"fx_1","values":{}})");
    args["values"]["1"] = *text;
    REQUIRE(bus.execute({"fx.param.set", args, kTest}));
    CHECK(app->head().fx.find(*model::Id::parse("fx_1"))->params.at(1) == doctest::Approx(0.5));
    const auto before = model::serializeProject(app->head());
    for (const auto* values : {R"({"Gain":2})", R"({"Missing":0})", R"({"Gain":false})"}) {
        args["values"] = j(values);
        REQUIRE_FALSE(bus.execute({"fx.param.set", args, kTest}));
        CHECK(model::serializeProject(app->head()) == before);
    }
    REQUIRE(bus.execute({"fx.add", j(R"({"track":"tr_1","plugin":"Test Edge"})"), kTest}));
    auto readonly = bus.execute({"fx.param.set", j(R"({"fx":"fx_2","values":{"7":1}})"), kTest});
    REQUIRE_FALSE(readonly);
    CHECK(readonly.error().code == "fx.param_readonly");
}
