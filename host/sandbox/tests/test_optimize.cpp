// The optimizer end to end with Orrery (docs/specs/optimizer.md §6): recover a
// hidden patch from its render, and hit a descriptor profile.
#include <doctest/doctest.h>
#include <unistd.h>

#include <cmath>
#include <filesystem>

#include "supple/sandbox/commands.h"

using namespace supple;
using namespace supple::sandbox;
namespace fs = std::filesystem;

namespace {

const fs::path kOrrery = ORRERY_CLAP_PATH;

struct Root {
    fs::path path =
        fs::temp_directory_path() / ("supple-sandbox-opt-" + std::to_string(::getpid()));
    Root() { fs::remove_all(path); }
    ~Root() { fs::remove_all(path); }
};

json::Value j(std::string_view text) { return json::parse(text).value(); }
const cmd::Origin kTest{cmd::Origin::Type::Test, "unit", ""};

constexpr std::string_view kPatch =
    R"({"instance": "in_1", "patch": {"schema": "supple.orrery-patch/1.0",
    "oscillators": {"osc1": {"type": "va", "wave": "saw"}},
    "filters": {"filter1": {"type": "svf"}},
    "envelopes": {"env2": {"attack": 1, "sustain": 0}},
    "routes": {"env2>filter1.cutoff": {"amount": "+2 oct"}}}})";

struct Harness {
    Root root;
    cmd::Registry registry;
    std::unique_ptr<SandboxHost> host;
    std::unique_ptr<cmd::Dispatcher> d;
    Harness() {
        registerCommands(registry);
        SessionOptions o;
        o.workdir = root.path;
        o.pluginPath = {kOrrery};
        auto s = Session::open(o);
        REQUIRE(s);
        host = std::make_unique<SandboxHost>(std::move(*s));
        d = std::make_unique<cmd::Dispatcher>(registry, *host);
    }
    ~Harness() { d.reset(); }  // joins the job before the host goes
    json::Value call(std::string command, std::string_view args) {
        auto r = d->execute({std::move(command), j(args), kTest});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    }
    json::Value waitDone(const std::string& job) {
        for (int i = 0; i < 20; ++i) {
            json::Value w = call("job.wait", R"({"job": ")" + job + R"(", "timeout_s": 30})");
            if (w["status"].asString() != "running") return w;
        }
        FAIL("job did not finish");
        return {};
    }
};

double normalizedLog(double v, double lo, double hi) {
    return std::log(v / lo) / std::log(hi / lo);
}

}  // namespace

TEST_CASE("optimize recovers a hidden patch from its render") {
    Harness h;
    h.call("plugin.load", R"({"plugin": ")" + kOrrery.string() + R"("})");
    h.call("patch.set", kPatch);
    h.call(
        "param.set",
        R"({"instance": "in_1", "values": {"filter1.cutoff": 2400, "filter1.resonance": 60, "env2.decay": 180}})");
    json::Value ref =
        h.call("render", R"({"notation": "1: C3:h r:h", "seconds": 1.0, "tail_s": 0.3})");
    CHECK(ref["render"].asString() == "rd_1");
    h.call("patch.set", kPatch);  // back to defaults: 1 kHz, 0 %, 300 ms
    json::Value started = h.call("optimize.start", R"({"instance": "in_1",
        "params": ["filter1.cutoff", "filter1.resonance", "env2.decay"],
        "target": {"reference": "rd_1"}, "phrase": "1: C3:h r:h", "seconds": 1.0, "tail_s": 0.3,
        "evaluations": 300, "max_seconds": 600, "seed": 1})");  // stops on evaluations only
    CHECK(started["status"].asString() == "running");
    json::Value done = h.waitDone(started["job"].asString());
    INFO(json::writeCompact(done));
    REQUIRE(done["status"].asString() == "done");
    json::Value result = done["result"];
    CHECK(result["loss"]["best"].asDouble() < 0.25 * result["loss"]["initial"].asDouble());
    json::Value p = result["patch"]["params"];
    // Within 10 % of each hidden value in the normalized domain.
    CHECK(std::fabs(normalizedLog(p["filter1.cutoff"].asDouble(), 20, 20000) -
                    normalizedLog(2400, 20, 20000)) < 0.1);
    CHECK(std::fabs(p["filter1.resonance"].asDouble() - 60.0) / 100.0 < 0.1);
    CHECK(std::fabs(std::cbrt((p["env2.decay"].asDouble() - 1) / 29999) -
                    std::cbrt((180.0 - 1) / 29999)) < 0.1);
    // The best candidate is a render the session knows (adopted on the next command).
    const std::string best = result["render"].asString();
    json::Value cmp = h.call("compare", R"({"reference": "rd_1", "test": ")" + best + R"("})");
    CHECK(cmp["ltas_rms_db"].asDouble() < 1.5);
    // The merge patch applies as-is.
    json::Value merge = json::Value::object();
    merge["instance"] = "in_1";
    merge["patch"] = result["patch"];
    h.call("patch.merge", json::writeCompact(merge));
}

TEST_CASE("optimize hits a descriptor profile; cancellation stops a job") {
    Harness h;
    h.call("plugin.load", R"({"plugin": ")" + kOrrery.string() + R"("})");
    h.call("patch.set", kPatch);
    json::Value started =
        h.call("optimize.start", R"({"instance": "in_1", "params": ["filter1.cutoff"],
        "target": {"profile": {"schema": "supple.analysis-profile/1.0",
                   "targets": {"spectral.centroid_hz.median": {"target": 1500, "tolerance": 50}}}},
        "phrase": "1: C3:h r:h", "seconds": 1.0, "tail_s": 0.3, "evaluations": 100, "max_seconds": 600,
        "seed": 2})");
    json::Value done = h.waitDone(started["job"].asString());
    INFO(json::writeCompact(done));
    REQUIRE(done["status"].asString() == "done");
    json::Value a =
        h.call("analyze", R"({"source": ")" + done["result"]["render"].asString() + R"("})");
    CHECK(a["report"]["spectral"]["centroid_hz"].asDouble() == doctest::Approx(1500).epsilon(0.05));

    json::Value slow = h.call("optimize.start", R"({"instance": "in_1", "params": "auto",
        "target": {"profile": {"schema": "supple.analysis-profile/1.0",
                   "targets": {"spectral.centroid_hz.median": {"target": 900, "tolerance": 10}}}},
        "evaluations": 5000, "max_seconds": 600})");
    h.call("job.cancel", R"({"job": ")" + slow["job"].asString() + R"("})");
    json::Value status = h.call("job.status", R"({"job": ")" + slow["job"].asString() + R"("})");
    CHECK(status["status"].asString() == "cancelled");
}
