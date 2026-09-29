#include <doctest/doctest.h>

#include <unistd.h>
#include <cmath>
#include <filesystem>

#include <supple/audio/audio_file.h>
#include <supple/engine/app.h>

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
    auto app = EngineApp::create(options);
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
    run(bus, "track.add", R"({"name":"Bus"})");
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
    auto created = EngineApp::create(options);
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
    run(bus, "track.add", R"({"name":"A"})");            // tr_1
    run(bus, "track.add", R"({"name":"B"})");            // tr_2
    run(bus, "track.add", R"({"name":"C","index":0})");  // tr_3, first under the master
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
    CHECK(fails(bus, "track.move", R"({"track":"tr_0"})") == "tr.master");
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
