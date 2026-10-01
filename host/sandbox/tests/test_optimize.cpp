// The optimizer end to end with Orrery (docs/specs/optimizer.md §6): recover a
// hidden patch from its render, and hit a descriptor profile.
#include <dlfcn.h>
#include <doctest/doctest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <thread>

#include "../src/optimize_loss.h"
#include "../src/optimize_range.h"
#include "../src/schema_params.h"
#include "supple/audio/audio_file.h"
#include "supple/sandbox/commands.h"

using namespace supple;
using namespace supple::sandbox;
namespace fs = std::filesystem;

namespace {

const fs::path kOrrery = ORRERY_CLAP_PATH;
const fs::path kFixture = FIXTURE_CLAP_PATH;
std::atomic<unsigned> gRootNumber{0};

struct Root {
    fs::path path =
        fs::temp_directory_path() / ("supple-sandbox-opt-" + std::to_string(::getpid()) + "-" +
                                     std::to_string(gRootNumber.fetch_add(1)));
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
    explicit Harness(std::function<void(std::string_view)> referenceHook = {}) {
        registerCommands(registry);
        SessionOptions o;
        o.workdir = root.path;
        o.pluginPath = {kOrrery, kFixture};
        o.referencePreparationHook = std::move(referenceHook);
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
    CHECK(result["failed"].asInt() == 0);
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
    auto saved = h.host->session().findRender(best);
    REQUIRE(saved);
    CHECK((*saved)->tailSeconds > 0.0);
    CHECK((*saved)->tailSeconds <= 0.3);
    CHECK((*saved)->chain == std::vector<std::string>{"in_1"});
    CHECK((*saved)->latency == 0);
    json::Value again =
        h.call("optimize.start", R"({"instance":"in_1",
        "params":["filter1.cutoff"],"target":{"reference":")" +
                                     best + R"("},"evaluations":10,"max_seconds":10})");
    CHECK(h.waitDone(again["job"].asString())["status"].asString() == "done");
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

TEST_CASE("curve metadata rejects invalid exponents without throwing") {
    for (std::string_view curve :
         {"pow:invalid", "pow:0", "pow:-2", "pow:inf", "bipolar:nan", "pow:2garbage"}) {
        auto parsed = curveFromName(curve);
        CHECK_FALSE(parsed);
        if (!parsed) CHECK(parsed.error().code == "sandbox.param_curve");
    }
    auto good = curveFromName("pow:3");
    REQUIRE(good);
    CHECK(good->k == doctest::Approx(3.0));
    Harness h;
    h.call("plugin.load", R"({"plugin":"com.suppleaudio.test.bad-curve"})");
    auto bad = h.d->execute({"optimize.start", j(R"({"instance":"in_1","params":["gain"],
                                 "target":{"profile":{"schema":"supple.analysis-profile/1.0",
                                 "targets":{"spectral.centroid_hz.median":{"target":1000,
                                 "tolerance":100}}}}})"),
                             kTest});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "sandbox.param_curve");
    CHECK(bad.error().path.find("/params/0") != std::string::npos);
}

TEST_CASE("compare reports the scored loss the optimizer minimizes") {
    Harness h;
    h.call("plugin.load", R"({"plugin": ")" + kOrrery.string() + R"("})");
    h.call("patch.set", kPatch);
    const std::string render = R"({"notation": "1: C3:h r:h", "seconds": 1.0, "tail_s": 0.3})";
    json::Value first = h.call("render", render);
    h.call("param.set", R"({"instance": "in_1", "values": {"filter1.cutoff": 6000}})");
    h.call("render", render);

    // Identical renders: a zero loss, both note-offs taken from the renders.
    json::Value same = h.call("compare", R"({"reference": "rd_1", "test": "rd_1"})");
    INFO(json::writeCompact(same));
    CHECK(same["loss"]["total"].asDouble() == doctest::Approx(0.0));
    CHECK(same["loss"]["note_off_s"]["reference"].asDouble() == doctest::Approx(1.0));
    CHECK(same["loss"]["note_off_s"]["test"].asDouble() == doctest::Approx(1.0));
    CHECK(same["loss"].find("note_off_assumed") == nullptr);

    // A different cutoff: a non-zero loss, with the terms that explain it.
    json::Value other = h.call("compare", R"({"reference": "rd_1", "test": "rd_2"})");
    INFO(json::writeCompact(other));
    CHECK(other["loss"]["total"].asDouble() > 1.0);
    CHECK(other["loss"]["terms"].find("ltas") != nullptr);

    // A quieter copy of the reference is the same sound: loudness matching
    // cancels the gain, so the scored loss stays at zero.
    auto quietAudio = audio::readAudio(first["path"].asString());
    REQUIRE(quietAudio);
    for (auto& channel : quietAudio->channels)
        for (float& sample : channel) sample *= 0.5f;
    const fs::path quiet = h.root.path / "quiet.wav";
    REQUIRE(audio::writeWav(quiet, *quietAudio, {}).ok());
    json::Value matched = h.call("compare", R"({"reference": "rd_1", "test": ")" + quiet.string() +
                                                R"(", "test_note_off_s": 1.0})");
    INFO(json::writeCompact(matched));
    CHECK(matched["loss"]["total"].asDouble() < 1e-6);
    CHECK(matched["loss"].find("note_off_assumed") == nullptr);

    // Without that argument the file borrows the render's note-off, and says so.
    json::Value assumed =
        h.call("compare", R"({"reference": "rd_1", "test": ")" + quiet.string() + R"("})");
    INFO(json::writeCompact(assumed));
    CHECK(assumed["loss"].find("note_off_assumed") != nullptr);
    CHECK(assumed["loss"]["note_off_s"]["test"].asDouble() == doctest::Approx(1.0));
    // Settling on the same note-off is the same score, to analysis precision.
    CHECK(std::fabs(assumed["loss"]["total"].asDouble() - matched["loss"]["total"].asDouble()) <
          1e-9);
    // A wrong note-off is visible in the result, and changes the loss.
    json::Value shifted = h.call("compare", R"({"reference": "rd_1", "test": ")" + quiet.string() +
                                                R"(", "test_note_off_s": 0.5})");
    CHECK(shifted["loss"]["note_off_s"]["test"].asDouble() == doctest::Approx(0.5));
}

TEST_CASE("a render depends only on the patch, not on the session's history") {
    // The sandbox's promise: a render of a given patch is the same audio
    // whether the session rendered before or not. A change to a *smoothed*
    // parameter (here the amp level) must not glide in from the previous
    // render's value.
    Harness h;
    h.call("plugin.load", R"({"plugin": ")" + kOrrery.string() + R"("})");
    const std::string render = R"({"notation": "1: C3:h r:h", "seconds": 1.0, "tail_s": 0.3})";
    const std::string start = R"({"instance": "in_1", "patch": {"schema": "supple.orrery-patch/1.0",
        "seed": 42, "voice": {"mode": "mono", "glide": "20 ms"},
        "oscillators": {"osc1": {"type": "va", "wave": "sine", "to": "direct"}},
        "envelopes": {"amp": {"attack": "3 ms", "sustain": "100 %", "release": "80 ms"}}}})";
    const std::string level = R"({"instance": "in_1", "patch": {"schema": "supple.orrery-patch/1.0",
        "seed": 42, "voice": {"mode": "mono", "glide": "20 ms"},
        "oscillators": {"osc1": {"type": "va", "wave": "sine", "to": "direct"}},
        "envelopes": {"amp": {"attack": "3 ms", "sustain": "100 %", "release": "80 ms"}},
        "amp": {"level": "0.825 dB"}}})";

    // (a) fresh: the target patch is the session's first render.
    h.call("patch.set", level);
    json::Value fresh = h.call("render", render);

    // (b) the same patch after a render of a different patch, applied as a
    // separate command (a flat params merge, as an agent or the optimizer does).
    h.call("patch.set", start);
    h.call("render", render);
    h.call("patch.merge", R"({"instance": "in_1", "patch": {"amp": {"level": "0.825 dB"}}})");
    json::Value after = h.call("render", render);

    auto audio = [](const std::string& path) {
        auto a = audio::readAudio(path);
        REQUIRE(a);
        REQUIRE_FALSE(a->channels.empty());
        return a->channels[0];
    };
    const auto x = audio(fresh["path"].asString());
    const auto y = audio(after["path"].asString());
    REQUIRE(x.size() == y.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < x.size(); ++i)
        worst = std::max(worst, std::fabs(static_cast<double>(x[i]) - static_cast<double>(y[i])));
    // NOTE (open): this covers the dispatcher path. The same sequence driven
    // through the MCP server (what tools/eval-harness uses) still differs in the
    // first ~40 ms after a level change (1922 of 211456 samples, max 1e-2, which
    // `analyze` turns into decay20_ms 348 vs 585): see
    // docs/benchmarks/eval/2026-10-01-p3.4c-reference-rerun.md.
    INFO("worst sample difference: " << worst);
    CHECK(worst == 0.0);
}

TEST_CASE("plugin.describe reports parameter activity from the current patch") {
    Harness h;
    h.call("plugin.load", R"({"plugin": ")" + kOrrery.string() + R"("})");
    // At load the patch has no filter, so the *load-time* schema marks filter1's
    // parameters inactive; kPatch adds filters.filter1, and from then on the
    // plugin reports them active. describe must follow the patch, not the copy
    // taken at load (this check fails on the old behavior).
    h.call("patch.set", kPatch);
    json::Value after = h.call("plugin.describe", R"({"instance": "in_1", "detail": "full"})");
    REQUIRE(after["params"].find("filter1.cutoff") != nullptr);
    CHECK(after["params"]["filter1.cutoff"].find("active") == nullptr);
    CHECK(after["params"]["filter1.cutoff"]["type"].asString() == "float");
    // The optimizer reads the instance's live schema, so the two agree; before
    // the fix they disagreed, which is what made this confusing to an agent.
    json::Value probe = h.call("optimize.start",
                               R"({"instance": "in_1", "params": ["filter1.cutoff"],
            "target": {"profile": {"schema": "supple.analysis-profile/1.0",
                       "targets": {"spectral.centroid_hz.median": {"target": 1500, "tolerance": 100}}}},
            "evaluations": 10, "seconds": 0.2, "tail_s": 0})");
    CHECK(probe["params"].size() == 1);
    h.call("job.cancel", R"({"job": ")" + probe["job"].asString() + R"("})");
    h.call("job.wait", R"({"job": ")" + probe["job"].asString() + R"(", "timeout_s": 30})");
    // Drop the module again: describe reports inactive and the optimizer
    // refuses the key, the same way round.
    h.call("patch.set", R"({"instance": "in_1", "patch": {"schema": "supple.orrery-patch/1.0"}})");
    json::Value dropped = h.call("plugin.describe", R"({"instance": "in_1", "detail": "full"})");
    REQUIRE(dropped["params"]["filter1.cutoff"].find("active") != nullptr);
    CHECK(dropped["params"]["filter1.cutoff"]["active"].asBool() == false);
    auto refused =
        h.d->execute({"optimize.start", j(R"({"instance": "in_1", "params": ["filter1.cutoff"],
               "target": {"profile": {"schema": "supple.analysis-profile/1.0",
                          "targets": {"spectral.centroid_hz.median": {"target": 1500, "tolerance": 100}}}},
               "evaluations": 10, "seconds": 0.2, "tail_s": 0})"),
                      kTest});
    REQUIRE_FALSE(refused);
    CHECK(refused.error().code == "optimize.not_tunable");
}

TEST_CASE("auto search windows follow the parameter's shape") {
    params::ParamSpec cutoff;  // 20 Hz .. 20 kHz, log
    cutoff.unit = params::Unit::Hz;
    cutoff.min = 20.0;
    cutoff.max = 20000.0;
    cutoff.curve = params::Curve{params::CurveKind::Log, 1.0};
    // A ratio-perceived quantity gets ±2 octaves (×4 / ÷4).
    const auto hz = detail::autoWindow(cutoff, 1200.0);
    CHECK(params::fromNormalized(cutoff, hz.first) == doctest::Approx(300.0).epsilon(0.01));
    CHECK(params::fromNormalized(cutoff, hz.second) == doctest::Approx(4800.0).epsilon(0.01));

    params::ParamSpec release;  // 1 ms .. 30 s, pow:3
    release.unit = params::Unit::ms;
    release.min = 1.0;
    release.max = 30000.0;
    release.curve = params::Curve{params::CurveKind::Pow, 3.0};
    const auto ms = detail::autoWindow(release, 300.0);
    // The window covers ÷4 and ×4 and stays inside the parameter's range: on a
    // pow curve the wider side sets the width.
    CHECK(params::fromNormalized(release, ms.first) <= 75.0);
    CHECK(params::fromNormalized(release, ms.first) >= 1.0);
    CHECK(params::fromNormalized(release, ms.second) >= 1200.0);
    CHECK(params::fromNormalized(release, ms.second) <= 30000.0);

    params::ParamSpec level;  // −60 .. +6 dB, linear: ±25 % of the range
    level.unit = params::Unit::dB;
    level.min = -60.0;
    level.max = 6.0;
    const auto db = detail::autoWindow(level, -30.0);
    CHECK(params::fromNormalized(level, db.first) == doctest::Approx(-46.5));
    CHECK(params::fromNormalized(level, db.second) == doctest::Approx(-13.5));

    // At the edge of the range the window is one-sided, never outside it.
    const auto atMin = detail::autoWindow(cutoff, 20.0);
    CHECK(atMin.first == doctest::Approx(0.0));
    CHECK(atMin.second > 0.0);
    CHECK(atMin.second < 1.0);
    const auto atMax = detail::autoWindow(level, 6.0);
    CHECK(atMax.second == doctest::Approx(1.0));
    CHECK(atMax.first > 0.0);
    CHECK(atMax.first < 1.0);
}

TEST_CASE("reference envelope loss uses a log-time distance") {
    analysis::Report ref, cand;
    ref.envelope.emplace();
    cand.envelope.emplace();
    ref.envelope->attackMs = 100.0;
    cand.envelope->attackMs = 160.0;
    ref.envelope->decay20Ms = cand.envelope->decay20Ms = 200.0;
    ref.envelope->releaseMs = cand.envelope->releaseMs = 300.0;
    const auto loss = detail::referenceLoss(ref, cand);
    // |log2((160 + 10) / (100 + 10))| / 0.43, squared. The old
    // |c − r| / max(10 ms, r) / 0.3 term scored 4.0 for the same pair.
    CHECK(loss.terms.at("envelope.attack") == doctest::Approx(2.13317));
    CHECK(loss.terms.at("envelope.decay20") == doctest::Approx(0.0));
    CHECK(loss.terms.at("envelope.release") == doctest::Approx(0.0));
    CHECK(loss.total == doctest::Approx(2.13317));
}

TEST_CASE("envelope time terms are bounded, logarithmic and keep the one-sided penalty") {
    auto attackTerm = [](double attackMs, double referenceMs) {
        analysis::Report ref, cand;
        ref.envelope.emplace();
        cand.envelope.emplace();
        ref.envelope->attackMs = referenceMs;
        cand.envelope->attackMs = attackMs;
        return detail::referenceLoss(ref, cand).terms.at("envelope.attack");
    };
    CHECK(attackTerm(50.0, 50.0) == doctest::Approx(0.0));
    // 0 ms against 230 ms: |log2(240 / 10)| / 0.43, squared. The old term was
    // unbounded and scored 5877 for the same pair.
    CHECK(attackTerm(0.0, 230.0) == doctest::Approx(113.693));
    CHECK(attackTerm(230.0, 0.0) == doctest::Approx(113.693));
    // Tiny values are bounded too: 0 ms against 0.5 ms is not a huge distance.
    CHECK(attackTerm(0.0, 0.5) < 113.693);
    // For references well above the 10 ms offset the term matches the old one
    // (whose denominator was max(10 ms, reference)) within 15 %.
    const double old1010 = std::pow((1010.0 - 1000.0) / 1000.0 / 0.3, 2.0);
    CHECK(attackTerm(1010.0, 1000.0) == doctest::Approx(old1010).epsilon(0.15));
    const double old110 = std::pow((110.0 - 100.0) / 110.0 / 0.3, 2.0);
    CHECK(attackTerm(100.0, 110.0) == doctest::Approx(old110).epsilon(0.15));
    // Near the offset the new term is the smaller of the two: the +10 ms
    // denominator is larger than the old max(10 ms, reference).
    const double old100 = std::pow((110.0 - 100.0) / 100.0 / 0.3, 2.0);
    CHECK(attackTerm(110.0, 100.0) < old100);
    CHECK(attackTerm(110.0, 100.0) == doctest::Approx(0.0852245));
    // A value present on one side only keeps its old contribution: distance
    // 1.43 / scale 0.43, squared ≈ 11.1.
    analysis::Report ref, cand;
    ref.envelope.emplace();
    cand.envelope.emplace();
    ref.envelope->attackMs = cand.envelope->attackMs = 20.0;
    ref.envelope->releaseMs = 200.0;
    const auto oneSidedRelease = detail::referenceLoss(ref, cand);
    CHECK(oneSidedRelease.terms.at("envelope.release") == doctest::Approx(11.0595));
    cand.envelope->releaseMs = 200.0;
    ref.envelope->decay20Ms = 200.0;
    const auto oneSidedDecay = detail::referenceLoss(ref, cand);
    CHECK(oneSidedDecay.terms.at("envelope.decay20") == doctest::Approx(11.0595));
}

TEST_CASE("reference read and analysis run on the job worker after optimize.start returns") {
    const auto commandThread = std::this_thread::get_id();
    std::atomic<bool> entered = false, release = false, analyzed = false;
    std::thread::id readThread, analysisThread;
    Harness h([&](std::string_view phase) {
        if (phase == "before_read") {
            readThread = std::this_thread::get_id();
            entered.store(true, std::memory_order_release);
            entered.notify_one();
            // A regression to synchronous preparation must fail without deadlocking.
            if (readThread != commandThread)
                while (!release.load(std::memory_order_acquire)) release.wait(false);
        } else if (phase == "after_analysis") {
            analysisThread = std::this_thread::get_id();
            analyzed.store(true, std::memory_order_release);
            analyzed.notify_one();
        }
    });
    h.call("plugin.load", R"({"plugin": ")" + kOrrery.string() + R"("})");
    h.call("patch.set", kPatch);
    audio::AudioData audio;
    audio.sampleRate = 48000.0;
    audio.channels = {std::vector<float>(48000, 0.0f), std::vector<float>(48000, 0.0f)};
    const fs::path file = h.root.path / "reference.wav";
    REQUIRE(audio::writeWav(file, audio, {}).ok());
    const auto start = std::chrono::steady_clock::now();
    auto started =
        h.call("optimize.start", R"({"instance":"in_1",
        "params":["filter1.cutoff"],"target":{"reference":")" +
                                     file.string() + R"("},"evaluations":10,"max_seconds":10})");
    const double ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    INFO(ms);
    MESSAGE("optimize.start latency (diagnostic only): " << ms << " ms");
    entered.wait(false);
    CHECK(readThread != commandThread);
    CHECK_FALSE(analyzed.load(std::memory_order_acquire));
    auto status = h.d->execute(
        {"job.status", j(R"({"job": ")" + started["job"].asString() + R"("})"), kTest});
    CHECK(status);
    if (status) CHECK((*status)["status"].asString() == "running");
    release.store(true, std::memory_order_release);
    release.notify_one();
    analyzed.wait(false);
    CHECK(analysisThread == readThread);
    h.call("job.cancel", R"({"job": ")" + started["job"].asString() + R"("})");
}

TEST_CASE("invalid profiles and universally rejected patches fail jobs") {
    Harness h;
    h.call("plugin.load", R"({"plugin": ")" + kOrrery.string() + R"("})");
    h.call("patch.set", kPatch);
    auto bad = h.call("optimize.start", R"({"instance":"in_1","params":["filter1.cutoff"],
        "target":{"profile":{"schema":"wrong","targets":{}}},"evaluations":10})");
    auto failure = h.waitDone(bad["job"].asString());
    REQUIRE(failure["status"].asString() == "failed");
    CHECK(failure["error"]["code"].asString() == "analysis.profile.schema");

    Harness refusing;
    refusing.call("plugin.load", R"({"plugin":"com.suppleaudio.test.reject-gain"})");
    auto rejected = refusing.call("optimize.start", R"({"instance":"in_1","params":["gain"],
        "target":{"profile":{"schema":"supple.analysis-profile/1.0",
        "targets":{"spectral.centroid_hz.median":{"target":1000,"tolerance":100}}}},
        "seconds":0.1,"tail_s":0,"evaluations":10})");
    auto rejectedStatus = refusing.waitDone(rejected["job"].asString());
    REQUIRE(rejectedStatus["status"].asString() == "failed");
    CHECK(rejectedStatus["error"]["code"].asString() == "fixture.reject");
}

TEST_CASE("profile weights scale candidate loss") {
    Harness h;
    h.call("plugin.load", R"({"plugin": ")" + kOrrery.string() + R"("})");
    h.call("patch.set", kPatch);
    auto runWeight = [&](int weight) {
        const std::string args =
            R"({"instance":"in_1","params":["filter1.cutoff"],
            "target":{"profile":{"schema":"supple.analysis-profile/1.0","targets":{
            "spectral.centroid_hz.median":{"target":1,"tolerance":1,"weight":)" +
            std::to_string(weight) +
            R"(},"spectral.flatness.median":{"target":0.5,"tolerance":0.1,"weight":1}}}},
            "seconds":0.2,"tail_s":0,"evaluations":10,"seed":1})";
        auto started = h.call("optimize.start", args);
        auto done = h.waitDone(started["job"].asString());
        INFO(json::writeCompact(done));
        REQUIRE(done["status"].asString() == "done");
        return done["result"]["loss"]["initial"].asDouble();
    };
    const double plain = runWeight(1);
    const double heavy = runWeight(100);
    CHECK(heavy > plain * 50.0);
}

TEST_CASE("worker-owned CLAP clone keeps main-thread lifecycle") {
    void* library = dlopen(kFixture.c_str(), RTLD_NOW);
    REQUIRE(library);
    auto failures = reinterpret_cast<int (*)()>(dlsym(library, "supple_fixture_thread_failures"));
    REQUIRE(failures);
    const int before = failures();
    {
        Harness h;
        h.call("plugin.load", R"({"plugin":"com.suppleaudio.test.threaded-gain"})");
        auto started = h.call("optimize.start", R"({"instance":"in_1","params":["gain"],
            "target":{"profile":{"schema":"supple.analysis-profile/1.0",
            "targets":{"spectral.centroid_hz.median":{"target":1000,"tolerance":100}}}},
            "seconds":0.1,"tail_s":0,"evaluations":10})");
        auto done = h.waitDone(started["job"].asString());
        INFO(json::writeCompact(done));
        REQUIRE(done["status"].asString() == "done");
    }
    CHECK(failures() == before);
    dlclose(library);
}

TEST_CASE("finished job threads are reaped before another starts") {
    Harness h;
    std::atomic<int> completed{0};
    for (int i = 0; i < 120; ++i) {
        h.host->session().startJob([&](std::stop_token) { completed.fetch_add(1); });
        while (completed.load() <= i) std::this_thread::yield();
        CHECK(h.host->session().jobThreadCountForTests() <= 2);
    }
    h.host->session().startJob([](std::stop_token) {});
    CHECK(h.host->session().jobThreadCountForTests() <= 2);
}

TEST_CASE("job.cancel stops a generation after the current candidate") {
    void* library = dlopen(kFixture.c_str(), RTLD_NOW);
    REQUIRE(library);
    auto starts = reinterpret_cast<int (*)()>(dlsym(library, "supple_fixture_patch_starts"));
    REQUIRE(starts);
    const int before = starts();
    Harness h;
    h.call("plugin.load", R"({"plugin":"com.suppleaudio.test.slow-gain"})");
    auto started = h.call("optimize.start", R"({"instance":"in_1","params":["gain"],
        "target":{"profile":{"schema":"supple.analysis-profile/1.0",
        "targets":{"spectral.centroid_hz.median":{"target":1000,"tolerance":100}}}},
        "seconds":0.1,"tail_s":0,"evaluations":100,"max_seconds":60})");
    const auto readyBy = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (starts() == before && std::chrono::steady_clock::now() < readyBy)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    REQUIRE(starts() > before);
    const auto cancelledAt = std::chrono::steady_clock::now();
    h.call("job.cancel", R"({"job": ")" + started["job"].asString() + R"("})");
    while (h.host->session().activeJobThreadCountForTests() != 0 &&
           std::chrono::steady_clock::now() - cancelledAt < std::chrono::seconds(2))
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    const double elapsedMs =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - cancelledAt)
            .count();
    INFO(elapsedMs);
    CHECK(h.host->session().activeJobThreadCountForTests() == 0);
    CHECK(elapsedMs < 500.0);
    CHECK_FALSE(fs::exists(h.root.path / "renders" / (started["render"].asString() + ".wav")));
    dlclose(library);
}

TEST_CASE("a flat params merge renders the same whether or not the session rendered before") {
    // The eval harness's sequence (reference.sub-bass), from an MCP origin: the
    // starting patch, optionally a render, then one flat merge of the amp
    // envelope + level. (What the harness saw was a stale Orrery in ~/.clap
    // shadowing the built one — fixed in the catalogue order, see clap-host's
    // test; this pins the sandbox side.)
    const std::string start = R"({"instance": "in_1", "patch": {"schema": "supple.orrery-patch/1.0",
        "seed": 42, "voice": {"mode": "mono", "glide": "20 ms"},
        "oscillators": {"osc1": {"type": "va", "wave": "sine", "to": "direct"}},
        "envelopes": {"amp": {"attack": "3 ms", "sustain": "100 %", "release": "80 ms"}}}})";
    const std::string merge = R"({"instance": "in_1", "patch": {"params": {"amp.attack": 0.001042,
        "amp.decay": 256.198095, "amp.sustain": 99.997281, "amp.release": 198.32985, "amp.level": 0.82515}}})";
    const std::string render =
        R"({"notation": "notation 1\nmeter 4/4\nvoice v ch 1 vel 96\n1: C2:q r:*", "tail_s": 2})";
    const cmd::Origin mcp{cmd::Origin::Type::Mcp, "supple-eval", {}};
    auto run = [&](bool pre) {
        Harness h;
        auto call = [&](const char* command, const std::string& args) {
            auto r = h.d->execute({command, j(args), mcp});
            REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
            return *r;
        };
        call(
            "session.set",
            R"({"block_size": 256, "meter": "4/4", "sample_rate": 48000, "tail_limit_s": 2, "tempo": 120})");
        call("plugin.load", R"({"plugin": ")" + kOrrery.string() + R"("})");
        call("patch.merge", R"({"instance": "in_1", "patch": {"seed": 42}})");
        call("patch.set", start);
        if (pre) call("render", render);
        call("patch.merge", merge);
        json::Value r = call("render", render);
        auto a = audio::readAudio(r["path"].asString());
        REQUIRE(a);
        return a->channels[0];
    };
    const auto fresh = run(false), after = run(true);
    REQUIRE(fresh.size() == after.size());
    double worst = 0.0;
    for (std::size_t i = 0; i < fresh.size(); ++i)
        worst = std::max(worst,
                         std::fabs(static_cast<double>(fresh[i]) - static_cast<double>(after[i])));
    INFO("worst sample difference: " << worst);
    CHECK(worst == 0.0);
}
