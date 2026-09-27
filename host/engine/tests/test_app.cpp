#include <doctest/doctest.h>
#include <unistd.h>

#include <chrono>
#include <cmath>
#include <filesystem>
#include <set>
#include <thread>

#include <supple/audio/audio_file.h>
#include <supple/engine/app.h>
#include <supple/host/clap_host.h>
#include <supple/model/schemas.h>

#include "fake_jack.h"

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
    o.pluginPath = {kFixture};
    auto app = EngineApp::create(o);
    REQUIRE(app);
    return std::move(*app);
}

json::Value j(std::string_view text) { return json::parse(text).value(); }
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

}  // namespace

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
          doctest::Approx(20 * std::log10(0.25 * 96 / 127 * std::sqrt(0.5)) - 6).epsilon(0.01));
    CHECK(fs::exists((*mix)["path"].asString()));

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
    auto created = EngineApp::create(o);
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

    // A device at another rate than the project: a clean error, nothing left open.
    fake.setRate(44100);
    auto refused = d.execute({"engine.start", j("{}"), kTest});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "engine.sample_rate");
    CHECK(fake.open() == 0);
    CHECK_FALSE(app->engine());
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
    DummyBackend backend({.mode = DummyBackend::Mode::Thread,
                          .paced = true,
                          .input = [](float* const* in, std::uint32_t channels, std::uint32_t n,
                                      std::int64_t frame) {
                              for (std::uint32_t c = 0; c < channels; ++c)
                                  for (std::uint32_t i = 0; i < n; ++i)
                                      in[c][i] =
                                          0.5f * static_cast<float>(std::sin(
                                                     2.0 * 3.14159265358979 * 441.0 *
                                                     static_cast<double>(frame + i) / 48000.0));
                          }});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}).ok());
    auto started = call("transport.record", "{}");
    CHECK(started["recording"].asBool());
    CHECK(started["playing"].asBool());
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    auto stopped = call("transport.stop", "{}");
    CHECK_FALSE(stopped["recording"].asBool());
    REQUIRE(stopped["recorded"].size() == 1);
    const json::Value& take = stopped["recorded"].at(0);
    CHECK(take.find("track")->asString() == "tr_1");
    CHECK(take.find("seconds")->asDouble() > 0.2);
    CHECK(take.find("lost_frames")->asInt() == 0);
    CHECK(fs::exists(take.find("file")->asString()));
    CHECK(app->head().items.size() == 1);
    app->stopEngine();

    // The take plays back in a render: the sine, centred (-3 dB).
    auto mix = call("render.mix", R"({"name": "take", "end_beat": 0.5})");
    auto audio = audio::readAudio(mix["path"].asString());
    REQUIRE(audio);
    double peak = 0;
    for (float x : audio->channels[0]) peak = std::max(peak, std::fabs(double(x)));
    CHECK(peak == doctest::Approx(0.5 * 0.70710678).epsilon(0.01));

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
        {.mode = DummyBackend::Mode::Thread,
         .paced = true,
         .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
             for (std::uint32_t c = 0; c < channels; ++c)
                 for (std::uint32_t i = 0; i < n; ++i) in[c][i] = 0.5f;
         }});
    REQUIRE(app->startEngine(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}).ok());
    call("transport.seek", R"({"seconds": 0.5})");
    call("transport.record", "{}");
    // While it records, its file is not offered.
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(call("project.recoverable", "{}")["recordings"].size() == 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
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
    CHECK(double(left[49000]) == doctest::Approx(0.25 * 0.70710678).epsilon(0.01));
    CHECK(double(left[95000]) == doctest::Approx(0.25 * 0.70710678).epsilon(0.01));
    CHECK(double(left[97000]) == 0.0);
}
