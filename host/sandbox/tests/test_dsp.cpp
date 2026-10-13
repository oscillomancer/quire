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

}  // namespace

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
