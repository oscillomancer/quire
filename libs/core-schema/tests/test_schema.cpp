#include <doctest/doctest.h>
#include <supple/schema/schema.h>
#include <filesystem>
#include <fstream>
#include <limits>
using namespace supple;
using namespace supple::schema;
struct Filter {
    double cutoff = 0;
    std::string mode;
    std::optional<double> drive;
};
TEST_CASE("object decode encode and errors") {
    auto s = object<Filter>("filter")
                 .field("cutoff", &Filter::cutoff, number().range(20, 20000).unit(Unit::Hz))
                 .defaultValue(1000.0)
                 .field("mode", &Filter::mode, string())
                 .field("drive", &Filter::drive, optional(number().unit(Unit::dB)))
                 .build();
    auto v = json::parse("{\"mode\":\"lowpass\",\"drive\":\"3 dB\"}");
    REQUIRE(v);
    auto r = s.decode(*v);
    REQUIRE(r);
    CHECK(r->cutoff == 1000);
    CHECK(r->drive == 3);
    auto encoded = s.encode(*r);
    CHECK(encoded.find("cutoff")->asDouble() == 1000);
    auto bad = json::parse("{\"cuttoff\":5}");
    REQUIRE(bad);
    auto error = s.decode(*bad);
    REQUIRE_FALSE(error);
    CHECK(error.error().code == "schema.invalid");
    CHECK(error.error().details.size() == 2);
}
TEST_CASE("units") {
    auto n = parseQuantity("1.2 kHz", Unit::Hz);
    REQUIRE(n);
    CHECK(*n == 1200);
    CHECK(parseQuantity("0.25 s", Unit::ms).value() == 250);
    CHECK_FALSE(parseQuantity("5 ms", Unit::Hz));
}
TEST_CASE("enum, arrays, maps and variant") {
    enum class Wave { sine, saw };
    struct Osc {
        Wave wave = Wave::sine;
    };
    struct Noise {
        int seed = 0;
    };
    using Module = std::variant<Osc, Noise>;
    auto wave = enumeration<Wave>({{Wave::sine, "sine"}, {Wave::saw, "saw"}});
    auto osc = object<Osc>("osc").field("wave", &Osc::wave, wave).build();
    auto noise = object<Noise>("noise").field("seed", &Noise::seed, integerOf<int>()).build();
    auto modules = variant<Module>("module")
                       .alternative<Osc>("osc", osc)
                       .alternative<Noise>("noise", noise)
                       .build();
    auto list = arrayOf(modules).minItems(1);
    auto v = json::parse("[{\"type\":\"osc\",\"wave\":\"saw\"},{\"type\":\"noise\",\"seed\":7}]");
    REQUIRE(v);
    auto r = list.decode(*v);
    REQUIRE(r);
    CHECK(std::get<Osc>(r->at(0)).wave == Wave::saw);
    CHECK(json::writeCompact(list.encode(*r)) == json::writeCompact(*v));
    auto bad = json::parse("[{\"type\":\"oc\",\"wave\":\"saw\"}]");
    REQUIRE(bad);
    auto e = list.decode(*bad);
    REQUIRE_FALSE(e);
    CHECK(e.error().code == "schema.unknown_variant");
    CHECK(e.error().hint.find("osc") != std::string::npos);
    auto m = mapOf(integerOf<int>()).keyPattern("^[a-z]+$", "lowercase key");
    auto input = json::parse("{\"Bad\":1}");
    REQUIRE(input);
    auto mr = m.decode(*input);
    REQUIRE_FALSE(mr);
    CHECK(mr.error().code == "schema.key");
}
TEST_CASE("schema constraints and warnings") {
    auto limited = number().range(20, 20000).unit(Unit::Hz);
    auto high = json::Value("21 kHz");
    auto e = limited.decode(high);
    REQUIRE_FALSE(e);
    CHECK(e.error().code == "schema.out_of_range");
    CHECK(e.error().hint.find("Hz") != std::string::npos);
    auto str = string().minLength(2).maxLength(5).pattern("^[a-z]+$", "lowercase letters");
    auto s = str.decode(json::Value("A"));
    REQUIRE_FALSE(s);
    CHECK(s.error().code == "schema.length");
    s = str.decode(json::Value("AA"));
    REQUIRE_FALSE(s);
    CHECK(s.error().code == "schema.pattern");
    auto arr = arrayOf(integerOf<int>()).uniqueItems();
    auto d = json::parse("[1,2,1]");
    REQUIRE(d);
    auto duplicate = arr.decode(*d);
    REQUIRE_FALSE(duplicate);
    CHECK(duplicate.error().code == "schema.unique");
    CHECK(duplicate.error().hint.find("0") != std::string::npos);
    struct Config {
        int value = 0;
    };
    auto schema = object<Config>("config").field("value", &Config::value, integerOf<int>()).build();
    auto v = json::parse("{\"value\":1,\"valu\":2}");
    REQUIRE(v);
    std::vector<Warning> warnings;
    DecodeOptions options;
    options.unknownMembers = UnknownMembers::Ignore;
    auto decoded = schema.decode(*v, options, &warnings);
    REQUIRE(decoded);
    CHECK(warnings.size() == 1);
    CHECK(warnings.front().path == "/valu");
    auto invalid = schema.decode(*v);
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "schema.unknown_member");
    CHECK(invalid.error().hint.find("value") != std::string::npos);
}
TEST_CASE("unit aliases") {
    struct Alias {
        const char* text;
        Unit unit;
        double value;
    };
    const Alias aliases[] = {
        {"1.2k", Unit::Hz, 1200},      {"2 khz", Unit::Hz, 2000},   {"0.25 s", Unit::ms, 250},
        {"1000 us", Unit::ms, 1},      {"1000 µs", Unit::ms, 1},    {"-3db", Unit::dB, -3},
        {"-14 LKFS", Unit::LUFS, -14}, {"3 LU", Unit::LU, 3},       {"7 semi", Unit::st, 7},
        {"50 cents", Unit::ct, 50},    {"2 octaves", Unit::oct, 2}, {"50%", Unit::percent, 50},
        {"4:1", Unit::ratio, 4},       {"2×", Unit::x, 2},          {"90°", Unit::deg, 90},
        {"120 bpm", Unit::bpm, 120},   {"4 qn", Unit::beats, 4},    {"512 smp", Unit::samples, 512},
        {"3", Unit::none, 3}};
    for (const auto& a : aliases) {
        auto r = parseQuantity(a.text, a.unit);
        INFO(a.text);
        REQUIRE(r);
        CHECK(*r == a.value);
    }
}
TEST_CASE("schema error codes") {
    auto type = number().decode(json::Value(true));
    REQUIRE_FALSE(type);
    CHECK(type.error().code == "schema.type");
    auto nonfinite = number().decode(json::Value(std::numeric_limits<double>::infinity()));
    REQUIRE_FALSE(nonfinite);
    CHECK(nonfinite.error().code == "schema.not_finite");
    auto integral = integer().decode(json::Value(1.5));
    REQUIRE_FALSE(integral);
    CHECK(integral.error().code == "schema.integer_required");
    CHECK(integral.error().hint.find("nearest integer") != std::string::npos);
    auto unit = number().unit(Unit::Hz).decode(json::Value("2 ms"));
    REQUIRE_FALSE(unit);
    CHECK(unit.error().code == "schema.unit");
    auto empty = arrayOf(integer()).minItems(1).decode(json::Value::array());
    REQUIRE_FALSE(empty);
    CHECK(empty.error().code == "schema.items");
    auto wave = enumeration<int>({{0, "sine"}, {1, "saw"}}).decode(json::Value("sow"));
    REQUIRE_FALSE(wave);
    CHECK(wave.error().code == "schema.enum");
    CHECK(wave.error().hint.find("saw") != std::string::npos);
    struct A {
        int value = 0;
    };
    using V = std::variant<A, std::string>;
    auto a = object<A>("a").field("value", &A::value, integerOf<int>()).build();
    auto v = variant<V>("variant").alternative<A>("a", a).build();
    auto missing = v.decode(json::Value::object());
    REQUIRE_FALSE(missing);
    CHECK(missing.error().code == "schema.missing_tag");
    auto capped = json::parse("{}");
    REQUIRE(capped);
    DecodeOptions opts;
    opts.maxErrors = 1;
    auto required = a.decode(*capped, opts);
    REQUIRE_FALSE(required);
    CHECK(required.error().code == "schema.invalid");
    CHECK(required.error().details.back().code == "schema.too_many_errors");
    auto check = number()
                     .check([](double numberValue, CheckCtx& c) {
                         if (numberValue > 0)
                             c.error("schema.check.positive", "positive forbidden", "use zero");
                     })
                     .decode(json::Value(1.0));
    REQUIRE_FALSE(check);
    CHECK(check.error().code == "schema.check.positive");
}
TEST_CASE("JSON Schema golden") {
    struct Config {
        double cutoff = 0;
        std::string mode;
    };
    auto schema = object<Config>("config")
                      .doc("Filter configuration")
                      .field("cutoff", &Config::cutoff, number().range(20, 20000).unit(Unit::Hz))
                      .defaultValue(1000.0)
                      .field("mode", &Config::mode, string())
                      .build();
    std::ifstream in(std::filesystem::path(__FILE__).parent_path() / "config-schema.golden.json");
    std::string expected((std::istreambuf_iterator<char>(in)), {});
    CHECK(json::writeCanonical(schema.jsonSchema()) == expected);
}
TEST_CASE("float field binding") {
    struct Gain {
        float level = 0;
    };
    auto schema = object<Gain>("gain")
                      .field("level", &Gain::level, number().range(-60, 12).unit(Unit::dB))
                      .build();
    auto input = json::parse("{\"level\":\"-3 dB\"}");
    REQUIRE(input);
    auto decoded = schema.decode(*input);
    REQUIRE(decoded);
    CHECK(decoded->level == -3.0f);
    CHECK(schema.encode(*decoded).find("level")->asDouble() == -3.0);
}
