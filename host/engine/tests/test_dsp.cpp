// DSP scripts over the bus (ADR-0032): a script written with dsp.write is a
// plugin like a stock one -- listed, loaded, and set in physical units
// through its patch -- and its mistakes come back with hints.
#include <doctest/doctest.h>
#include <unistd.h>

#include <filesystem>
#include <string>

#include "supple/engine/app.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

namespace {

const fs::path kStock = STOCK_CLAP_DIR;
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

struct Root {
    fs::path path = fs::temp_directory_path() / ("supple-engine-dsp-" + std::to_string(::getpid()));
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

const char* const kScript = R"(#include <supple/script.h>

struct AgentGain : Effect {
    static constexpr Info info = {.name = "Agent Gain", .id = "com.example.agent_gain"};
    Param gain = decibels("Gain", -24, 24, 0).smoothed(20).help("The level change.");
    Param mix = percent("Mix", 50).help("Dry to processed.");

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i)
                io[c][i] = lerp(io[c][i], io[c][i] * dbToGain(gain.at(i)), mix);
    }
};
)";

}  // namespace

TEST_CASE("a DSP script written over the bus is a plugin an agent sets through its patch") {
    if (!fs::exists(kStock / "Script Player.clap")) {
        MESSAGE("skipped: no script player in " << kStock.string());
        return;
    }
    Root root;
    AppOptions o;
    o.workdir = root.path;
    o.chainDir = root.path / "chains";
    o.pluginPath = {kStock};
    auto app = EngineApp::create(o, {.console9 = false});
    REQUIRE(app);
    cmd::Registry registry;
    registerEngineCommands(registry);
    cmd::Dispatcher bus(registry, **app);
    auto call = [&](std::string name, json::Value args) {
        auto r = bus.execute({std::move(name), std::move(args), kTest});
        REQUIRE_MESSAGE(r, (r ? std::string() : r.error().toString()));
        return *r;
    };
    auto write = [&](const std::string& source) {
        json::Value a = json::Value::object();
        a["name"] = "agent_gain";
        a["source"] = source;
        return call("dsp.write", std::move(a));
    };

    json::Value written = write(kScript);
    CHECK(written["ok"].asBool());
    CHECK(written["listed"].asBool());
    CHECK(written["plugin"]["id"].asString() == "com.example.agent_gain");
    CHECK(written["plugin"]["params"].at(1).find("unit")->asString() == "%");
    CHECK(written["plugin"]["params"].at(1).find("default")->asDouble() == 50.0);

    call("track.add", json::parse(R"({"name": "Vocal"})").value());
    json::Value added = call("fx.add", json::parse(R"({"track": "tr_1",
        "plugin": "com.example.agent_gain", "patch": {"params": {"gain": -6}}})")
                                           .value());
    CHECK(added["semantic"].asBool());
    const std::string fx = added["fx"].asString();
    auto patch = [&] {
        return call("fx.patch.get", json::parse(R"({"fx": ")" + fx + R"("})").value())["patch"];
    };
    CHECK(patch()["params"]["gain"].asDouble() == doctest::Approx(-6));
    CHECK(patch()["params"]["mix"].asDouble() == doctest::Approx(50));
    call("fx.param.set", json::parse(R"({"fx": ")" + fx + R"(", "values": {"mix": 25}})").value());
    CHECK(patch()["params"]["mix"].asDouble() == doctest::Approx(25));

    // A mistake: saved, reported with its line and hint, and no longer a plugin.
    std::string broken = kScript;
    broken.replace(broken.find("dbToGain(gain.at(i))"), 20,
                   "dbToGain(gain.at(i)) / (io.frames / c)");
    json::Value failed = write(broken);
    CHECK_FALSE(failed["ok"].asBool());
    CHECK_FALSE(failed["listed"].asBool());
    REQUIRE(failed["diagnostics"].size() >= 1);
    CHECK(failed["diagnostics"].at(0).find("code")->asString() == "script.ub.division");
    CHECK(failed["diagnostics"].at(0).find("line")->asInt() == 11);
    CHECK_FALSE(failed["diagnostics"].at(0).find("hint")->asString().empty());
    json::Value listed = call("dsp.list", json::Value::object());
    REQUIRE(listed["scripts"].size() == 1);
    CHECK_FALSE(listed["scripts"].at(0).find("ok")->asBool());
    CHECK(call("dsp.get", json::parse(R"({"name": "agent_gain"})").value())["source"].asString() ==
          broken);
}
