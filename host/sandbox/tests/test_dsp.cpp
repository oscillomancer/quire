// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The DSP script harness (dsp.test) through the real script player: what it
// measures is what the script does, and what goes wrong is said in words.
// And dsp.export: a copy of the player that plays one script anywhere.
#include <doctest/doctest.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <sstream>
#include <string>

#include "supple/audio/audio_file.h"
#include "supple/sandbox/commands.h"

using namespace supple;
using namespace supple::sandbox;
namespace fs = std::filesystem;

namespace {

const fs::path kStock = STOCK_CLAP_DIR;
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

struct Root {
    fs::path path =
        fs::temp_directory_path() / ("supple-sandbox-dsp-" + std::to_string(::getpid()));
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

std::string script(const std::string& name, const std::string& id, const std::string& body) {
    return "#include <supple/script.h>\n\nstruct " + name +
           " : Effect {\n    static constexpr Info info = {.name = \"" + name + "\", .id = \"" +
           id + "\"};\n" + body + "\n};\n";
}

// New harness cases share the real player and the small host fixtures.
struct Harness {
    Root root;
    cmd::Registry registry;
    std::unique_ptr<SandboxHost> host;
    std::unique_ptr<cmd::Dispatcher> bus;

    Harness() {
        SessionOptions options;
        options.workdir = root.path;
        options.pluginPath = {fs::path(FIXTURE_CLAP_PATH), kStock / "Script Player.clap"};
        auto session = Session::open(options);
        REQUIRE(session);
        host = std::make_unique<SandboxHost>(std::move(*session), fs::path{});
        registerCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *host);
    }
    Result<json::Value> test(std::string_view args) {
        auto parsed = json::parse(args);
        REQUIRE(parsed);
        return bus->execute({"dsp.test", *parsed, kTest});
    }
    json::Value measured(std::string_view args) {
        auto result = test(args);
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    void write(const std::string& name, const std::string& source) {
        auto args = json::Value::object();
        args["name"] = name;
        args["source"] = source;
        auto result = bus->execute({"dsp.write", args, kTest});
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        REQUIRE_MESSAGE((*result)["ok"].asBool(), json::writeCompact((*result)["diagnostics"]));
    }
};

}  // namespace

TEST_CASE("DSP render commands locate invalid patch values inside the patch argument") {
    Harness h;
    h.write("gain",
            script("Gain", "com.example.patch-errors",
                   "Param gain = decibels(\"Gain\", -24, 24, 0); void process(Block& io) {}"));
    for (const std::string command : {"dsp.test", "dsp.preview"}) {
        if (command == "dsp.preview" && !SUPPLE_EDITORS) continue;
        auto args = json::parse(R"({"script":"gain","patch":{"params":{"gain":1000}}})").value();
        auto result = h.bus->execute({command, args, kTest});
        REQUIRE_FALSE(result);
        CHECK(result.error().path == "/args/patch/params/gain");
        CHECK_FALSE(result.error().code.empty());
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().hint.empty());
    }
}

TEST_CASE("two scripts with one id are refused: the player would play only one") {
    Harness h;
    const std::string body = "    void process(Block& io) {}";
    h.write("first", script("First", "com.example.twin", body));
    auto args = json::Value::object();
    args["name"] = "second";
    args["source"] = script("Second", "com.example.twin", body);
    auto refused = h.bus->execute({"dsp.write", args, kTest});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "dsp.id_taken");
    CHECK(refused.error().path == "/args/source");
    // One saved some other way is refused by name too.
    auto list = h.bus->execute({"dsp.list", json::Value::object(), kTest});
    REQUIRE(list);
    const fs::path library((*list)["library"].asString());
    CHECK_FALSE(fs::exists(library / "second"));
    fs::create_directories(library / "second");
    std::ofstream(library / "second" / "second.cpp") << args["source"].asString();
    auto tested = h.test(R"({"script":"second","signals":["sine"],"seconds":0.1})");
    REQUIRE_FALSE(tested);
    CHECK(tested.error().code == "dsp.id_taken");
    CHECK(tested.error().path == "/args/script");
}

TEST_CASE("a source export's tests move a parameter whose name survives CMake and C") {
    Harness h;
    auto exportSource = [&](const std::string& params) {
        h.write("named",
                script("Named", "com.example.named", params + "    void process(Block& io) {}"));
        auto args = json::Value::object();
        args["script"] = "named";
        args["format"] = "source";
        args["folder"] = (h.root.path / "named-source").string();
        return h.bus->execute({"dsp.export", args, kTest});
    };
    auto refused = exportSource("    Param gain = decibels(\"Out \\\"dB\\\"\", -24, 24, 0);\n");
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "dsp.no_parameter");
    CHECK(refused.error().path == "/args/script");
}

TEST_CASE("dsp.test automation uses physical values and detects abrupt steps") {
    Harness h;
    // The command's value reaches the renderer at sample 12012, not the next block.
    auto value = h.measured(R"({"plugin":"com.suppleaudio.test.gain","signals":["sine"],
        "save":true,"frequency_hz":1000,"automation":[{"param":"gain","at_s":0.25025,"value":-6}]})");
    auto audio = audio::readAudio(value["signals"].at(0).find("path")->asString());
    REQUIRE(audio);
    const auto& x = audio->channels[0];
    const double amplitude = std::pow(10.0, -12.0 / 20.0);
    for (std::size_t i : {12011u, 12012u}) {
        const double input = amplitude * std::sin(2.0 * std::numbers::pi * 1000.0 *
                                                  static_cast<double>(i) / 48000.0);
        CHECK(
            static_cast<double>(x[i]) ==
            doctest::Approx(input * (i < 12012 ? 1.0 : std::pow(10.0, -6.0 / 20.0))).epsilon(1e-5));
    }
    CHECK_FALSE(value["signals"].at(0).find("thd_percent"));
    for (const std::string name : {"abrupt", "smooth"}) {
        h.write(name, script("Gain", "com.example." + name,
                             "Param gain = decibels(\"Gain\", -60, 0, -60)" +
                                 std::string(name == "smooth" ? ".smoothed(20)" : "") + R"(;
            void process(Block& io) {
                for (int c = 0; c < io.channels; ++c)
                    for (int i = 0; i < io.frames; ++i) io[c][i] *= dbToGain(gain.at(i));
            })"));
    }
    auto abrupt = h.measured(R"({"script":"abrupt","signals":["sine","sine"],"frequency_hz":100,
        "automation":[{"param":"gain","at_s":0.2525,"value":0}]})");
    auto smooth = h.measured(R"({"script":"smooth","signals":["sine"],"frequency_hz":100,
        "automation":[{"param":"gain","at_s":0.2525,"value":0}]})");
    const auto& step = *abrupt["signals"].at(0).find("automation");
    CHECK(step.find("max_step")->asDouble() > 0.2);
    const auto& smoothedStep = *smooth["signals"].at(0).find("automation");
    CHECK(smoothedStep.find("max_step")->asDouble() < step.find("max_step")->asDouble() * 0.1);
    CHECK(abrupt["problems"].at(0).asString().find("zipper noise") != std::string::npos);
    CHECK(abrupt["signals"].at(0) == abrupt["signals"].at(1));  // a fresh initial value per pass
}

TEST_CASE(
    "dsp.test compressor ducks under a timed key, measures a silent reference, and keys from its "
    "input without one") {
    Harness h;
    std::ifstream in(std::string(SUPPLE_SCRIPT_EXAMPLES) + "/compressor.cpp");
    std::stringstream source;
    source << in.rdbuf();
    h.write("compressor", source.str());
    auto active = h.measured(R"({"script":"compressor","signals":["sine"],"level_db":-30,
        "patch":{"params":{"attack":1}},"sidechain":{"level_db":-6}})");
    const auto& duck = *active["signals"].at(0).find("sidechain");
    CHECK(duck.find("gain_change_db")->asDouble() < -10.0);
    CHECK(duck.find("reference_rms_dbfs")->asDouble() == doctest::Approx(-33.01).epsilon(0.001));
    CHECK_FALSE(active["signals"].at(0).find("thd_percent"));
    auto silent = h.measured(R"({"script":"compressor","signals":["sine","silence"],
        "level_db":-30,"sidechain":{"signal":"silence"}})");
    const auto& silentSine = *silent["signals"].at(0).find("sidechain");
    const auto& silentMain = *silent["signals"].at(1).find("sidechain");
    CHECK(silentSine.find("gain_change_db")->asDouble() == 0.0);
    CHECK(silentMain.find("gain_change_db")->isNull());
    // With no key routed, the key port is deactivated and the compressor
    // listens to its own input: a loud sine is turned down.
    auto own = h.measured(R"({"script":"compressor","signals":["sine"],"level_db":-6,
        "patch":{"params":{"attack":1}}})");
    CHECK(own["signals"].at(0).find("gain_db")->asDouble() < -5.0);
}

TEST_CASE("dsp.test custom notes keep exact timing, velocity and their own final release") {
    Harness h;
    h.write("gates", R"(#include <supple/script.h>
        struct Gates : Instrument {
            static constexpr Info info = {.name="Gates", .id="com.example.gates", .voices=4};
            Array<int, 4> remaining;
            void startVoice(int v) { remaining[v] = -1; }
            void releaseVoice(int v) { remaining[v] = 240; }
            void process(Block& io) {
                for (int v = 0; v < voiceCount(); ++v) {
                    if (!voicePlaying(v)) continue;
                    for (int i = 0; i < io.frames; ++i) {
                        if (remaining[v] == 0) { voiceFinished(v); break; }
                        for (int c = 0; c < io.channels; ++c) io[c][i] += 0.1 * voiceVelocity(v);
                        if (remaining[v] > 0) --remaining[v];
                    }
                }
            }
        };)");
    auto result = h.measured(R"({"script":"gates","save":true,
        "notes":[{"key":60,"velocity":0.4,"start_s":0.013,"length_s":0.05},
                 {"key":60,"velocity":0.8,"start_s":0.043,"length_s":0.06}]})");
    REQUIRE(result["signals"].size() == 2);
    const auto& notes = result["signals"].at(0);
    CHECK(notes.find("signal")->asString() == "notes");
    CHECK_FALSE(notes.find("pitch_hz"));
    CHECK(notes.find("release_ms")->asDouble() == 5.0);
    auto audio = audio::readAudio(notes.find("path")->asString());
    REQUIRE(audio);
    const auto& x = audio->channels[0];
    CHECK(x[623] == 0.0f);
    CHECK(static_cast<double>(x[624]) == doctest::Approx(0.04));
    CHECK(static_cast<double>(x[2064]) == doctest::Approx(0.12));
    CHECK(static_cast<double>(x[3263]) == doctest::Approx(0.12));
    CHECK(static_cast<double>(x[3264]) == doctest::Approx(0.08));
    CHECK(static_cast<double>(x[5183]) == doctest::Approx(0.08));
    CHECK(x[5184] == 0.0f);
    CHECK(result["signals"].at(1).find("peak_dbfs")->asDouble() == -200.0);
}

TEST_CASE("dsp.test rejects invalid automation, key and custom-note arguments at their paths") {
    Harness h;
    auto check = [&](std::string_view args, std::string_view path) {
        auto result = h.test(args);
        REQUIRE_FALSE(result);
        if (result.error().code == "command.invalid_args") {
            REQUIRE(result.error().details.size() == 1);
            CHECK(result.error().details[0].path == "/args" + std::string(path));
        } else {
            CHECK(result.error().path == "/args" + std::string(path));
        }
        CHECK_FALSE(result.error().code.empty());
        CHECK_FALSE(result.error().message.empty());
        CHECK_FALSE(result.error().hint.empty());
    };
    check(
        R"({"plugin":"com.suppleaudio.test.gain","automation":[{"param":"missing","at_s":0.2,"value":0}]})",
        "/automation/0/param");
    check(
        R"({"plugin":"com.suppleaudio.test.gain","automation":[{"param":"gain","at_s":0.2,"value":999}]})",
        "/automation/0/value");
    check(
        R"({"plugin":"com.suppleaudio.test.gain","automation":[{"param":"gain","at_s":1,"value":0}]})",
        "/automation/0/at_s");
    check(
        R"({"plugin":"com.suppleaudio.test.keymix","automation":[{"param":"gain","at_s":0.2,"value":0}]})",
        "/automation");
    h.write(
        "meter",
        script("Meter", "com.example.readonly",
               "Param level = decibels(\"Level\", -60, 0, 0).meter(); void process(Block& io) {}"));
    check(R"({"script":"meter","automation":[{"param":"level","at_s":0.2,"value":0}]})",
          "/automation/0/param");
    check(R"({"plugin":"com.suppleaudio.test.gain","sidechain":{}})", "/sidechain");
    check(R"({"plugin":"com.suppleaudio.test.keymix","sidechain":{"start_s":1}})",
          "/sidechain/start_s");
    check(R"({"plugin":"com.suppleaudio.test.keymix","sidechain":{"length_s":0.000000001}})",
          "/sidechain/length_s");
    check(
        R"({"plugin":"com.suppleaudio.test.sine","notes":[{"key":60,"start_s":0.2,"length_s":1}]})",
        "/notes/0/length_s");
    check(
        R"({"plugin":"com.suppleaudio.test.sine","notes":[{"key":128,"start_s":0.2,"length_s":0.1}]})",
        "/notes/0/key");
    check(
        R"({"plugin":"com.suppleaudio.test.sine","signals":["note"],"notes":[{"key":60,"start_s":0,"length_s":0.1}]})",
        "/notes");
    check(
        R"({"plugin":"com.suppleaudio.test.sine","signals":["silence"],"notes":[{"key":60,"start_s":0,"length_s":0.1}]})",
        "/notes");
    check(
        R"({"plugin":"com.suppleaudio.test.gain","notes":[{"key":60,"start_s":0,"length_s":0.1}]})",
        "/notes");
}

TEST_CASE("dsp.test measures a script through the player and names what goes wrong") {
    if (!fs::exists(kStock / "Script Player.clap")) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    SessionOptions o;
    o.workdir = root.path;
    o.pluginPath = {kStock};
    auto session = Session::open(o);
    REQUIRE(session);
    SandboxHost host(std::move(*session), {});
    cmd::Registry registry;
    registerCommands(registry);
    cmd::Dispatcher bus(registry, host);
    auto call = [&](std::string name, json::Value args) {
        auto r = bus.execute({std::move(name), std::move(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    auto write = [&](const std::string& name, const std::string& source) {
        json::Value a = json::Value::object();
        a["name"] = name;
        a["source"] = source;
        json::Value written = call("dsp.write", std::move(a));
        REQUIRE(written["ok"].asBool());
    };
    auto test = [&](const std::string& name) {
        json::Value a = json::Value::object();
        a["script"] = name;
        return call("dsp.test", std::move(a));
    };

    // +6 dB, clean: the numbers say so, and nothing is wrong.
    write("boost",
          script("Boost", "com.example.boost",
                 "    Param gain = decibels(\"Gain\", -12, 12, 6);\n"
                 "    void factoryPatches(Patches& p) { p.add(\"Unity\").set(gain, 0); }\n"
                 "    void process(Block& io) {\n"
                 "        for (int c = 0; c < io.channels; ++c)\n"
                 "            for (int i = 0; i < io.frames; ++i) io[c][i] *= gain.gain();\n"
                 "    }"));
    json::Value boost = test("boost");
    CHECK(boost["plugin"].asString() == "com.example.boost");
    std::string clean;
    for (const json::Value& p : boost["problems"].asArray()) clean += p.asString() + "\n";
    CHECK_MESSAGE(clean.empty(), clean);
    REQUIRE(boost["signals"].size() == 5);
    // Inspection addresses the loaded instance, independently of dsp.test.
    auto loaded = call("plugin.load", json::parse(R"({"plugin":"com.example.boost"})").value());
    json::Value inspect = json::Value::object();
    inspect["instance"] = loaded["instance"];
    auto described = call("plugin.describe", inspect);
    CHECK(described["status_available"].asBool());
    CHECK(described["problems"].size() == 0);
    auto factories = call(
        "preset.list", json::parse(R"({"plugin":"com.example.boost","source":"factory"})").value());
    REQUIRE(factories["presets"].size() == 1);
    CHECK(factories["presets"].at(0).find("read_only")->asBool());
    auto load = inspect;
    load["name"] = "Unity";
    load["source"] = "factory";
    call("preset.load", std::move(load));
    CHECK(call("patch.get", inspect)["patch"]["params"]["gain"].asDouble() == 0.0);
    const json::Value& sine = boost["signals"].at(0);
    CHECK(sine.find("signal")->asString() == "sine");
    CHECK(sine.find("gain_db")->asDouble() == doctest::Approx(6.0).epsilon(0.002));
    CHECK(sine.find("thd_percent")->asDouble() < 0.01);
    CHECK(sine.find("peak_dbfs")->asDouble() == doctest::Approx(-6.0).epsilon(0.01));
    const json::Value& impulse = boost["signals"].at(2);
    REQUIRE(impulse.find("response_db")->size() >= 9);
    for (const json::Value& point : impulse.find("response_db")->asArray())
        CHECK(point.find("db")->asDouble() == doctest::Approx(6.0).epsilon(0.01));
    CHECK(boost["signals"].at(4).find("peak_dbfs")->asDouble() < -150.0);

    // Sound from silence, a DC offset, and a NaN the framework silenced.
    write("broken", script("Broken", "com.example.broken",
                           "    void process(Block& io) {\n"
                           "        for (int c = 0; c < io.channels; ++c)\n"
                           "            for (int i = 0; i < io.frames; ++i)\n"
                           "                io[c][i] = io[c][i] > 0.2 ? log(0.0) : 0.5;\n"
                           "    }"));
    json::Value broken = test("broken");
    std::string problems;
    for (const json::Value& p : broken["problems"].asArray()) problems += p.asString() + "\n";
    INFO(problems);
    CHECK(problems.find("sound from silence") != std::string::npos);
    CHECK(problems.find("DC offset") != std::string::npos);
    CHECK(problems.find("NaN or infinity") != std::string::npos);
}

#if defined(__linux__)
// Binary script exports append to the Linux player; native bundle export is a later port.
TEST_CASE("dsp.export makes a plugin that plays its version of the script, and no other") {
    const fs::path player = kStock / "Script Player.clap";
    if (!fs::exists(player)) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    cmd::Registry registry;
    registerCommands(registry);
    struct Host {
        std::unique_ptr<SandboxHost> host;
        std::unique_ptr<cmd::Dispatcher> bus;
    };
    auto open = [&](const std::string& name, const fs::path& plugins) {
        SessionOptions o;
        o.workdir = root.path / name;
        o.pluginPath = {plugins};
        auto session = Session::open(o);
        REQUIRE(session);
        Host h{std::make_unique<SandboxHost>(std::move(*session), fs::path{}), nullptr};
        h.bus = std::make_unique<cmd::Dispatcher>(registry, *h.host);
        return h;
    };
    auto call = [&](Host& h, std::string name, json::Value args) {
        auto r = h.bus->execute({std::move(name), std::move(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    auto write = [&](Host& h, const std::string& name, const std::string& source) {
        json::Value a = json::Value::object();
        a["name"] = name;
        a["source"] = source;
        json::Value written = call(h, "dsp.write", std::move(a));
        REQUIRE_MESSAGE(written["ok"].asBool(), json::writeCompact(written["diagnostics"]));
    };
    auto exportTo = [&](Host& h, const fs::path& folder) {
        json::Value a = json::Value::object();
        a["script"] = "boost";
        a["folder"] = folder.string();
        return call(h, "dsp.export", std::move(a));
    };
    auto gainDb = [&](Host& h) {
        json::Value a = json::Value::object();
        a["plugin"] = "com.example.boost";
        a["signals"] = json::Value::array();
        a["signals"].push_back("sine");
        return call(h, "dsp.test", std::move(a))["signals"].at(0).find("gain_db")->asDouble();
    };
    auto boost = [](int db) {
        return script("Boost", "com.example.boost",
                      "    Param gain = decibels(\"Gain\", -24, 24, " + std::to_string(db) +
                          ");\n"
                          "    void process(Block& io) {\n"
                          "        for (int c = 0; c < io.channels; ++c)\n"
                          "            for (int i = 0; i < io.frames; ++i) io[c][i] *= "
                          "gain.gain();\n"
                          "    }");
    };

    // From the library's player: Boost.clap, with the notices to ship.
    Host library = open("library", player);
    write(library, "boost", boost(6));
    json::Value first = exportTo(library, root.path / "out");
    CHECK(first["path"].asString() == (root.path / "out" / "Boost.clap").string());
    CHECK(first["id"].asString() == "com.example.boost");
    CHECK(first["player"].asString() == player.string());
    CHECK(fs::file_size(first["path"].asString()) ==
          static_cast<std::uintmax_t>(first["bytes"].asInt()));
    CHECK(fs::exists(first["notices"].asString()));

    // The library changes and grows; the export keeps its own version and
    // offers nothing else.
    write(library, "boost", boost(12));
    write(library, "other", script("Other", "com.example.other", "    void process(Block& io) {}"));
    Host anywhere = open("anywhere", root.path / "out");
    auto listed = anywhere.host->session().plugins(true);
    REQUIRE(listed);
    REQUIRE(listed->size() == 1);
    CHECK(listed->front().id == "com.example.boost");
    CHECK(gainDb(anywhere) == doctest::Approx(6.0).epsilon(0.002));

    // An export exported again starts from the player, with the new script.
    write(anywhere, "boost", boost(12));
    json::Value second = exportTo(anywhere, root.path / "again");
    CHECK(second["player"].asString() == first["path"].asString());
    CHECK(second["bytes"].asInt() == first["bytes"].asInt() + 1);  // "12" for "6"
    CHECK(fs::file_size(second["notices"].asString()) ==
          fs::file_size(first["notices"].asString()));
    Host again = open("again", root.path / "again");
    CHECK(gainDb(again) == doctest::Approx(12.0).epsilon(0.002));

    // An installed copy missing its notices must not yield an export that
    // cannot be redistributed with the required licenses.
    REQUIRE(fs::remove(first["notices"].asString()));
    json::Value missing = json::Value::object();
    missing["script"] = "boost";
    missing["folder"] = (root.path / "without-notices").string();
    auto refused = anywhere.bus->execute({"dsp.export", missing, kTest});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "dsp.notices");
    CHECK(refused.error().path == "/args/script");
    CHECK_FALSE(refused.error().hint.empty());
    CHECK_FALSE(fs::exists(root.path / "without-notices" / "Boost.clap"));
}

TEST_CASE("dsp.test plays notes into an instrument and hears its pitch") {
    const fs::path player = kStock / "Script Player.clap";
    if (!fs::exists(player)) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    SessionOptions o;
    o.workdir = root.path;
    o.pluginPath = {player};
    auto session = Session::open(o);
    REQUIRE(session);
    SandboxHost host(std::move(*session), {});
    cmd::Registry registry;
    registerCommands(registry);
    cmd::Dispatcher bus(registry, host);
    auto call = [&](std::string name, json::Value args) {
        return bus.execute({std::move(name), std::move(args), kTest});
    };
    std::ifstream in(std::string(SUPPLE_SCRIPT_EXAMPLES) + "/synth.cpp");
    std::stringstream text;
    text << in.rdbuf();
    json::Value w = json::Value::object();
    w["name"] = "synth";
    w["source"] = text.str();
    auto written = call("dsp.write", std::move(w));
    REQUIRE_MESSAGE(written, (written ? std::string() : written.error().toString()));
    REQUIRE((*written)["ok"].asBool());

    json::Value t = json::Value::object();
    t["script"] = "synth";
    auto tested = call("dsp.test", t);
    REQUIRE_MESSAGE(tested, (tested ? std::string() : tested.error().toString()));
    json::Value result = *tested;
    std::string problems;
    for (const json::Value& p : result["problems"].asArray()) problems += p.asString() + "\n";
    CHECK_MESSAGE(problems.empty(), problems);
    REQUIRE(result["signals"].size() == 3);
    CHECK(result["signals"].at(0).find("peak_dbfs")->asDouble() > -40.0);  // notes
    const json::Value& note = result["signals"].at(1);
    REQUIRE(note.find("pitch_hz"));
    CHECK(note.find("pitch_hz")->asDouble() == doctest::Approx(440.0).epsilon(0.002));
    CHECK(result["signals"].at(2).find("peak_dbfs")->asDouble() < -150.0);  // no notes

    // An instrument takes notes, not audio.
    t["signals"] = json::Value::array();
    t["signals"].push_back("sine");
    auto refused = call("dsp.test", t);
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "dsp.signal");
}

TEST_CASE("dsp.test gives a convolution reverb its room through a patch") {
    const fs::path player = kStock / "Script Player.clap";
    if (!fs::exists(player)) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    SessionOptions o;
    o.workdir = root.path;
    o.pluginPath = {player};
    auto session = Session::open(o);
    REQUIRE(session);
    SandboxHost host(std::move(*session), {});
    cmd::Registry registry;
    registerCommands(registry);
    cmd::Dispatcher bus(registry, host);
    auto call = [&](std::string name, json::Value args) {
        return bus.execute({std::move(name), std::move(args), kTest});
    };
    std::ifstream in(std::string(SUPPLE_SCRIPT_EXAMPLES) + "/reverb.cpp");
    std::stringstream text;
    text << in.rdbuf();
    json::Value w = json::Value::object();
    w["name"] = "reverb";
    w["source"] = text.str();
    auto written = call("dsp.write", std::move(w));
    REQUIRE_MESSAGE(written, (written ? std::string() : written.error().toString()));
    // The script's file, and how to give it one.
    const json::Value* files = (*written)["plugin"].find("files");
    REQUIRE(files);
    CHECK(files->at(0).find("key")->asString() == "room");
    CHECK(files->at(0).find("kind")->asString() == "impulse");

    // Half a second of a fading echo.
    audio::AudioData room;
    room.channels.assign(1, std::vector<float>(24000));
    for (size_t n = 0; n < room.channels[0].size(); ++n)
        room.channels[0][n] = static_cast<float>((n % 997 == 0 ? 0.5 : 0.0) *
                                                 std::exp(-static_cast<double>(n) / 6000.0));
    const fs::path ir = root.path / "room.wav";
    fs::create_directories(root.path);
    REQUIRE(audio::writeWav(ir, room));
    auto test = [&](const std::string& path) {
        json::Value t = json::Value::object();
        t["script"] = "reverb";
        t["signals"] = json::Value::array();
        t["signals"].push_back("impulse");
        t["patch"]["params"]["mix"] = 100;
        t["patch"]["properties"]["room"] = path;
        return call("dsp.test", std::move(t));
    };
    auto dry = test("");
    auto wet = test(ir.string());
    REQUIRE_MESSAGE(wet, (wet ? std::string() : wet.error().toString()));
    REQUIRE(dry);
    // The room's echo comes back: a tail the dry render does not have.
    CHECK((*wet)["signals"].at(0).find("peak_dbfs")->asDouble() > -30.0);
    CHECK((*dry)["signals"].at(0).find("peak_dbfs")->asDouble() < -150.0);
    // A path that names nothing is refused, saying which and why.
    auto missing = test((root.path / "nowhere.wav").string());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().toString().find("there is no file there") != std::string::npos);
}

#endif
