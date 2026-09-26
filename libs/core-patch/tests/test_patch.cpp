#include <doctest/doctest.h>

#include <map>
#include <string>
#include <variant>
#include <vector>

#include <supple/json/json.h>
#include <supple/patch/patch.h>

using namespace supple;
using namespace supple::patch;
using supple::params::ParamSpec;
using supple::params::Type;
using supple::params::Unit;

namespace {

// --- structured fixture: a tiny modular synth body ---------------------------------

enum class Wave { saw, square };
struct Osc {
    Wave wave = Wave::saw;
    double level = -6;
    friend bool operator==(const Osc&, const Osc&) = default;
};
struct Noise {
    double level = -12;
    friend bool operator==(const Noise&, const Noise&) = default;
};
using Module = std::variant<Osc, Noise>;
struct Body {
    std::map<std::string, Module> modules;
    std::vector<double> steps;
    double cutoff = 1000;
    friend bool operator==(const Body&, const Body&) = default;
};

const PatchType<Body>& synthType() {
    static const PatchType<Body> type = [] {
        using namespace supple::schema;
        auto wave = enumeration<Wave>({{Wave::saw, "saw"}, {Wave::square, "square"}});
        auto osc = object<Osc>("osc")
                       .field("wave", &Osc::wave, wave)
                       .defaultValue(Wave::saw)
                       .field("level", &Osc::level, number().range(-60, 6).unit(Unit::dB))
                       .defaultValue(-6.0)
                       .build();
        auto noise = object<Noise>("noise")
                         .field("level", &Noise::level, number().range(-60, 6).unit(Unit::dB))
                         .defaultValue(-12.0)
                         .build();
        auto module = variant<Module>("module")
                          .alternative<Osc>("osc", osc)
                          .alternative<Noise>("noise", noise)
                          .build();
        auto body =
            object<Body>("body")
                .field("modules", &Body::modules,
                       mapOf(module).keyPattern("^[a-z][a-z0-9]*$", "module id"))
                .defaultValue(std::map<std::string, Module>{})
                .field("steps", &Body::steps, arrayOf(number().range(-100, 100)).maxItems(16))
                .defaultValue(std::vector<double>{})
                .field("cutoff", &Body::cutoff, number().range(20, 20000).unit(Unit::Hz))
                .defaultValue(1000.0)
                .build();
        // Two migrations: 1.x renamed "filter_cutoff" -> "cutoff"; 2.x had "voices" (dropped).
        std::vector<Migration> migrations = {[](json::Value doc) -> Result<json::Value> {
                                                 if (auto* fc = doc.find("filter_cutoff")) {
                                                     json::Value v = *fc;
                                                     doc.asObject().erase("filter_cutoff");
                                                     doc["cutoff"] = v;
                                                 }
                                                 doc["schema"] = "supple.toy-patch/2.0";
                                                 return doc;
                                             },
                                             [](json::Value doc) -> Result<json::Value> {
                                                 doc.asObject().erase("voices");
                                                 doc["schema"] = "supple.toy-patch/3.0";
                                                 return doc;
                                             }};
        return PatchType<Body>("toy", 3, 1, body, true, migrations);
    }();
    return type;
}

// --- flat fixture: a small EQ ------------------------------------------------------

constexpr supple::params::Choice kTypes[] = {{"bell", "Bell"}, {"high_pass", "High-pass"}};
constexpr ParamSpec kEq[] = {
    {.id = 0,
     .key = "band1.type",
     .name = "Type",
     .group = "Band 1",
     .type = Type::Enum,
     .max = 1,
     .def = 1,
     .choices = kTypes},
    {.id = 1,
     .key = "band1.freq",
     .name = "Freq",
     .group = "Band 1",
     .unit = Unit::Hz,
     .min = 20,
     .max = 20000,
     .def = 30,
     .curve = {supple::params::CurveKind::Log}},
    {.id = 2,
     .key = "band2.type",
     .name = "Type",
     .group = "Band 2",
     .type = Type::Enum,
     .max = 1,
     .def = 0,
     .choices = kTypes},
    {.id = 3,
     .key = "band2.freq",
     .name = "Freq",
     .group = "Band 2",
     .unit = Unit::Hz,
     .min = 20,
     .max = 20000,
     .def = 1000,
     .curve = {supple::params::CurveKind::Log}},
    {.id = 4,
     .key = "band2.gain",
     .name = "Gain",
     .group = "Band 2",
     .unit = Unit::dB,
     .min = -30,
     .max = 30,
     .def = 0,
     .digits = 2},
    {.id = 5, .key = "mix", .name = "Mix", .unit = Unit::percent, .min = 0, .max = 100, .def = 100},
    {.id = 6,
     .key = "meter",
     .name = "Meter",
     .unit = Unit::dB,
     .min = -120,
     .max = 6,
     .def = -120,
     .flags = supple::params::kReadonly},
};

const FlatPatchType& eqType() {
    static const FlatPatchType type("eq", 1, 0, kEq, false);
    return type;
}

}  // namespace

TEST_CASE("structured patch: parse, canonical serialize, idempotent") {
    const auto& t = synthType();
    auto p = t.parse(R"({"cutoff": "1.2 kHz", "schema": "supple.toy-patch/3.1", "name": "Buzz",
                        "modules": {"osc1": {"type": "osc", "wave": "square"}, "n": {"type": "noise"}},
                        "seed": 7})");
    REQUIRE(p);
    CHECK(p->meta.name == "Buzz");
    CHECK(p->meta.seed == 7u);
    CHECK(p->body.cutoff == doctest::Approx(1200));
    CHECK(std::get<Osc>(p->body.modules.at("osc1")).wave == Wave::square);
    const std::string text = t.serialize(*p);
    CHECK(text.rfind(
              "{\n  \"schema\": \"supple.toy-patch/3.1\",\n  \"name\": \"Buzz\",\n  \"seed\": 7,",
              0) == 0);
    auto again = t.parse(text);
    REQUIRE(again);
    CHECK(t.serialize(*again) == text);
    CHECK(again->body == p->body);
    CHECK(again->meta == p->meta);
}

TEST_CASE("version handling: wrong type, too new, newer minor, older majors migrate") {
    const auto& t = synthType();
    auto wrong = t.parse(R"({"schema": "supple.eq-patch/1.0"})");
    REQUIRE_FALSE(wrong);
    CHECK(wrong.error().code == "patch.wrong_type");
    CHECK(wrong.error().hint.find("supple.toy-patch/3.1") != std::string::npos);
    auto future = t.parse(R"({"schema": "supple.toy-patch/4.0"})");
    REQUIRE_FALSE(future);
    CHECK(future.error().code == "patch.too_new");
    auto bad = t.parse(R"({"schema": "toy"})");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "patch.bad_schema_id");
    auto missing = t.parse(R"({"cutoff": 100})");
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "patch.missing_schema");

    // Same major, older/equal minor: strict.
    auto strict = t.parse(R"({"schema": "supple.toy-patch/3.0", "cutoff": 500, "sparkle": 1})");
    REQUIRE_FALSE(strict);
    // Newer minor: unknown members ignored with warnings.
    std::vector<Warning> warnings;
    auto lenient =
        t.parse(R"({"schema": "supple.toy-patch/3.9", "cutoff": 500, "sparkle": 1})", &warnings);
    INFO((lenient ? std::string() : lenient.error().toString()));
    REQUIRE(lenient);
    CHECK(lenient->body.cutoff == 500);
    CHECK(warnings.size() >= 2);

    // Major 1 -> 2 -> 3.
    auto old = t.parse(R"({"schema": "supple.toy-patch/1.4", "filter_cutoff": 440, "voices": 8})");
    REQUIRE(old);
    CHECK(old->body.cutoff == 440);
}

TEST_CASE("merge: transactional, paths refer to the merged document") {
    const auto& t = synthType();
    auto base =
        t.parse(R"({"schema": "supple.toy-patch/3.1", "modules": {"osc1": {"type": "osc"}}})");
    REQUIRE(base);
    auto merged = t.merge(
        *base,
        *json::parse(R"({"cutoff": 800, "modules": {"n": {"type": "noise", "level": -20}}})"));
    REQUIRE(merged);
    CHECK(merged->body.cutoff == 800);
    CHECK(merged->body.modules.size() == 2);
    auto removed = t.merge(*merged, *json::parse(R"({"modules": {"n": null}})"));
    REQUIRE(removed);
    CHECK(removed->body.modules.size() == 1);
    auto failing =
        t.merge(*merged, *json::parse(R"({"cutoff": 5, "modules": {"osc1": {"wave": "sine"}}})"));
    REQUIRE_FALSE(failing);
    CHECK(failing.error().code == "schema.invalid");
    std::vector<std::string> paths;
    for (const auto& d : failing.error().details) paths.push_back(d.path);
    CHECK(std::find(paths.begin(), paths.end(), "/cutoff") != paths.end());
    CHECK(std::find(paths.begin(), paths.end(), "/modules/osc1/wave") != paths.end());
    CHECK(merged->body.cutoff == 800);  // unchanged: merge returns a new value or an error
    auto otherType = t.merge(*merged, *json::parse(R"({"schema": "supple.eq-patch/1.0"})"));
    REQUIRE_FALSE(otherType);
    CHECK(otherType.error().code == "patch.wrong_type");
}

TEST_CASE("diff then merge reproduces the target") {
    const auto& t = synthType();
    auto a = t.parse(
        R"({"schema": "supple.toy-patch/3.1", "modules": {"osc1": {"type": "osc"}}, "steps": [1, 2]})");
    auto b = t.parse(R"({"schema": "supple.toy-patch/3.1", "name": "B", "cutoff": 3000,
                        "modules": {"n": {"type": "noise"}}, "steps": [5]})");
    REQUIRE(a);
    REQUIRE(b);
    json::Value d = t.diff(*a, *b);
    auto rebuilt = t.merge(*a, d);
    REQUIRE(rebuilt);
    CHECK(rebuilt->body == b->body);
    CHECK(rebuilt->meta == b->meta);
    CHECK(t.serialize(*rebuilt) == t.serialize(*b));
}

TEST_CASE("flat patch: defaults, changed detail, re-read, errors with paths") {
    const auto& t = eqType();
    auto d = t.defaults();
    CHECK(t.serialize(d, Detail::Changed) ==
          "{\n  \"schema\": \"supple.eq-patch/1.0\",\n  \"params\": {}\n}\n");
    auto p = t.parse(R"({"schema": "supple.eq-patch/1.0", "name": "Vocal cleanup",
                        "params": {"band1.freq": "90 Hz", "band2.freq": 250, "band2.gain": -3}})");
    REQUIRE(p);
    const std::string changed = t.serialize(*p, Detail::Changed);
    CHECK(changed ==
          "{\n  \"schema\": \"supple.eq-patch/1.0\",\n  \"name\": \"Vocal cleanup\",\n  "
          "\"params\": {\n"
          "    \"band1.freq\": 90.0,\n    \"band2.freq\": 250.0,\n    \"band2.gain\": -3.0\n  "
          "}\n}\n");
    auto reread = t.parse(changed);
    REQUIRE(reread);
    CHECK(reread->body == p->body);
    const std::string full = t.serialize(*p);
    CHECK(full.find("\"band1.type\": \"high_pass\"") != std::string::npos);
    CHECK(full.find("\"meter\"") == std::string::npos);  // readonly never written

    auto bad = t.parse(R"({"schema": "supple.eq-patch/1.0",
                          "params": {"band2.gian": -3, "band1.freq": "5 Hz", "meter": 0, "band1.type": "shelf"},
                          "extra": 1})");
    REQUIRE_FALSE(bad);
    CHECK(bad.error().code == "schema.invalid");
    std::map<std::string, std::string> byPath;
    for (const auto& e : bad.error().details) byPath[e.path] = e.code;
    CHECK(byPath["/params/band2.gian"] == "params.unknown_key");
    CHECK(byPath["/params/band1.freq"] == "params.out_of_range");
    CHECK(byPath["/params/meter"] == "params.readonly");
    CHECK(byPath["/params/band1.type"] == "params.enum");
    CHECK(byPath["/extra"] == "schema.unknown_member");
    auto seed = t.parse(R"({"schema": "supple.eq-patch/1.0", "seed": 3})");
    REQUIRE_FALSE(seed);
    CHECK(seed.error().path == "/seed");
}

TEST_CASE("flat patch merge and diff") {
    const auto& t = eqType();
    auto base = t.defaults();
    auto m = t.merge(
        base, *json::parse(R"({"params": {"band2.gain": "-3 dB", "band2.freq": "250 Hz"}})"));
    REQUIRE(m);
    CHECK(m->body.values[4] == -3);
    auto fail = t.merge(*m, *json::parse(R"({"params": {"band2.gain": 40}})"));
    REQUIRE_FALSE(fail);
    CHECK(fail.error().path == "/params/band2.gain");
    json::Value d = t.diff(base, *m);
    CHECK(json::writeCompact(d) == R"({"params":{"band2.freq":250.0,"band2.gain":-3.0}})");
    auto back = t.merge(base, d);
    REQUIRE(back);
    CHECK(back->body == m->body);
    auto schemaDoc = t.jsonSchema();
    CHECK(schemaDoc.find("properties")->find("params")->find("properties")->find("band2.gain") !=
          nullptr);
    CHECK(schemaDoc.find("properties")->find("params")->find("properties")->find("meter") ==
          nullptr);
}

TEST_CASE("state framing") {
    const std::string doc = "{\"schema\":\"supple.eq-patch/1.0\"}";
    auto bytes = encodeState(doc, 0x51455131).value();
    CHECK(isFramedState(bytes, 0x51455131));
    CHECK_FALSE(isFramedState(bytes, 0x51455132));
    auto decoded = decodeState(bytes, 0x51455131);
    REQUIRE(decoded);
    CHECK(*decoded == doc);
    CHECK(decodeState(bytes, 0x12345678).error().code == "patch.state.magic");
    auto truncated = bytes;
    truncated.pop_back();
    CHECK(decodeState(truncated, 0x51455131).error().code == "patch.state.truncated");
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK(decodeState(trailing, 0x51455131).error().code == "patch.state.trailing");
    auto v1 = bytes;
    v1[4] = 1;
    CHECK(decodeState(v1, 0x51455131).error().code == "patch.state.version");
    CHECK_FALSE(isFramedState(v1, 0x51455131));
    CHECK(isFramedState(truncated, 0x51455131));  // claims the frame: corrupt, not legacy
    auto huge = bytes;
    huge[11] = 0x7f;
    CHECK(decodeState(huge, 0x51455131).error().code == "patch.state.too_large");
    std::vector<std::uint8_t> short4 = {1, 2, 3, 4};
    CHECK(decodeState(short4, 0x51455131).error().code == "patch.state.truncated");
    auto badUtf8 = encodeState("{\"a\":\"\xff\"}", 7).value();
    CHECK(decodeState(badUtf8, 7).error().code == "patch.state.utf8");
    CHECK(encodeState(std::string(kMaxStateBytes + 1, ' '), 7).error().code ==
          "patch.state.too_large");
}

TEST_CASE("describeFlat") {
    const auto& t = eqType();
    CHECK(describeFlat(t, t.defaults()) == "default settings");
    auto p = t.merge(
        t.defaults(),
        *json::parse(
            R"({"params": {"band1.freq": 90, "band2.freq": 250, "band2.gain": -3, "mix": 80}})"));
    REQUIRE(p);
    CHECK(describeFlat(t, *p) ==
          "band1: Freq 90.0 Hz; band2: Freq 250 Hz, Gain -3.0 dB; Mix 80.0 %");
    CHECK(describeFlat(t, *p, Detail::Full).find("band1: Type High-pass, Freq 90.0 Hz") == 0);
}
