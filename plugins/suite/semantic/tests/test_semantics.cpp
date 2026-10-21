// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <memory>
#include <string>

#ifdef _WIN32
#include <process.h>
#define getpid _getpid
#else
#include <unistd.h>
#endif

#include "supple/suite/semantics.h"

using namespace supple;
using namespace supple::suite;

namespace {

const char* const kModes[] = {"Low Pass", "High Pass"};

// A plugin with one of each kind of parameter, a meter and three properties
// (one of them a file); with `conditions`, a meaning of its own they change.
std::shared_ptr<const Definition> fixture(bool normalized = true,
                                          std::function<void(Meaning&)> conditions = {}) {
    PluginInfo info;
    info.id = "com.example.fixture";
    info.name = "Fixture";
    info.version = "1.0.0";
    info.description = "A filter, for tests.";
    auto params = [&] {
        ParamTable::Builder b;
        auto norm = [&](ParamDef p) { return normalized ? p.normalizedHostValue() : p; };
        b.add(norm(decibels(1, "Gain", -24.0, 12.0, 0.0)).help("The level."));
        b.add(frequency(2, "Cutoff", 20.0, 20000.0, 1000.0).in("Filter"));
        b.add(choice(3, "Mode", kModes, 0).in("Filter"));
        b.add(toggle(4, "On", true));
        b.add(integer(5, "Steps", 1, 8, 4));
        b.add(norm(percent(6, "Mix", 100.0)));
        b.add(norm(decibels(7, "Meter", -60.0, 0.0, -60.0)).output());
        return b.build();
    };
    auto properties = [] {
        PropertyList::Builder pb;
        pb.add(property(1, "Label").perProject().maxBytes(16));
        pb.add(property(2, "Text", "hello"));
        pb.add(property(3, "Sample").audioFile().help("The sound it plays."));
        return pb.build();
    };
    if (!conditions) return std::make_shared<Definition>(info, params(), properties());
    Meaning meaning = meaningOf(Definition(info, params(), properties()));
    conditions(meaning);
    return std::make_shared<SemanticDefinition>(info, params(), properties(), meaning);
}

json::Value parsed(std::string_view text) {
    json::Value v;
    json::Error e;
    REQUIRE_MESSAGE(json::parse(text, v, e), e.message);
    return v;
}

size_t index(const Definition& d, uint32_t id) {
    return static_cast<size_t>(d.params().indexOf(id));
}

}  // namespace

TEST_CASE("a definition's meaning comes from its names when it states none") {
    const auto d = fixture();
    const Meaning m = meaningOf(*d);
    CHECK(m.patchType == "supple.fixture-patch/1.0");
    REQUIRE(m.params.size() == 7);
    CHECK(m.params[0].key == "gain");
    CHECK(m.params[1].key == "filter.cutoff");
    CHECK(m.params[2].choiceKeys == std::vector<std::string>{"low_pass", "high_pass"});
    CHECK(m.properties[0].key == "label");
    CHECK(meaningProblems(*d, m).empty());
    CHECK(snakeCase("High Cut (Hz)") == "high_cut_hz");
    // A linear continuous parameter whose CLAP value is plain is not what a
    // semantic host expects.
    const auto plain = fixture(false);
    CHECK(meaningProblems(*plain, meaningOf(*plain)).find("normalizedHostValue") !=
          std::string::npos);
}

TEST_CASE("the schema lists every CLAP parameter the way core-params describes it") {
    const auto d = fixture();
    const PatchCodec codec(*d);
    const json::Value schema = codec.schemaDocument();
    CHECK(schema.find("schema")->asString() == "supple.semantic-schema/1.0");
    CHECK(schema.find("patch_type")->asString() == "supple.fixture-patch/1.0");
    const json::Value& params = *schema.find("params");
    CHECK(params.size() == d->params().count());
    const json::Value& gain = *params.find("gain");
    CHECK(gain.find("id")->asInt() == 1);
    CHECK(gain.find("type")->asString() == "float");
    CHECK(gain.find("unit")->asString() == "dB");
    CHECK(gain.find("curve")->asString() == "linear");
    // A normalized host position maps back with platform-dependent rounding.
    CHECK(gain.find("default")->asDouble() == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(params.find("filter.cutoff")->find("curve")->asString() == "log");
    CHECK(params.find("filter.mode")->find("type")->asString() == "enum");
    CHECK(params.find("steps")->find("min")->isInt());
    const json::Value& mix = *params.find("mix");
    CHECK(mix.find("unit")->asString() == "%");
    CHECK(mix.find("max")->asDouble() == 100.0);
    const json::Value& meter = *params.find("meter");
    CHECK(meter.find("default") == nullptr);
    CHECK(meter.find("flags")->asArray().front().asString() == "readonly");
    CHECK(schema.find("patch")
              ->find("properties")
              ->find("params")
              ->find("properties")
              ->find("meter") == nullptr);
    // A file property says it takes a path, after its own help.
    const std::string sample = schema.find("patch")
                                   ->find("properties")
                                   ->find("properties")
                                   ->find("properties")
                                   ->find("sample")
                                   ->find("description")
                                   ->asString();
    CHECK(sample.rfind("The sound it plays. The path of a file (wav ", 0) == 0);
}

TEST_CASE("the schema and the prose say which parameters apply now") {
    // On applies in high-pass mode; the mix while On applies and is on.
    const auto d = fixture(true, [](Meaning& m) {
        m.params[3].activeWhen = {{2, {1}}};
        m.params[5].activeWhen = {{3, {1}}};
    });
    REQUIRE(meaningProblems(*d, meaningOf(*d)).empty());
    const PatchCodec codec(*d);
    state::Snapshot s(*d);
    s.defaultProperties();
    const size_t mode = index(*d, 3);
    auto active = [&](const char* key) {
        const json::Value schema = codec.schemaDocument(&s);
        const json::Value* a = schema.find("params")->find(key)->find("active");
        return !a || a->asBool();
    };
    // Low-pass: neither applies, though On is on.
    CHECK(codec.activity(s) == std::vector<bool>{true, true, true, false, true, false, true});
    CHECK_FALSE(active("on"));
    CHECK_FALSE(active("mix"));
    CHECK(active("gain"));
    CHECK(codec.describe(s, false).find("Mix") == std::string::npos);
    s.host[mode] = toHost(d->params().at(mode), 1.0);
    CHECK(active("on"));
    CHECK(active("mix"));
    CHECK(codec.describe(s, false).find("Mix 100") != std::string::npos);
    // Without values the schema leaves activity out; the conditions it states.
    const json::Value schema = codec.schemaDocument();
    CHECK(schema.find("params")->find("mix")->find("active") == nullptr);
    const json::Value& when = *schema.find("params")->find("on")->find("active_when");
    REQUIRE(when.size() == 1);
    CHECK(when.asArray()[0].find("param")->asString() == "filter.mode");
    CHECK(when.asArray()[0].find("in")->asArray()[0].asString() == "high_pass");
    const json::Value& mix = *schema.find("params")->find("mix");
    CHECK(mix.find("active_when")->asArray()[0].find("in")->asArray()[0].asBool());
}

TEST_CASE("a number's range decides what applies, said in its unit") {
    const double inf = std::numeric_limits<double>::infinity();
    // The mix applies above 4 steps; the meter while the mix is above 50 %.
    const auto d = fixture(true, [&](Meaning& m) {
        m.params[5].activeWhen = {{4, {}, 4.0, inf}};
        m.params[6].activeWhen = {{5, {}, 0.5, inf}};
    });
    REQUIRE(meaningProblems(*d, meaningOf(*d)).empty());
    const PatchCodec codec(*d);
    state::Snapshot s(*d);
    s.defaultProperties();
    const size_t steps = index(*d, 5);
    CHECK_FALSE(codec.activity(s)[5]);  // 4 steps: not above 4
    CHECK_FALSE(codec.activity(s)[6]);  // nor what follows the mix
    s.host[steps] = toHost(d->params().at(steps), 5.0);
    CHECK(codec.activity(s)[5]);
    CHECK(codec.activity(s)[6]);  // the mix's 100 % is above 50 %
    CHECK(d->applies(6, [&](size_t i) { return s.host[i]; }));
    const json::Value schema = codec.schemaDocument();
    const json::Value& mix = *schema.find("params")->find("mix")->find("active_when");
    CHECK(json::writeCompact(mix) == R"([{"param":"steps","above":4}])");
    const json::Value& meter = *schema.find("params")->find("meter")->find("active_when");
    CHECK(json::writeCompact(meter) == R"([{"param":"mix","above":50.0}])");
}

TEST_CASE("a condition names values of a toggle or a choice, or a number's range, above it") {
    const double inf = std::numeric_limits<double>::infinity();
    for (const ActiveCondition& bad :
         {ActiveCondition{5, {1}, -inf, inf}, ActiveCondition{4, {1}, -inf, inf},
          ActiveCondition{3, {2}, -inf, inf}, ActiveCondition{2, {}, -inf, inf},
          ActiveCondition{0, {0}, -inf, inf}, ActiveCondition{3, {}, 0.0, inf},
          ActiveCondition{4, {}, -inf, inf}, ActiveCondition{4, {}, 5.0, 3.0},
          ActiveCondition{2, {0}, 1.0, inf}}) {
        CAPTURE(bad.param);
        const auto d = fixture(true, [&](Meaning& m) { m.params[5].activeWhen = {bad}; });
        CHECK(meaningProblems(*d, meaningOf(*d)).find("applies on a condition") !=
              std::string::npos);
    }
}

TEST_CASE("a patch says each value in physical units, and only what changed when asked") {
    const auto d = fixture();
    const PatchCodec codec(*d);
    state::Snapshot s(*d);
    s.defaultProperties();
    const json::Value full = codec.patch(s, {}, false);
    const json::Value& params = *full.find("params");
    CHECK(params.find("mix")->asDouble() == 100.0);
    CHECK(params.find("filter.mode")->asString() == "low_pass");
    CHECK(params.find("on")->asBool());
    CHECK(params.find("steps")->isInt());
    CHECK(params.find("meter") == nullptr);
    CHECK(full.find("properties")->find("text")->asString() == "hello");
    CHECK(codec.patch(s, {}, true).find("params")->size() == 0);
    CHECK(codec.describe(s, true) == "Fixture: default settings");
}

TEST_CASE("a replacing patch sets what it says and defaults the rest") {
    const auto d = fixture();
    const PatchCodec codec(*d);
    state::Snapshot s(*d);
    Envelope env;
    std::vector<json::Error> errors;
    const json::Value doc = parsed(R"({"schema": "supple.fixture-patch/1.0", "name": "Bright",
        "params": {"gain": -6, "filter.cutoff": "1.2 kHz", "filter.mode": "High Pass", "mix": 50, "steps": 2},
        "properties": {"label": "lead"}})");
    REQUIRE_MESSAGE(codec.apply(doc, false, s, env, errors), json::errorsToJson(errors));
    CHECK(env.name == std::optional<std::string>("Bright"));
    CHECK(toPlain(d->params().at(0), s.host[0]) == doctest::Approx(-6.0));
    CHECK(toPlain(d->params().at(1), s.host[1]) == doctest::Approx(1200.0));
    CHECK(toPlain(d->params().at(2), s.host[2]) == 1.0);
    CHECK(toPlain(d->params().at(index(*d, 6)), s.host[index(*d, 6)]) == doctest::Approx(0.5));
    CHECK(s.property[0] == "lead");
    CHECK(s.property[1] == "hello");  // left out: its default
    const json::Value back = codec.patch(s, env, true);
    CHECK(back.find("params")->find("mix")->asDouble() == doctest::Approx(50.0));
    const std::string prose = codec.describe(s, true);
    CHECK(prose.find("Filter: Cutoff") != std::string::npos);
    CHECK(prose.size() <= 600);
}

TEST_CASE("the prose names a grouped parameter by what its group does not say") {
    PluginInfo info;
    info.id = "com.example.bands";
    info.name = "Bands";
    ParamTable::Builder b;
    b.add(toggle(1, "Band 1 On", false).in("Band 1"));
    b.add(decibels(2, "Band 1 Gain", -24.0, 24.0, 0.0).normalizedHostValue().in("Band 1"));
    b.add(toggle(3, "Auto Gain", true));
    const Definition d(info, b.build(), PropertyList::Builder().build());
    const PatchCodec codec(d);
    state::Snapshot s(d);
    Envelope env;
    std::vector<json::Error> errors;
    REQUIRE(codec.apply(parsed(R"({"params": {"band_1.band_1_on": true, "band_1.band_1_gain": -3,
        "auto_gain": false}})"),
                        true, s, env, errors));
    CHECK(codec.describe(s, true) == "Bands: Band 1: On, Gain -3.0 dB; Auto Gain off");
}

TEST_CASE("a file property takes an existing file by absolute path, or none") {
    const auto d = fixture();
    PatchCodec codec(*d);
    state::Snapshot s(*d);
    Envelope env;
    const std::filesystem::path folder =
        std::filesystem::temp_directory_path() / ("supple-semantic-" + std::to_string(::getpid()));
    std::filesystem::create_directories(folder);
    const std::string wav = (folder / "kick.wav").string(), text = (folder / "kick.txt").string();
    const std::string other = (folder / "snare.wav").string();
    std::ofstream(wav) << "RIFF";
    std::ofstream(other) << "RIFF";
    std::ofstream(text) << "text";
    auto set = [&](const std::string& path) {
        std::vector<json::Error> errors;
        json::Value doc = json::Value::object();
        doc["properties"]["sample"] = path;
        const bool ok = codec.apply(doc, true, s, env, errors);
        return ok ? std::string() : errors.at(0).code + " " + errors.at(0).message;
    };
    CHECK(set("kick.wav").rfind("properties.file 'sample': the path is not absolute", 0) == 0);
    CHECK(set((folder / "none.wav").string()).find("there is no file there") != std::string::npos);
    CHECK(set(text).find("it takes wav ") != std::string::npos);
    CHECK(set(wav).empty());
    CHECK(s.property[2] == wav);
    // The file check has the last word; a path it holds stays, found or not.
    codec.setFileCheck([](size_t, const std::string&, std::string& why) {
        why = "not audio";
        return false;
    });
    CHECK(set(wav).empty());
    CHECK(set(other).find("'sample': not audio") != std::string::npos);
    std::filesystem::remove_all(folder);
    CHECK(set(wav).empty());
    CHECK(set("").empty());
    CHECK(set(wav).find("there is no file there") != std::string::npos);
}

TEST_CASE("a merge changes only what it names") {
    const auto d = fixture();
    const PatchCodec codec(*d);
    state::Snapshot s(*d);
    Envelope env;
    std::vector<json::Error> errors;
    REQUIRE(codec.apply(parsed(R"({"schema": "supple.fixture-patch/1.0", "params": {"gain": 3}})"),
                        false, s, env, errors));
    REQUIRE(codec.apply(parsed(R"({"params": {"steps": 7}})"), true, s, env, errors));
    CHECK(toPlain(d->params().at(0), s.host[0]) == doctest::Approx(3.0));
    CHECK(toPlain(d->params().at(index(*d, 5)), s.host[index(*d, 5)]) == 7.0);
}

TEST_CASE("a bad patch changes nothing, and every error says where and how to fix it") {
    const auto d = fixture();
    const PatchCodec codec(*d);
    state::Snapshot s(*d);
    const std::vector<double> before = s.host;
    Envelope env;
    std::vector<json::Error> errors;
    CHECK_FALSE(codec.apply(parsed(R"({"schema": "supple.fixture-patch/1.0",
        "params": {"gian": 1, "mix": 150, "filter.mode": "band", "on": "yes", "meter": -3, "filter.cutoff": "3 dB"},
        "properties": {"label": "this label is far too long"}, "extra": 1})"),
                            false, s, env, errors));
    CHECK(s.host == before);
    auto has = [&](const char* code, const char* path) {
        for (const json::Error& e : errors)
            if (e.code == code && e.path == path && !e.hint.empty()) return true;
        return false;
    };
    CHECK(has("params.unknown_key", "/params/gian"));
    CHECK(has("params.out_of_range", "/params/mix"));
    CHECK(has("params.enum", "/params/filter.mode"));
    CHECK(has("params.type", "/params/on"));
    CHECK(has("params.readonly", "/params/meter"));
    CHECK(has("params.unit", "/params/filter.cutoff"));
    CHECK(has("properties.too_long", "/properties/label"));
    CHECK(has("patch.unknown_member", "/extra"));
    for (const json::Error& e : errors)
        if (e.code == "params.unknown_key") CHECK(e.hint.find("gain") != std::string::npos);
    errors.clear();
    CHECK_FALSE(
        codec.apply(parsed(R"({"schema": "supple.other-patch/1.0"})"), false, s, env, errors));
    CHECK(errors.at(0).code == "patch.wrong_type");
    errors.clear();
    CHECK_FALSE(codec.apply(parsed(R"({"params": {}})"), false, s, env, errors));
    CHECK(errors.at(0).code == "patch.missing_schema");
    // A newer minor version's additions are ignored.
    errors.clear();
    CHECK(codec.apply(parsed(R"({"schema": "supple.fixture-patch/1.3", "future": true})"), false, s,
                      env, errors));
}

TEST_CASE("a saved patch restores leniently and frames round trip") {
    const auto d = fixture();
    const PatchCodec codec(*d);
    state::Snapshot s(*d);
    Envelope env;
    codec.restore(parsed(R"({"schema": "supple.fixture-patch/1.0", "notes": "kept",
        "params": {"gain": -3, "removed": 1, "mix": 500}})"),
                  s, env);
    CHECK(toPlain(d->params().at(0), s.host[0]) == doctest::Approx(-3.0));
    CHECK(env.notes == std::optional<std::string>("kept"));
    const std::string text = json::writeCanonical(codec.patch(s, env, false));
    const auto framed = frame(text, state::fourcc("FiXt"));
    std::string back;
    CHECK(unframe(framed.data(), framed.size(), state::fourcc("FiXt"), back));
    CHECK(back == text);
    CHECK_FALSE(unframe(framed.data(), framed.size() - 1, state::fourcc("FiXt"), back));
    CHECK_FALSE(unframe(framed.data(), framed.size(), state::fourcc("Othr"), back));
}

TEST_CASE("a state's own values survive its patch exactly; the patch fills in the rest") {
    const auto d = fixture();
    const PatchCodec codec(*d);
    const Envelope none;
    int inexact = 0;
    for (int k = 1; k < 97; ++k) {
        state::Snapshot saved(*d);
        saved.host[index(*d, 1)] = k / 97.0;  // normalized: dB through a position
        saved.host[index(*d, 6)] = k / 97.0;  // a fraction written as a percent
        const json::Value patch = codec.patch(saved, none, false);
        state::Snapshot loaded = saved;  // what the framework's own chunk restored
        Envelope env;
        codec.restore(patch, loaded, env);
        CHECK(loaded.host == saved.host);
        // The patch alone would not have been exact for every value.
        state::Snapshot fresh(*d);
        codec.restore(patch, fresh, env);
        CHECK(json::writeCanonical(codec.patch(fresh, none, false)) == json::writeCanonical(patch));
        if (fresh.host != saved.host) ++inexact;
    }
    CHECK(inexact > 0);
}

TEST_CASE("a patch gotten and set again changes nothing, merged or replaced") {
    const auto d = fixture();
    const PatchCodec codec(*d);
    for (int k = 1; k < 97; ++k) {
        state::Snapshot s(*d);
        s.host[index(*d, 1)] = k / 97.0;  // through dB and back is not always exact
        s.host[index(*d, 2)] = k / 97.0;  // nor through Hz on a log taper
        Envelope env;
        const json::Value patch = codec.patch(s, env, false);
        for (bool merge : {false, true}) {
            state::Snapshot after = s;
            std::vector<json::Error> errors;
            REQUIRE_MESSAGE(codec.apply(patch, merge, after, env, errors),
                            json::errorsToJson(errors));
            CHECK(after.host == s.host);
        }
    }
    // A value set comes back as it was given, through a log taper too.
    for (double hz : {507.5, 20.0, 1234.567, 19999.9}) {
        state::Snapshot s(*d);
        Envelope env;
        std::vector<json::Error> errors;
        json::Value doc = parsed(R"({"params": {}})");
        doc["params"]["filter.cutoff"] = hz;
        REQUIRE_MESSAGE(codec.apply(doc, true, s, env, errors), json::errorsToJson(errors));
        CHECK(codec.patch(s, env, false).find("params")->find("filter.cutoff")->asDouble() == hz);
    }
}

TEST_CASE("a plugin without properties rejects property edits transactionally") {
    const auto base = fixture();
    ParamTable::Builder parameters;
    for (size_t i = 0; i < base->params().count(); ++i) parameters.add(base->params().at(i));
    const Definition d(base->info(), parameters.build());
    const PatchCodec codec(d);
    state::Snapshot snapshot(d);
    const auto before = snapshot.host;
    Envelope envelope;
    envelope.name = "Before";
    std::vector<json::Error> errors;
    const auto doc =
        parsed(R"({"name":"After","params":{"gain":-6},"properties":{"sample":"missing.wav"}})");
    CHECK_FALSE(codec.apply(doc, true, snapshot, envelope, errors));
    CHECK(snapshot.host == before);
    CHECK(envelope.name == std::optional<std::string>("Before"));
    REQUIRE(errors.size() == 1);
    CHECK(errors[0].code == "patch.unknown_member");
    CHECK(errors[0].path == "/properties");
    CHECK_FALSE(errors[0].hint.empty());
}
