#include <doctest/doctest.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>
#include <fstream>

#include <supple/base/files.h>
#include <supple/sandbox/commands.h>

using namespace supple;
using namespace supple::sandbox;
namespace fs = std::filesystem;

namespace {

const fs::path kFixture = FIXTURE_CLAP_PATH;

struct Root {
    fs::path path =
        fs::temp_directory_path() / ("supple-sandbox-cmd-" + std::to_string(::getpid()));
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

std::unique_ptr<SandboxHost> makeHost(const fs::path& dir, fs::path log = {}) {
    SessionOptions o;
    o.workdir = dir;
    o.pluginPath = {kFixture};
    auto s = Session::open(o);
    REQUIRE(s);
    return std::make_unique<SandboxHost>(std::move(*s), std::move(log));
}

json::Value j(std::string_view text) { return json::parse(text).value(); }

const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

}  // namespace

TEST_CASE("every sandbox command example runs against its fixture" * doctest::description("rt")) {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    int n = 0;
    auto st = cmd::runExamples(registry, [&](std::string_view) -> std::unique_ptr<cmd::Host> {
        return makeHost(root.path / std::to_string(n++));
    });
    INFO((st ? std::string() : st.error().toString()));
    CHECK(st.ok());
    CHECK(n >= 16);
}

TEST_CASE("an LLM-style session: load, program, render, batch with references") {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    auto host = makeHost(root.path / "s", root.path / "commands.ndjson");
    host->setFixedClock(1000);
    cmd::Dispatcher d(registry, *host);
    auto call = [&](std::string command, std::string_view args) {
        return d.execute({std::move(command), j(args), kTest});
    };

    auto load = call("plugin.load", R"({"plugin": "com.suppleaudio.test.sine"})");
    REQUIRE(load);
    CHECK((*load)["instance"].asString() == "in_1");

    // One atomic batch: add the effect, set it with a reference to its id, render.
    auto batch = call("batch", R"({
        "label": "quieter phrase",
        "steps": [
          {"command": "plugin.load", "args": {"plugin": "com.suppleaudio.test.gain"}, "as": "fx"},
          {"command": "param.set", "args": {"instance": "${fx.instance}", "values": {"gain": "-6 dB"}}},
          {"command": "render", "args": {"notation": "voice lead; 1: A4:q r:q r:h", "name": "take 1"}, "as": "r1"}
        ]})");
    INFO((batch ? std::string()
                : batch.error().toString() + " / " +
                      (batch.error().details.empty() ? std::string()
                                                     : batch.error().details[0].toString())));
    REQUIRE(batch);
    json::Value set = (*batch)["results"].at(1), rendered = (*batch)["results"].at(2);
    CHECK(set["values"]["gain"]["value"].asDouble() == doctest::Approx(-6.0));
    CHECK(fs::exists(rendered["path"].asString()));
    // Notation's default velocity 96 -> 0.25 * 96/127, then -6 dB.
    CHECK(rendered["peak_dbfs"].asDouble() ==
          doctest::Approx(20 * std::log10(0.25 * 96 / 127) - 6).epsilon(0.001));

    // A failing batch rolls back everything, including its render.
    auto failed = call("batch", R"({
        "steps": [
          {"command": "render", "args": {"notation": "voice lead; 1: A4:h r:h"}, "as": "r2"},
          {"command": "param.set", "args": {"instance": "in_2", "values": {"gian": 1}}}
        ]})");
    REQUIRE(!failed);
    CHECK(failed.error().code == "batch.step_failed");
    REQUIRE(failed.error().details.size() == 1);
    CHECK(failed.error().details[0].path == "/steps/1/args/values/gian");
    auto session = call("session.get", "{}");
    REQUIRE(session);
    CHECK((*session)["renders"].size() == 1);
    CHECK(!fs::exists(root.path / "s" / "renders" / "rd_2.wav"));

    // Errors carry argument paths and, for notation, a text location.
    auto bad =
        call("render", R"({"notation": "voice lead; 1: C4:q D4:q"})");  // bar 1 does not add up
    REQUIRE(!bad);
    CHECK(bad.error().code.rfind("notation.", 0) == 0);
    CHECK(bad.error().path == "/args/notation");
    CHECK(bad.error().loc.has_value());
    auto unknown = call("param.get", R"({"instance": "in_9"})");
    REQUIRE(!unknown);
    CHECK(unknown.error().code == "sandbox.unknown_instance");
    CHECK(unknown.error().path == "/args/instance");
    auto wrongArgs = call("render", R"({"notation": 5})");
    REQUIRE(!wrongArgs);
    CHECK(wrongArgs.error().code == "command.invalid_args");

    // The command log has one NDJSON line per record, batch steps included.
    auto text = readTextFile(root.path / "commands.ndjson");
    REQUIRE(text);
    std::size_t lines = 0;
    for (char ch : *text) lines += ch == '\n';
    CHECK(lines == host->records().size());
    CHECK(lines >= 10);
    auto first = json::parse(text->substr(0, text->find('\n')));
    REQUIRE(first);
    CHECK((*first)["command"].asString() == "plugin.load");
    CHECK((*first)["time_ms"].asInt() == 1000);
}

TEST_CASE("session.set validates, render uses notation tempo and bars") {
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    auto host = makeHost(root.path / "s");
    cmd::Dispatcher d(registry, *host);
    auto call = [&](std::string command, std::string_view args) {
        return d.execute({std::move(command), j(args), kTest});
    };
    REQUIRE(call("plugin.load", R"({"plugin": "com.suppleaudio.test.sine"})"));
    auto bad = call("session.set", R"({"meter": "99999999999/4"})");
    REQUIRE(!bad);
    CHECK(bad.error().path == "/args/meter");
    CHECK(call("session.set", R"({"sample_rate": "44.1 kHz"})"));
    auto r =
        call("render",
             R"({"notation": "voice lead; 1: A4:q r:q r:h", "bars": 2, "tempo": 60, "tail_s": 0})");
    INFO((r ? std::string() : r.error().toString()));
    REQUIRE(r);
    CHECK((*r)["seconds"].asDouble() == doctest::Approx(8.0));  // 2 bars of 4/4 at 60 BPM
    CHECK(host->session().renders().at("rd_1").sampleRate == 44100.0);
}

TEST_CASE("registry exports are deterministic and name every sandbox tool") {
    cmd::Registry a, b;
    registerCommands(a);
    registerCommands(b);
    CHECK(json::writeCanonical(a.exportJson()) == json::writeCanonical(b.exportJson()));
    const auto tools = cmd::mcpToolList(a);
    std::string names = json::writeCompact(tools);
    for (const char* t : {"plugin_load", "param_set", "patch_merge", "render", "session_get"})
        CHECK(names.find(t) != std::string::npos);
}
