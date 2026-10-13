// The DSP script harness (dsp.test) through the real script player: what it
// measures is what the script does, and what goes wrong is said in words.
#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>
#include <string>

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
