#include <doctest/doctest.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>

#include <supple/engine/app.h>
#include <supple/model/schemas.h>

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
