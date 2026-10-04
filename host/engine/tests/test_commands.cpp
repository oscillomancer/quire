#include <doctest/doctest.h>

#include <unistd.h>
#include <cmath>
#include <filesystem>

#include <supple/audio/audio_file.h>
#include <supple/base/files.h>
#include <supple/base/sha256.h>
#include <supple/engine/app.h>
#include <supple/model/schemas.h>

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {
struct Workdir {
    fs::path path =
        fs::temp_directory_path() / ("supple-engine-commands-" + std::to_string(::getpid()));
    Workdir() {
        fs::remove_all(path);
        fs::create_directories(path);
    }
    ~Workdir() { fs::remove_all(path); }
};

std::unique_ptr<EngineApp> makeApp(const fs::path& path) {
    AppOptions options;
    options.workdir = path;
    options.pluginPath = {FIXTURE_CLAP_PATH};
    auto app = EngineApp::create(options, {.console9 = false});
    REQUIRE(app);
    return std::move(*app);
}

const cmd::Origin kOrigin{cmd::Origin::Type::Test, "commands", ""};
json::Value j(std::string_view text) { return json::parse(text).value(); }
json::Value run(cmd::Dispatcher& bus, std::string name, std::string_view args) {
    auto result = bus.execute({std::move(name), j(args), kOrigin});
    REQUIRE_MESSAGE(result, (result ? std::string() : result.error().toString()));
    return *result;
}
double peak(const fs::path& path) {
    auto audio = audio::readAudio(path);
    REQUIRE(audio);
    double max = 0;
    for (float x : audio->channels[0]) max = std::max(max, std::fabs(double(x)));
    return max;
}
}  // namespace

TEST_CASE("route sends through a gain bus, rejects cycles and replays in a branch merge") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Source"})");
    run(bus, "track.add", R"({"name":"Bus","type":"buss"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "fx.add",
        R"({"track":"tr_2","plugin":"com.suppleaudio.test.gain","patch":{"params":{"gain":-6}}})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    const double direct =
        peak(run(bus, "render.mix", R"({"name":"direct","end_beat":2})")["path"].asString());
    run(bus, "route.add", R"({"from":"tr_1","to":"tr_2"})");
    auto listed = run(bus, "route.list", "{}");
    REQUIRE(listed["routes"].size() == 1);
    CHECK(listed["routes"].at(0).find("id")->asString() == "sd_1");
    const double sent =
        peak(run(bus, "render.mix", R"({"name":"sent","end_beat":2})")["path"].asString());
    CHECK(sent > direct * 1.2);
    auto cycle = bus.execute({"route.add", j(R"({"from":"tr_2","to":"tr_1"})"), kOrigin});
    REQUIRE_FALSE(cycle);
    CHECK(cycle.error().code == "route.cycle");
    auto self = bus.execute({"route.add", j(R"({"from":"tr_1","to":"tr_1"})"), kOrigin});
    REQUIRE_FALSE(self);
    CHECK(self.error().code == "route.cycle");

    run(bus, "branch.create", R"({"name":"agent/test/route"})");
    run(bus, "route.set", R"({"route":"sd_1","volume_db":-3})");
    run(bus, "branch.switch", R"({"name":"main"})");
    run(bus, "marker.add", R"({"pos":1,"name":"Check"})");
    run(bus, "branch.merge", R"({"from":"agent/test/route"})");
    CHECK(app->head().routes.find(*model::Id::parse("sd_1"))->volumeDb == -3);
    CHECK(app->head().markers.size() == 1);
}

TEST_CASE("audio split is sample exact and take activation changes the render") {
    Workdir dir;
    audio::AudioData source;
    source.sampleRate = 48000;
    source.channels = {std::vector<float>(48000)};
    for (std::size_t i = 0; i < source.frames(); ++i)
        source.channels[0][i] =
            static_cast<float>(0.25 * std::sin(2 * 3.141592653589793 * 440 * double(i) / 48000));
    REQUIRE(audio::writeWav(dir.path / "source.wav", source).ok());
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Audio"})");
    const std::string add = "{\"track\":\"tr_1\",\"audio_file\":" +
                            json::writeCompact(json::Value((dir.path / "source.wav").string())) +
                            "}";
    run(bus, "item.add", add);
    auto before = audio::readAudio(
        run(bus, "render.mix", R"({"name":"before","end_beat":2})")["path"].asString());
    REQUIRE(before);
    run(bus, "item.split", R"({"item":"it_1","at":1})");
    auto after = audio::readAudio(
        run(bus, "render.mix", R"({"name":"after","end_beat":2})")["path"].asString());
    REQUIRE(after);
    REQUIRE(before->frames() == after->frames());
    std::size_t mismatches = 0;
    for (std::size_t i = 0; i < before->frames(); ++i)
        if (before->channels[0][i] != after->channels[0][i]) ++mismatches;
    CHECK(mismatches == 0);
    CHECK(app->head().items.size() == 2);
    CHECK(app->head().takes.size() == 2);
    run(bus, "take.add", R"({"item":"it_1","from":"tk_1"})");
    run(bus, "take.set", R"({"take":"tk_3","gain_db":-24})");
    const double loud =
        peak(run(bus, "render.mix", R"({"name":"loud","end_beat":1})")["path"].asString());
    run(bus, "take.activate", R"({"item":"it_1","take":"tk_3"})");
    const double quiet =
        peak(run(bus, "render.mix", R"({"name":"quiet","end_beat":1})")["path"].asString());
    CHECK(quiet < loud * 0.1);

    // take.set: the rate and pitch fields round-trip, and an unknown pitch
    // mode is rejected with the valid choices.
    run(bus, "take.set",
        R"({"take":"tk_1","playrate":2,"preserve_pitch":false,"pitch":-3,"pitch_mode":"resample"})");
    auto takes = run(bus, "take.list", R"({"item":"it_1"})");
    REQUIRE(takes["takes"].size() == 2);
    CHECK(takes["takes"].at(0).find("playrate")->asDouble() == 2.0);
    CHECK_FALSE(takes["takes"].at(0).find("preserve_pitch")->asBool());
    CHECK(takes["takes"].at(0).find("pitch")->asDouble() == -3.0);
    CHECK(takes["takes"].at(0).find("pitch_mode")->asString() == "resample");
    CHECK(takes["takes"].at(1).find("playrate")->asDouble() == 1.0);
    auto bad =
        bus.execute({"take.set", j(R"({"take":"tk_1","pitch_mode":"phase-vocoder"})"), kOrigin});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "command.invalid_args");
    REQUIRE(bad.error().details.size() == 1);
    CHECK(bad.error().details[0].hint.find("signalsmith") != std::string::npos);
}

TEST_CASE("FX order is audible and rulers survive save and reopen") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Tone"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "fx.add",
        R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","patch":{"params":{"gain":-12}}})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    const double low =
        peak(run(bus, "render.mix", R"({"name":"low","end_beat":2})")["path"].asString());
    run(bus, "fx.move", R"({"fx":"fx_2","index":0})");
    const double high =
        peak(run(bus, "render.mix", R"({"name":"high","end_beat":2})")["path"].asString());
    CHECK(high > low * 3.5);
    run(bus, "marker.add", R"({"pos":4,"name":"Verse"})");
    run(bus, "region.add", R"({"start":4,"end":8,"name":"Verse"})");
    const std::string save =
        "{\"path\":" + json::writeCompact(json::Value((dir.path / "project.json").string())) + "}";
    run(bus, "project.save", save);
    auto reopened = makeApp(dir.path / "reopened");
    REQUIRE(reopened->open(dir.path / "project.json").ok());
    CHECK(reopened->head().markers.find(*model::Id::parse("mk_1"))->name == "Verse");
    CHECK(reopened->head().regions.find(*model::Id::parse("rg_1"))->end == 8);

    run(bus, "branch.create", R"({"name":"agent/test/arrangement"})");
    run(bus, "item.set", R"({"item":"it_1","gain_db":-3})");
    run(bus, "region.set", R"({"id":"rg_1","end":10})");
    run(bus, "fx.move", R"({"fx":"fx_2","index":1})");
    run(bus, "branch.switch", R"({"name":"main"})");
    run(bus, "marker.set", R"({"id":"mk_1","name":"Start"})");
    run(bus, "branch.merge", R"({"from":"agent/test/arrangement"})");
    CHECK(app->head().items.find(*model::Id::parse("it_1"))->gainDb == -3);
    CHECK(app->head().regions.find(*model::Id::parse("rg_1"))->end == 10);
    CHECK(app->head().tracks.find(*model::Id::parse("tr_1"))->chain[1] ==
          *model::Id::parse("fx_2"));
    CHECK(app->head().markers.find(*model::Id::parse("mk_1"))->name == "Start");
}

TEST_CASE("MIDI split keeps held notes audible on both sides") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Tone"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:h r:h"})");
    run(bus, "item.split", R"({"item":"it_1","at":1})");
    const auto rendered = run(bus, "render.mix", R"({"name":"midi-split","end_beat":2})");
    auto audio = audio::readAudio(rendered.find("path")->asString());
    REQUIRE(audio);
    double left = 0, right = 0;
    for (std::size_t i = 10000; i < 20000; ++i)
        left = std::max(left, std::fabs(double(audio->channels[0][i])));
    for (std::size_t i = 30000; i < 40000; ++i)
        right = std::max(right, std::fabs(double(audio->channels[0][i])));
    CHECK(left > 0.05);
    CHECK(right > 0.05);
    const auto leftSource = app->head().takes.find(*model::Id::parse("tk_1"))->source;
    const auto rightSource = app->head().takes.find(*model::Id::parse("tk_2"))->source;
    REQUIRE(leftSource);
    REQUIRE(rightSource);
    CHECK(leftSource != rightSource);  // non-pooled MIDI stays independent
    const std::string leftNotation = run(bus, "midi.get", R"({"item":"it_1"})")["text"].asString();
    run(bus, "midi.write", R"({"item":"it_2","notation":"voice a; 1: r:w"})");
    CHECK(run(bus, "midi.get", R"({"item":"it_1"})")["text"].asString() == leftNotation);
    CHECK(app->head().takes.find(*model::Id::parse("tk_2"))->startOffset == 0);
}

TEST_CASE("a VST3 is listed with its format and describes its plain parameters") {
    Workdir dir;
    AppOptions options;
    options.workdir = dir.path;
    options.pluginPath = {FIXTURE_VST3_PATH};
    auto created = EngineApp::create(options, {.console9 = false});
    REQUIRE(created);
    auto app = std::move(*created);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    json::Value listed = run(bus, "plugin.list", R"({"filter":"Sidechain"})");
    REQUIRE(listed["plugins"].asArray().size() == 1);
    json::Value plugin = listed["plugins"].asArray()[0];
    CHECK(plugin["format"].asString() == "vst3");
    run(bus, "track.add", R"({"name":"Fx"})");
    const std::string args = R"({"track":"tr_1","plugin":")" + plugin["id"].asString() + R"("})";
    json::Value added = run(bus, "fx.add", args);
    const std::string fx = added["fx"].asString();
    json::Value full = run(bus, "fx.describe", R"({"fx":")" + fx + R"(","full":true})");
    REQUIRE(full["params"].isArray());
    REQUIRE(full["params"].asArray().size() == 1);
    CHECK(full["params"].asArray()[0]["name"].asString() == "Gain");
    CHECK(full["params_total"].asDouble() == 1);
    json::Value none =
        run(bus, "fx.describe", R"({"fx":")" + fx + R"(","full":true,"filter":"cutoff"})");
    CHECK(none["params"].asArray().empty());
    CHECK(none["params_total"].asDouble() == 0);
}

namespace {
// Runs a command that must fail and returns its error code ("" when it worked).
std::string fails(cmd::Dispatcher& bus, std::string name, std::string_view args) {
    auto result = bus.execute({std::move(name), j(args), kOrigin});
    return result ? std::string() : result.error().code;
}
std::vector<model::Id> childrenOf(const model::Session& s, std::string_view track) {
    const auto id = model::Id::parse(track);
    REQUIRE(id);
    const model::Track* t = s.tracks.find(*id);
    REQUIRE(t);
    return t->children;
}
}  // namespace

TEST_CASE("track.move re-parents, reorders, and refuses a cycle (review 14 D5)") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"A","type":"buss"})");  // tr_1
    run(bus, "track.add", R"({"name":"B","type":"buss"})");  // tr_2
    run(bus, "track.add", R"({"name":"C","index":0})");      // tr_3, first under the master
    const auto first = childrenOf(app->view(), "tr_0");
    REQUIRE(first.size() == 3);
    CHECK(first[0].str() == "tr_3");
    // A pure reorder: parent defaults to the current one.
    run(bus, "track.move", R"({"track":"tr_1","index":0})");
    const auto reordered = childrenOf(app->view(), "tr_0");
    REQUIRE(reordered.size() == 3);
    CHECK(reordered[0].str() == "tr_1");
    CHECK(reordered[1].str() == "tr_3");
    // Into a folder: the old parent loses it, the new one gains it.
    run(bus, "track.move", R"({"track":"tr_2","parent":"tr_1"})");
    CHECK(app->view().tracks.find(*model::Id::parse("tr_2"))->parent == *model::Id::parse("tr_1"));
    CHECK(childrenOf(app->view(), "tr_1")[0].str() == "tr_2");
    const auto master = childrenOf(app->view(), "tr_0");
    REQUIRE(master.size() == 2);
    // And the model stays consistent: the track is in exactly one parent.
    std::size_t listed = 0;
    for (const auto& e : app->view().tracks)
        for (model::Id child : e.value->children)
            if (child == *model::Id::parse("tr_2")) ++listed;
    CHECK(listed == 1);
    // A track cannot move under its own child, under itself, or move the master.
    CHECK(fails(bus, "track.move", R"({"track":"tr_1","parent":"tr_2"})") == "tr.cycle");
    CHECK(fails(bus, "track.move", R"({"track":"tr_1","parent":"tr_1"})") == "tr.cycle");
    CHECK(fails(bus, "track.move", R"({"track":"tr_0"})") == "track.strip_type");
    CHECK(fails(bus, "track.move", R"({"track":"tr_9"})") == "tr.unknown");
    // Sorting an index past the end appends, like track.add.
    run(bus, "track.move", R"({"track":"tr_2","parent":"tr_0","index":99})");
    const auto back = childrenOf(app->view(), "tr_0");
    CHECK(back.size() == 3);
    CHECK(back[2].str() == "tr_2");
}

TEST_CASE("item.set rejects a conflicting position and a zero length (review 14 D1)") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"A"})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    CHECK(fails(bus, "item.set", R"({"item":"it_1","position":1,"bar":2})") == "item.position");
    CHECK(fails(bus, "item.set", R"({"item":"it_1","length":0})") == "item.length");
    // A negative length is refused by the schema before the command runs.
    CHECK(fails(bus, "item.set", R"({"item":"it_1","length":-1})") == "command.invalid_args");
    run(bus, "item.set", R"({"item":"it_1","name":"Intro"})");  // it_1 has an active take
    run(bus, "item.set", R"({"item":"it_1","bar":2})");         // one of them is fine
    CHECK(app->view().items.find(*model::Id::parse("it_1"))->position > 0);
}

TEST_CASE("item.set locks and unlocks an item; a locked item refuses other edits") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"A"})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    const auto item = [&] { return app->view().items.find(*model::Id::parse("it_1")); };
    CHECK_FALSE(item()->locked);
    run(bus, "item.set", R"({"item":"it_1","locked":true})");
    CHECK(item()->locked);
    CHECK(fails(bus, "item.set", R"({"item":"it_1","gain_db":-3})") == "item.locked");
    CHECK(item()->gainDb == doctest::Approx(0.0));              // the refused edit rolled back
    run(bus, "item.set", R"({"item":"it_1","locked":false})");  // unlocking is allowed
    CHECK_FALSE(item()->locked);
    run(bus, "item.set", R"({"item":"it_1","gain_db":-3})");
    CHECK(item()->gainDb == doctest::Approx(-3.0));
}

TEST_CASE("fx.move names a real command in its hint, and stops at the first chain (D2)") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"A"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.sine"})");
    run(bus, "item.add", R"({"track":"tr_1","notation":"voice a; 1: C4:w"})");
    // The "not in a track chain" rejection (whose hint used to name a command
    // that does not exist) needs an fx that only a take chain holds, and no
    // command can put one there yet: the fix is covered by fx.move on a track
    // chain (the first chain that holds it) staying correct.
    run(bus, "take.add", R"({"item":"it_1","from":"tk_1"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    CHECK(app->view().tracks.find(*model::Id::parse("tr_1"))->chain.size() == 2);
    run(bus, "fx.move", R"({"fx":"fx_1","index":1})");
    const auto& chain = app->view().tracks.find(*model::Id::parse("tr_1"))->chain;
    REQUIRE(chain.size() == 2);
    CHECK(chain[0].str() == "fx_2");
    CHECK(chain[1].str() == "fx_1");
    CHECK(fails(bus, "fx.move", R"({"fx":"fx_1","index":9})") == "fx.index");
}

TEST_CASE("fx.add {chain:\"input\"} fills the input chain; fx.move/remove work on it") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"A"})");
    auto strip = [&] { return *app->view().tracks.find(*model::Id::parse("tr_1")); };
    // The input chain is separate from the track's chain.
    auto added = run(bus, "fx.add",
                     R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","chain":"input"})");
    CHECK(added["chain"].asArray().size() == 1);
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.sine"})");
    CHECK(strip().inputChain.size() == 1);
    CHECK(strip().inputChain[0].str() == "fx_1");
    REQUIRE(strip().chain.size() == 1);
    CHECK(strip().chain[0].str() == "fx_2");
    // Reorder inside the input chain.
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","chain":"input"})");
    REQUIRE(strip().inputChain.size() == 2);
    CHECK(strip().inputChain[0].str() == "fx_1");
    CHECK(strip().inputChain[1].str() == "fx_3");
    run(bus, "fx.move", R"({"fx":"fx_3","index":0})");
    CHECK(strip().inputChain[0].str() == "fx_3");
    CHECK(strip().inputChain[1].str() == "fx_1");
    CHECK(fails(bus, "fx.move", R"({"fx":"fx_3","index":9})") == "fx.index");
    // An input-chain effect stays on its track: moving it elsewhere is refused.
    run(bus, "track.add", R"({"name":"B"})");
    CHECK(fails(bus, "fx.move", R"({"fx":"fx_3","track":"tr_2","index":0})") == "fx.chain");
    // Remove empties it from the input chain, leaving the track's chain alone.
    run(bus, "fx.remove", R"({"fx":"fx_3"})");
    CHECK(strip().inputChain.size() == 1);
    CHECK(strip().chain.size() == 1);
    // A bad chain name is rejected by the schema.
    CHECK_FALSE(
        fails(bus, "fx.add",
              R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","chain":"sidechain"})")
            .empty());
}

TEST_CASE("key routes over the bus: route.add {fx}, validation, listing, removal (ADR-0018)") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Kick"})");
    run(bus, "track.add", R"({"name":"Bass"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "item.add", R"({"track":"tr_1","bar":1,"notation":"voice k; 1: A4:w"})");
    json::Value added =
        run(bus, "fx.add", R"({"track":"tr_2","plugin":"com.suppleaudio.test.keymix"})");
    const std::string keyMix = added["fx"].asString();
    added = run(bus, "fx.add", R"({"track":"tr_2","plugin":"com.suppleaudio.test.gain"})");
    const std::string gain = added["fx"].asString();
    json::Value described = run(bus, "fx.describe", R"({"fx":")" + keyMix + R"("})");
    CHECK(described["sidechain_channels"].asInt() == 2);
    auto noKey =
        bus.execute({"route.add", j(R"({"from":"tr_1","fx":")" + gain + R"("})"), kOrigin});
    REQUIRE_FALSE(noKey);
    CHECK(noKey.error().code == "route.no_sidechain");
    auto wrongTrack = bus.execute(
        {"route.add", j(R"({"from":"tr_1","to":"tr_1","fx":")" + keyMix + R"("})"), kOrigin});
    REQUIRE_FALSE(wrongTrack);
    CHECK(wrongTrack.error().code == "route.endpoint");
    json::Value made =
        run(bus, "route.add", R"({"from":"tr_1","fx":")" + keyMix + R"(","tap":"pre_fader"})");
    const std::string route = made["id"].asString();
    json::Value listed = run(bus, "route.list", "{}");
    REQUIRE(listed["routes"].asArray().size() == 1);
    json::Value first = listed["routes"].asArray()[0];
    CHECK(first["fx"].asString() == keyMix);
    CHECK(first["to"].asString() == "tr_2");
    // A key from Bass into a plug-in on Kick would close a loop.
    added = run(bus, "fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.keymix"})");
    const std::string kickKey = added["fx"].asString();
    auto cycle =
        bus.execute({"route.add", j(R"({"from":"tr_2","fx":")" + kickKey + R"("})"), kOrigin});
    REQUIRE_FALSE(cycle);
    CHECK(cycle.error().code == "route.cycle");
    // Audible: the keyed track now carries the kick.
    json::Value keyed = run(bus, "render.mix", R"({"end_beat":2})");
    run(bus, "route.set", R"({"route":")" + route + R"(","mute":true})");
    json::Value unkeyed = run(bus, "render.mix", R"({"end_beat":2})");
    CHECK(peak(keyed["path"].asString()) > peak(unkeyed["path"].asString()) * 1.3);
    // Removing the keyed plug-in removes its key route.
    run(bus, "fx.remove", R"({"fx":")" + keyMix + R"("})");
    json::Value after = run(bus, "route.list", "{}");
    CHECK(after["routes"].asArray().empty());
}

TEST_CASE("typed strip commands: rejected edits roll back and masters render independently") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Alt","type":"master","parent":null})");
    run(bus, "track.add", R"({"name":"Group","type":"buss","parent":"tr_1"})");
    run(bus, "track.add", R"({"name":"Tone","parent":"tr_2"})");
    run(bus, "fx.add", R"({"track":"tr_3","plugin":"Test Sine"})");
    run(bus, "item.add", R"({"track":"tr_3","notation":"voice a; 1: C4:w"})");
    const auto masters = app->head().project->masters;
    CHECK(masters == std::vector<model::Id>{model::kMaster, *model::Id::parse("tr_1")});
    auto rejected = [&](const char* command, const char* args, const char* code) {
        const auto version = app->head().version;
        auto result = bus.execute({command, j(args), kOrigin});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == code);
        CHECK_FALSE(result.error().hint.empty());
        CHECK(app->head().version == version);
        CHECK(model::checkInvariants(app->head()).ok());
    };
    rejected("track.add", R"({"parent":"tr_3"})", "track.strip_type");
    rejected("track.set", R"({"track":"tr_3","type":"buss"})", "track.strip_type");
    rejected("track.set", R"({"track":"tr_0","type":"buss"})", "track.strip_type");
    rejected("track.set", R"({"track":"tr_2","armed":true})", "track.strip_type");
    rejected("track.set", R"({"track":"tr_3","color":"pink"})", "track.color");
    rejected("track.set", R"({"track":"tr_3","color":"#12345g"})", "track.color");
    run(bus, "track.set", R"({"track":"tr_3","color":"teal"})");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_3"))->color == "teal");
    run(bus, "track.set", R"({"track":"tr_3","color":"#a1b2c3"})");
    run(bus, "track.set", R"({"track":"tr_3","color":""})");
    CHECK(app->head().tracks.find(*model::Id::parse("tr_3"))->color.empty());
    rejected("item.move", R"({"item":"it_1","track":"tr_2"})", "track.strip_type");
    rejected("item.add", R"({"track":"tr_1"})", "track.strip_type");
    rejected("route.add", R"({"from":"tr_0","to":"tr_2"})", "route.strip_type");
    rejected("route.add", R"({"from":"tr_0","to":"tr_1"})", "route.strip_type");
    rejected("route.add", R"({"from":"tr_2","to":"tr_3"})", "route.cycle");
    rejected("env.add", R"({"track":"tr_3","param":"trim"})", "track.strip_type");
    rejected("render.mix", R"({"master":"tr_2","end_beat":1})", "track.strip_type");
    auto list = run(bus, "track.list", "{}");
    CHECK(list["tracks"].at(0).find("type")->asString() == "master");
    CHECK(list["tracks"].at(2).find("type")->asString() == "buss");
    CHECK(describeSession(app->head()).find("[master]") != std::string::npos);
    CHECK(describeSession(app->head()).find("[buss]") != std::string::npos);
    const auto main = run(bus, "render.mix", R"({"name":"main","end_beat":1})");
    const auto alt = run(bus, "render.mix", R"({"master":"tr_1","name":"alt","end_beat":1})");
    CHECK(peak(main.find("path")->asString()) == 0);
    CHECK(peak(alt.find("path")->asString()) > 0.01);
    run(bus, "project.set",
        R"({"console9":false,"render":{"matrix":{"project":["tr_0","tr_1"]},"pattern":"master-{track}"}})");
    auto matrix = run(bus, "render.dry-run", R"({"operation":"regions","end_beat":1})");
    CHECK(matrix["files"].size() == 2);
    auto actual = run(bus, "render.regions", R"({"end_beat":1})");
    CHECK(actual["files"].size() == 2);
    run(bus, "route.add", R"({"from":"tr_3","to":"tr_0","pan":"follow"})");
    CHECK(run(bus, "route.list", "{}")["routes"].at(0).find("pan")->asString() == "follow");
    run(bus, "route.set", R"({"route":"sd_1","pan":-25})");
    CHECK(app->head().routes.find(*model::Id::parse("sd_1"))->pan.percent == -25);
    run(bus, "route.remove", R"({"route":"sd_1"})");
    run(bus, "track.add", R"({"name":"Empty"})");
    run(bus, "track.set", R"({"track":"tr_4","type":"master"})");
    CHECK(app->head().project->masters.back().str() == "tr_4");
    run(bus, "track.move", R"({"track":"tr_4","index":1})");
    CHECK(app->head().project->masters[1].str() == "tr_4");
    run(bus, "track.set", R"({"track":"tr_4","type":"buss"})");
    CHECK(app->head().project->masters == masters);
    run(bus, "track.set", R"({"track":"tr_4","type":"master"})");
    run(bus, "track.remove", R"({"track":"tr_4"})");
    CHECK(app->head().project->masters == masters);
}

TEST_CASE("project.open returns migration warnings in its load report") {
    Workdir dir;
    auto app = makeApp(dir.path);
    auto doc = model::projectToJson(app->head());
    doc["schema"] = "supple.project/1.0";
    doc["project"]["pan_law_db"] = -3;
    REQUIRE(writeFileAtomic(dir.path / "legacy.json", json::writeCanonical(doc)).ok());
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    auto loaded = run(bus, "project.open", R"({"path":"legacy.json"})");
    REQUIRE(loaded.find("warnings"));
    CHECK(loaded["warnings"].size() == 2);
    CHECK(loaded["warnings"].at(1).find("path")->asString() == "/project/console9");
}

namespace {
struct ChainFixture {
    fs::path dir = fs::temp_directory_path() / ("supple-chain-tests-" + std::to_string(::getpid()));
    std::unique_ptr<EngineApp> app;
    cmd::Registry registry;
    std::unique_ptr<cmd::Dispatcher> bus;
    ChainFixture() {
        fs::remove_all(dir);
        AppOptions options;
        options.workdir = dir;
        options.chainDir = dir / "chains";
        options.pluginPath = {FIXTURE_CLAP_PATH};
        auto result = EngineApp::create(options, {.console9 = false});
        REQUIRE(result);
        app = std::move(*result);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
    }
    ~ChainFixture() {
        bus.reset();
        app.reset();
        fs::remove_all(dir);
    }
    json::Value run(const char* name, std::string_view args = "{}") {
        return ::run(*bus, name, args);
    }
    std::size_t history() { return app->versions().history(1000).size(); }
    json::Value document(const char* name = "Tone") {
        auto text = readTextFile(dir / "chains" / (std::string(name) + ".json"));
        REQUIRE(text);
        auto value = json::parse(*text);
        REQUIRE(value);
        return *value;
    }
    void write(json::Value doc, const char* name = "Tone") {
        REQUIRE(writeFileAtomic(dir / "chains" / (std::string(name) + ".json"),
                                json::writeCanonical(doc)));
    }
};
}  // namespace

TEST_CASE("FX chain presets round-trip ordered patches, blobs and options in one edit") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    f.run("track.add", R"({"name":"Target"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain","patch":{"params":{"gain":-9}}})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Edge"})");
    f.run("fx.set",
          R"({"fx":"fx_1","label":"Quiet","bypass":true,"wet":37,"isolation":"in_process"})");
    f.run("fx.set", R"({"fx":"fx_2","offline":true,"wet":82})");
    const auto beforeSave = f.history();
    auto saved = f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone"})");
    CHECK(saved["effects"].asInt() == 2);
    CHECK(f.history() == beforeSave);
    auto text = readTextFile(saved["path"].asString());
    REQUIRE(text);
    auto doc = f.document();
    CHECK(doc["schema"].asString() == "supple.fx-chain/1.0");
    REQUIRE(doc["effects"].size() == 2);
    CHECK(doc["effects"].at(1).find("state")->asString() == "AAAAAA==");
    // Oversampling is part of the preset even though fx.set has no such option.
    doc["effects"].asArray()[0]["oversampling"] = 4;
    f.write(doc);
    auto listed = f.run("fx.chain.list");
    REQUIRE(listed["chains"].size() == 1);
    CHECK(listed["chains"].at(0).find("effects")->asInt() == 2);
    const auto beforeLoad = f.history();
    auto loaded = f.run("fx.chain.load", R"({"track":"tr_2","name":"Tone"})");
    CHECK(f.history() == beforeLoad + 1);
    REQUIRE(loaded["effects"].size() == 2);
    const auto& target = f.app->head().tracks.find(*model::Id::parse("tr_2"))->chain;
    REQUIRE(target.size() == 2);
    const auto& first = *f.app->head().fx.find(target[0]);
    const auto& second = *f.app->head().fx.find(target[1]);
    CHECK(first.plugin.id == "com.suppleaudio.test.gain");
    CHECK(second.plugin.id == "com.suppleaudio.test.edge");
    CHECK(first.label == "Quiet");
    CHECK(first.bypass);
    CHECK(first.wet == 37);
    CHECK(first.isolation == model::Isolation::InProcess);
    CHECK(first.oversampling == 4);
    CHECK(second.offline);
    CHECK(second.wet == 82);
    REQUIRE(second.state);
    CHECK(*second.state ==
          "sha256:df3f619804a92fdb4057192dc43dd748ea778adc52bc498ce80524c014b81119");
    REQUIRE(f.app->head().blobs.contains(*second.state));
    CHECK(f.app->head().blobs.at(*second.state)->data == std::vector<std::uint8_t>(4, 0));
    const auto sourcePatch = f.run("fx.patch.get", R"({"fx":"fx_1"})");
    const auto targetPatch = f.run("fx.patch.get", "{\"fx\":\"" + target[0].str() + "\"}");
    CHECK(sourcePatch == targetPatch);
    f.run("history.undo");
    CHECK(f.app->head().tracks.find(*model::Id::parse("tr_2"))->chain.empty());
    f.run("history.redo");
    CHECK(f.app->head().tracks.find(*model::Id::parse("tr_2"))->chain.size() == 2);
    f.run("fx.chain.save", R"({"track":"tr_2","name":"Copy"})");
    auto copy = f.document("Copy");
    CHECK(copy["effects"] == doc["effects"]);
    f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone","replace":true})");
    CHECK(readTextFile(saved["path"].asString()).value() == *text);
}

TEST_CASE("FX chain replace removes envelopes and routes and restores them on undo") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    f.run("track.add", R"({"name":"Target"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain","chain":"input"})");
    f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone","chain":"input"})");
    f.run("fx.add", R"({"track":"tr_2","plugin":"Test Key Mix"})");
    f.run("route.add", R"({"from":"tr_1","fx":"fx_2"})");
    f.run("env.add", R"({"route":"sd_1","param":"volume"})");
    const auto before = f.history();
    auto loaded = f.run("fx.chain.load", R"({"track":"tr_2","name":"Tone","mode":"replace"})");
    CHECK(f.history() == before + 1);
    const auto& track = *f.app->head().tracks.find(*model::Id::parse("tr_2"));
    CHECK(track.inputChain.empty());
    REQUIRE(track.chain.size() == 1);
    CHECK(track.chain[0].str() == loaded["effects"].at(0).asString());
    CHECK_FALSE(f.app->head().fx.find(*model::Id::parse("fx_2")));
    CHECK(f.app->head().routes.empty());
    CHECK(f.app->head().envelopes.empty());
    f.run("history.undo");
    CHECK(f.app->head().fx.find(*model::Id::parse("fx_2")));
    CHECK(f.app->head().routes.size() == 1);
    CHECK(f.app->head().envelopes.size() == 1);
    f.run("fx.add", R"({"track":"tr_2","plugin":"Test Gain","chain":"input"})");
    const auto path = json::writeCompact(json::Value((f.dir / "chains/Tone.json").string()));
    f.run("fx.chain.load", "{\"track\":\"tr_2\",\"path\":" + path + ",\"chain\":\"input\"}");
    CHECK(f.app->head().tracks.find(*model::Id::parse("tr_2"))->inputChain.size() == 2);
}

TEST_CASE("FX chain presets reject invalid names, overwrites and invalid loads atomically") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    auto empty = f.bus->execute({"fx.chain.save", j(R"({"track":"tr_1","name":"Tone"})"), kOrigin});
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == "chain.empty");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Gain"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Edge"})");
    f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone"})");
    auto original = f.document();
    for (const char* name : {"../Tone", "sub/Tone", "sub\\Tone", ".", ".."}) {
        auto args = json::Value::object();
        args["track"] = "tr_1";
        args["name"] = name;
        auto result = f.bus->execute({"fx.chain.save", args, kOrigin});
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "chain.name");
    }
    auto overwrite =
        f.bus->execute({"fx.chain.save", j(R"({"track":"tr_1","name":"Tone"})"), kOrigin});
    REQUIRE_FALSE(overwrite);
    CHECK(overwrite.error().code == "chain.exists");
    CHECK(f.document() == original);
    const auto before = f.history();
    auto bad = original;
    bad["effects"].asArray()[1]["plugin"]["id"] = "not-installed";
    bad["effects"].asArray()[1]["plugin"]["name"] = "Missing Space Echo";
    f.write(bad);
    auto missing = f.bus->execute(
        {"fx.chain.load", j(R"({"track":"tr_1","name":"Tone","mode":"replace"})"), kOrigin});
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "chain.plugin_missing");
    CHECK(missing.error().message.find("Missing Space Echo") != std::string::npos);
    CHECK(f.history() == before);
    CHECK(f.app->head().tracks.find(*model::Id::parse("tr_1"))->chain.size() == 2);
    bad = original;
    bad["effects"].asArray()[1]["state"] = "not base64";
    f.write(bad);
    auto invalid = f.bus->execute(
        {"fx.chain.load", j(R"({"track":"tr_1","name":"Tone","mode":"replace"})"), kOrigin});
    REQUIRE_FALSE(invalid);
    CHECK(f.history() == before);
    CHECK(f.app->head().fx.size() == 2);
    auto ambiguous = f.bus->execute(
        {"fx.chain.load", j(R"({"track":"tr_1","name":"Tone","path":"Tone.json"})"), kOrigin});
    REQUIRE_FALSE(ambiguous);
    CHECK(ambiguous.error().code == "chain.source");
    REQUIRE(writeFileAtomic(f.dir / "chains/broken.json", "{}"));
    auto listed = f.run("fx.chain.list");
    CHECK(listed["chains"].size() == 1);
}

TEST_CASE("FX chain opaque states have content hashes across SHA-256 block boundaries") {
    ChainFixture f;
    f.run("track.add", R"({"name":"Source"})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"Test Edge"})");
    f.run("fx.chain.save", R"({"track":"tr_1","name":"Tone"})");
    auto doc = f.document();
    // Standard SHA-256 vectors for bytes 0..n-1, independently checked with hashlib.
    for (const auto& [size, hash] : std::vector<std::pair<std::size_t, std::string>>{
             {55, "463eb28e72f82e0a96c0a4cc53690c571281131f672aa229e0d45ae59b598b59"},
             {56, "da2ae4d6b36748f2a318f23e7ab1dfdf45acdc9d049bd80e59de82a60895f562"},
             {64, "fdeab9acf3710362bd2658cdc9a29e8f9c757fcf9811603a8c447cd1d9151108"},
             {120, "f52b23db1fbb6ded89ef42a23ce0c8922c45f25c50b568a93bf1c075420bbb7c"}}) {
        std::vector<std::uint8_t> bytes(size);
        for (std::size_t i = 0; i < size; ++i) bytes[i] = static_cast<std::uint8_t>(i);
        doc["effects"].asArray()[0]["state"] = base64Encode(bytes);
        f.write(doc);
        auto loaded = f.run("fx.chain.load", R"({"track":"tr_1","name":"Tone","mode":"replace"})");
        auto id = model::Id::parse(loaded["effects"].at(0).asString());
        REQUIRE(id);
        const auto& fx = *f.app->head().fx.find(*id);
        REQUIRE(fx.state);
        CHECK(*fx.state == "sha256:" + hash);
        CHECK(f.app->head().blobs.at(*fx.state)->data == bytes);
    }
}

TEST_CASE("item.add hashes the file, reads its header, and adds one source per file") {
    Workdir dir;
    audio::AudioData source;
    source.sampleRate = 48000;
    source.channels.assign(2, std::vector<float>(48000));  // 1 s stereo
    for (std::size_t i = 0; i < source.frames(); ++i)
        source.channels[0][i] = source.channels[1][i] =
            static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000));
    const fs::path file = dir.path / "source.wav";
    REQUIRE(audio::writeWav(file, source).ok());
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Audio"})");
    const std::string add =
        "{\"track\":\"tr_1\",\"audio_file\":" + json::writeCompact(json::Value(file.string())) +
        "}";

    audio::resetDecodeCountForTests();
    const auto first = run(bus, "item.add", add);
    CHECK(first.find("source")->asString() == "md_1");
    for (int i = 0; i < 100; ++i) run(bus, "item.add", add);
    // One source for one file, and no decode to learn its metadata.
    CHECK(app->head().media.size() == 1);
    CHECK(app->head().items.size() == 101);
    CHECK(audio::decodeCountForTests() == 0);
    const auto* md = app->head().media.find(model::Id{model::Kind::Media, 1});
    REQUIRE(md != nullptr);
    CHECK(md->hash == sha256File(file).value());
    CHECK(md->sampleRate == 48000);
    CHECK(md->channels == 2);
    CHECK(md->frames == 48000);
    CHECK(md->format == "wav");
    // The item's length came from the header (1 s at 120 bpm = 2 beats), and
    // every add used the same source.
    const auto* item = app->head().items.find(model::Id{model::Kind::Item, 1});
    REQUIRE(item != nullptr);
    CHECK(item->length == doctest::Approx(2.0));

    // The hash survives a save/open round trip: the schema accepts the hashed
    // form (and the empty form old projects carry — see the fuzz corpus).
    const std::string save =
        "{\"path\":" + json::writeCompact(json::Value((dir.path / "project.json").string())) + "}";
    run(bus, "project.save", save);
    auto reopened = makeApp(dir.path / "reopened");
    REQUIRE(reopened->open(dir.path / "project.json").ok());
    const auto* again = reopened->head().media.find(model::Id{model::Kind::Media, 1});
    REQUIRE(again != nullptr);
    CHECK(again->hash == md->hash);
}

TEST_CASE("item.add reuses a pre-hash source for the same file and backfills its hash") {
    Workdir dir;
    audio::AudioData source;
    source.sampleRate = 48000;
    source.channels.assign(2, std::vector<float>(48000));  // 1 s stereo
    for (std::size_t i = 0; i < source.frames(); ++i)
        source.channels[0][i] = source.channels[1][i] =
            static_cast<float>(0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000));
    const fs::path file = dir.path / "source.wav";
    REQUIRE(audio::writeWav(file, source).ok());
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Audio"})");
    const std::string add =
        "{\"track\":\"tr_1\",\"audio_file\":" + json::writeCompact(json::Value(file.string())) +
        "}";
    run(bus, "item.add", add);

    // A project saved before MediaSource::hash existed: the source keeps the
    // file's path and header, and has no hash.
    auto doc = model::projectToJson(app->head());
    doc["schema"] = "supple.project/1.0";
    doc["sources"]["media"]["md_1"]["hash"] = "";
    REQUIRE(writeFileAtomic(dir.path / "legacy.json", json::writeCanonical(doc)).ok());
    REQUIRE(app->open(dir.path / "legacy.json").ok());
    REQUIRE(app->head().media.size() == 1);
    const auto media = [&] { return app->head().media.find(model::Id{model::Kind::Media, 1}); };
    REQUIRE(media() != nullptr);
    CHECK(media()->hash.empty());

    // Adding the same file again converges on that source and fills its hash,
    // instead of growing a second source for one file.
    const auto again = run(bus, "item.add", add);
    CHECK(again.find("source")->asString() == "md_1");
    CHECK(app->head().media.size() == 1);
    CHECK(app->head().items.size() == 2);  // the item is still added
    CHECK(media()->hash == sha256File(file).value());

    // A different file of the same length is not conflated with it: the path
    // has to be the same file, not just the same shape.
    audio::AudioData other = source;
    for (float& v : other.channels[0]) v *= 0.5f;
    const fs::path otherFile = dir.path / "other.wav";
    REQUIRE(audio::writeWav(otherFile, other).ok());
    const auto third = run(bus, "item.add",
                           "{\"track\":\"tr_1\",\"audio_file\":" +
                               json::writeCompact(json::Value(otherFile.string())) + "}");
    CHECK(third.find("source")->asString() == "md_2");
    CHECK(app->head().media.size() == 2);
}

TEST_CASE("timebase: items keep their place when it changes, and follow it when the tempo does") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"A"})");  // tr_1
    run(bus, "track.add", R"({"name":"B"})");  // tr_2
    // At 120 BPM: beat 4 = 2 s, two beats long = 1 s.
    run(bus, "item.add", R"({"track":"tr_1","position":4,"length":2,"notation":"1: C4:h r:h"})");
    run(bus, "item.add", R"({"track":"tr_1","position":8,"length":2,"notation":"1: C4:h r:h"})");
    const model::Id a = *model::Id::parse("it_1"), b = *model::Id::parse("it_2");
    auto span = [&](model::Id id) {
        return model::itemSpan(app->head(), *app->head().items.find(id));
    };
    // it_1 follows time from now on: the model holds seconds, nothing moves.
    run(bus, "item.set", R"({"item":"it_1","timebase":"time"})");
    CHECK(app->head().items.find(a)->position == doctest::Approx(2.0));
    CHECK(app->head().items.find(a)->length == doctest::Approx(1.0));
    CHECK(span(a).startBeat == doctest::Approx(4.0));
    // Positions given in beats land in its domain.
    run(bus, "item.set", R"({"item":"it_1","position":6})");
    CHECK(app->head().items.find(a)->position == doctest::Approx(3.0));
    CHECK(app->head().items.find(a)->length == doctest::Approx(1.0));  // seconds kept
    run(bus, "item.set", R"({"item":"it_1","length":4})");
    CHECK(app->head().items.find(a)->length == doctest::Approx(2.0));
    // Half the tempo: the time item stays at 3 s, the beats item doubles its time.
    run(bus, "tempo.set", R"({"points":[{"beat":0,"bpm":60}]})");
    CHECK(span(a).startSec == doctest::Approx(3.0));
    CHECK(span(a).endSec == doctest::Approx(5.0));
    CHECK(span(b).startSec == doctest::Approx(8.0));
    CHECK(span(b).endSec == doctest::Approx(10.0));
    // Moved to B by beat, it keeps its seconds length.
    run(bus, "item.move", R"({"item":"it_1","track":"tr_2","position":1})");
    CHECK(span(a).startSec == doctest::Approx(1.0));
    CHECK(span(a).endSec == doctest::Approx(3.0));
    // A track's timebase converts its inheriting items, keeping them in place.
    run(bus, "track.set", R"({"track":"tr_1","timebase":"time"})");
    CHECK(app->head().items.find(b)->position == doctest::Approx(8.0));  // seconds now
    run(bus, "tempo.set", R"({"points":[{"beat":0,"bpm":120}]})");
    CHECK(span(b).startSec == doctest::Approx(8.0));
    CHECK(span(b).endSec == doctest::Approx(10.0));
    // beats_position: the start follows the beat, the length stays in seconds.
    run(bus, "item.set", R"({"item":"it_2","timebase":"beats_position"})");
    CHECK(app->head().items.find(b)->position == doctest::Approx(16.0));  // beat at 8 s
    run(bus, "tempo.set", R"({"points":[{"beat":0,"bpm":60}]})");
    CHECK(span(b).startSec == doctest::Approx(16.0));
    CHECK(span(b).endSec - span(b).startSec == doctest::Approx(2.0));
    // The project's timebase converts what inherits it (items on tracks that
    // follow the project); a duplicate lands right after the original.
    run(bus, "item.add", R"({"track":"tr_2","position":2,"length":1,"notation":"1: C4:q r:*"})");
    const model::Id c = *model::Id::parse("it_3");
    run(bus, "project.set", R"({"timebase":"time"})");
    CHECK(app->head().project->timebase == model::Timebase::Time);
    CHECK(app->head().items.find(c)->position == doctest::Approx(2.0));  // beat 2 at 60 BPM
    const auto dup = run(bus, "item.duplicate", R"({"item":"it_3"})");
    const model::Id d = *model::Id::parse(dup.find("id")->asString());
    CHECK(span(d).startSec == doctest::Approx(span(c).endSec));
    // The describe text says which items do not follow beats.
    const auto text = run(bus, "session.describe", "{}");
    CHECK(json::writeCompact(text).find("[time]") != std::string::npos);
    // One undo step undoes the project change and its conversions together.
    run(bus, "history.undo", "{}");
    run(bus, "history.undo", "{}");
    CHECK(app->head().project->timebase == model::Timebase::Beats);
    CHECK(app->head().items.find(c)->position == doctest::Approx(2.0));
}

TEST_CASE("renders: FLAC files, a secondary format, and a queue pinned to its versions") {
    Workdir dir;
    auto app = makeApp(dir.path);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, *app);
    run(bus, "track.add", R"({"name":"Synth"})");
    run(bus, "fx.add", R"({"track":"tr_1","plugin":"Test Sine"})");
    run(bus, "item.add", R"({"track":"tr_1","position":0,"length":4,"notation":"1: C4:w"})");
    // A FLAC mix with a 16-bit WAV beside it.
    const auto mix = run(bus, "render.mix",
                         R"({"name":"song","end_beat":2,"format":"flac","bit_depth":24,
                             "secondary":{"format":"wav","bit_depth":16}})");
    const fs::path flac = mix.find("path")->asString();
    const fs::path wav = mix.find("secondary_path")->asString();
    CHECK(flac.extension() == ".flac");
    CHECK(wav.filename() == "song.wav");
    auto a = audio::readAudio(flac);
    auto b = audio::readAudio(wav);
    REQUIRE(a);
    REQUIRE(b);
    CHECK(audio::inspect(flac)->container == audio::Container::Flac);
    CHECK(a->frames() == b->frames());
    CHECK(peak(flac) == doctest::Approx(peak(wav)).epsilon(1e-3));
    CHECK(peak(flac) > 0.05);
    // Stems as FLAC; a same-container secondary carries its depth in the name.
    const auto stems = run(bus, "render.stems",
                           R"({"pattern":"stem-{track}","end_beat":2,"format":"flac","bit_depth":16,
                               "secondary":{"format":"flac","bit_depth":24}})");
    const auto& file = stems.find("files")->at(0);
    CHECK(fs::path(file.find("path")->asString()).filename() == "stem-Synth.flac");
    CHECK(fs::path(file.find("secondary_path")->asString()).filename() == "stem-Synth.24.flac");
    CHECK(fs::exists(file.find("secondary_path")->asString()));
    // FLAC is 16 or 24 bit.
    auto refused = bus.execute({"render.mix", j(R"({"format":"flac","bit_depth":32})"), kOrigin});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "render.format");
    // The queue: q1 now, then the track 12 dB down and q2; q1 still renders loud.
    const auto q1 = run(bus, "render.queue.add",
                        R"({"command":"render.mix","args":{"name":"before","end_beat":2}})");
    CHECK(q1.find("id")->asString() == "q1");
    run(bus, "track.set", R"({"track":"tr_1","volume_db":-12})");
    run(bus, "render.queue.add",
        R"({"command":"render.stems","args":{"pattern":"after-{track}","end_beat":2},"label":"stems"})");
    CHECK(run(bus, "render.queue.list", "{}").find("queue")->size() == 2);
    const auto ran = run(bus, "render.queue.run", "{}");
    REQUIRE(ran.find("renders")->size() == 2);
    const fs::path before =
        ran.find("renders")->at(0).find("files")->at(0).find("path")->asString();
    const fs::path after = ran.find("renders")->at(1).find("files")->at(0).find("path")->asString();
    CHECK(before.filename() == "before.wav");
    CHECK(ran.find("renders")->at(1).find("label")->asString() == "stems");
    CHECK(peak(before) / peak(after) == doctest::Approx(std::pow(10.0, 12.0 / 20.0)).epsilon(0.02));
    CHECK(run(bus, "render.queue.list", "{}").find("queue")->size() == 0);  // done: off the queue
    // Remove and clear.
    run(bus, "render.queue.add", R"({"command":"render.mix","args":{"end_beat":1}})");
    run(bus, "render.queue.add", R"({"command":"render.mix","args":{"end_beat":1,"name":"x"}})");
    run(bus, "render.queue.remove", R"({"id":"q3"})");
    CHECK(run(bus, "render.queue.list", "{}").find("queue")->size() == 1);
    CHECK(run(bus, "render.queue.remove", R"({"all":true})").find("removed")->asInt() == 1);
    CHECK(bus.execute({"render.queue.run", j("{}"), kOrigin}).error().code == "render.queue_empty");
}
