// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <clap/ext/params.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>

#include <supple/json/json.h>
#include <supple/params/params.h>

using namespace supple;
using namespace supple::params;

namespace {

constexpr Choice kModes[] = {
    {"lowpass", "Low-pass"}, {"bandpass", "Band-pass"}, {"highpass", "High-pass"}};
constexpr std::string_view kBright[] = {"role:brightness", "stage:filter"};
constexpr std::string_view kLevel[] = {"role:level"};

constexpr ParamSpec kTable[] = {
    {.id = 0,
     .key = "filter.cutoff",
     .name = "Cutoff",
     .group = "Filter",
     .type = Type::Float,
     .unit = Unit::Hz,
     .min = 20,
     .max = 20000,
     .def = 1000,
     .curve = {CurveKind::Log},
     .digits = 3,
     .tags = kBright,
     .description = "Low-pass corner frequency.",
     .flags = kAutomatable | kModulatable,
     .smoothingMs = 20},
    {.id = 1,
     .key = "filter.mode",
     .name = "Mode",
     .group = "Filter",
     .type = Type::Enum,
     .min = 0,
     .max = 2,
     .def = 0,
     .choices = kModes,
     .flags = kAutomatable},
    {.id = 2,
     .key = "env.attack",
     .name = "Attack",
     .group = "Envelope",
     .type = Type::Float,
     .unit = Unit::ms,
     .min = 0,
     .max = 10000,
     .def = 5,
     .curve = {CurveKind::Pow, 3.0},
     .digits = 3,
     .flags = kAutomatable | kModulatable},
    {.id = 3,
     .key = "osc.fine",
     .name = "Fine",
     .group = "Oscillator",
     .type = Type::Float,
     .unit = Unit::ct,
     .min = -100,
     .max = 100,
     .def = 0,
     .curve = {CurveKind::Bipolar, 2.0},
     .digits = 3,
     .flags = kAutomatable | kModulatable},
    {.id = 4,
     .key = "voice.count",
     .name = "Voices",
     .group = "Voice",
     .type = Type::Int,
     .min = 1,
     .max = 16,
     .def = 8,
     .flags = kAutomatable},
    {.id = 5,
     .key = "out.level",
     .name = "Level",
     .group = "Output",
     .type = Type::Float,
     .unit = Unit::dB,
     .min = -60,
     .max = 12,
     .def = 0,
     .curve = {CurveKind::Linear},
     .digits = 2,
     .tags = kLevel,
     .flags = kAutomatable | kModulatable | kOffAtMin},
    {.id = 6,
     .key = "bypass",
     .name = "Bypass",
     .type = Type::Bool,
     .min = 0,
     .max = 1,
     .def = 0,
     .flags = kAutomatable | kBypass},
    {.id = 7,
     .key = "meter.out",
     .name = "Output Level",
     .group = "Meters",
     .type = Type::Float,
     .unit = Unit::dB,
     .min = -120,
     .max = 6,
     .def = -120,
     .digits = 3,
     .flags = kReadonly},
    {.id = 8,
     .key = "mix",
     .name = "Mix",
     .type = Type::Float,
     .unit = Unit::percent,
     .min = 0,
     .max = 100,
     .def = 100,
     .digits = 3,
     .flags = kAutomatable},
};
constexpr Table kT{kTable};

const ParamSpec& P(std::string_view key) { return *findByKey(kT, key); }

}  // namespace

TEST_CASE("the fixture table is valid") {
    auto s = validateTable(kT);
    INFO((s.ok() ? std::string() : s.error().toString()));
    CHECK(s.ok());
}

TEST_CASE("table validation rejects every rule violation") {
    constexpr Choice oneChoice[] = {{"only", "Only"}};
    constexpr Choice badChoice[] = {{"Bad", "Bad"}, {"ok", "Ok"}};
    constexpr std::string_view badTag[] = {"brightness"};
    struct Case {
        ParamSpec spec;
        const char* code;
    };
    const Case cases[] = {
        {{.id = 0, .key = "Bad.Key", .name = "X"}, "params.table.bad_key"},
        {{.id = 0, .key = "x", .name = "X", .min = 1, .max = 1, .def = 1}, "params.table.range"},
        {{.id = 0,
          .key = "x",
          .name = "X",
          .unit = Unit::Hz,
          .min = 0,
          .max = 10,
          .def = 1,
          .curve = {CurveKind::Log}},
         "params.table.log_min"},
        {{.id = 0,
          .key = "x",
          .name = "X",
          .min = 0,
          .max = 1,
          .def = 0.5,
          .curve = {CurveKind::Pow, 0}},
         "params.table.curve_k"},
        {{.id = 0, .key = "x", .name = "X", .type = Type::Int, .min = 0, .max = 3.5, .def = 1},
         "params.table.int_range"},
        {{.id = 0, .key = "x", .name = "X", .type = Type::Bool, .min = 0, .max = 2, .def = 0},
         "params.table.bool_range"},
        {{.id = 0, .key = "x", .name = "X", .type = Type::Enum, .max = 0, .choices = oneChoice},
         "params.table.choices"},
        {{.id = 0, .key = "x", .name = "X", .type = Type::Enum, .max = 1, .choices = badChoice},
         "params.table.choice_key"},
        {{.id = 0, .key = "x", .name = "X", .min = 0, .max = 1, .def = 2}, "params.table.default"},
        {{.id = 0, .key = "x", .name = "X", .tags = badTag}, "params.table.tag"},
        {{.id = 0, .key = "x", .name = "X", .flags = kReadonly | kModulatable},
         "params.table.readonly_mod"},
        {{.id = 0, .key = "x", .name = "X", .flags = kBypass}, "params.table.bypass_type"},
        {{.id = 0, .key = "x", .name = ""}, "params.table.no_name"},
    };
    for (const Case& c : cases) {
        const ParamSpec one[] = {c.spec};
        auto s = validateTable(one);
        REQUIRE_FALSE(s.ok());
        INFO(c.code, " got ", s.error().toString());
        const bool found = s.error().code == c.code ||
                           std::any_of(s.error().details.begin(), s.error().details.end(),
                                       [&](const Error& e) { return e.code == c.code; });
        CHECK(found);
    }
    const ParamSpec dup[] = {{.id = 1, .key = "a", .name = "A"},
                             {.id = 1, .key = "a", .name = "B"}};
    auto d = validateTable(dup);
    REQUIRE_FALSE(d.ok());
    CHECK(d.error().details.size() == 2);
}

TEST_CASE("normalization round-trips to 1e-9 relative on every curve, endpoints exact") {
    for (const char* key : {"filter.cutoff", "env.attack", "osc.fine", "out.level", "mix"}) {
        const ParamSpec& p = P(key);
        INFO(key);
        CHECK(toNormalized(p, p.min) == 0.0);
        CHECK(toNormalized(p, p.max) == 1.0);
        CHECK(fromNormalized(p, 0.0) == p.min);
        CHECK(fromNormalized(p, 1.0) == p.max);
        for (int i = 0; i <= 10000; ++i) {
            const double v = fromNormalized(p, i / 10000.0);
            const double back = fromNormalized(p, toNormalized(p, v));
            const double tol = std::max(1e-12, 1e-9 * std::fabs(v));
            if (std::fabs(back - v) > tol) {
                FAIL_CHECK("round trip failed at ", v, " -> ", back);
                break;
            }
        }
    }
    // Log: equal steps in normalized domain are equal frequency ratios.
    const ParamSpec& cutoff = P("filter.cutoff");
    CHECK(fromNormalized(cutoff, 0.5) == doctest::Approx(std::sqrt(20.0 * 20000.0)));
    // Bipolar: centre maps to the midpoint exactly.
    CHECK(fromNormalized(P("osc.fine"), 0.5) == 0.0);
    CHECK(toNormalized(P("osc.fine"), 0.0) == 0.5);
    // NaN never escapes.
    CHECK(clampPhysical(cutoff, std::numeric_limits<double>::quiet_NaN()) == 1000.0);
    CHECK(fromNormalized(cutoff, std::numeric_limits<double>::quiet_NaN()) == 1000.0);
}

TEST_CASE("CLAP value domain per ADR-0010") {
    const ParamSpec& cutoff = P("filter.cutoff");
    CHECK(toClap(cutoff, 20000) == 1.0);
    CHECK(fromClap(cutoff, toClap(cutoff, 1234.5)) == doctest::Approx(1234.5).epsilon(1e-12));
    auto r = clapRange(cutoff);
    CHECK(r.min == 0.0);
    CHECK(r.max == 1.0);
    CHECK(r.def == doctest::Approx(toNormalized(cutoff, 1000)));
    CHECK_FALSE(r.stepped);
    const ParamSpec& mode = P("filter.mode");
    CHECK(toClap(mode, 2) == 2.0);
    auto mr = clapRange(mode);
    CHECK(mr.stepped);
    CHECK(mr.isEnum);
    CHECK((clapFlags(mode) & CLAP_PARAM_IS_ENUM) != 0);
    CHECK((clapFlags(mode) & CLAP_PARAM_IS_STEPPED) != 0);
    CHECK((clapFlags(cutoff) & CLAP_PARAM_IS_MODULATABLE) != 0);
    CHECK((clapFlags(P("bypass")) & CLAP_PARAM_IS_BYPASS) != 0);
    CHECK((clapFlags(P("meter.out")) & CLAP_PARAM_IS_READONLY) != 0);
    CHECK((clapFlags(P("meter.out")) & CLAP_PARAM_IS_AUTOMATABLE) == 0);
    CHECK(fromClap(P("voice.count"), 7.6) == 8.0);
}

TEST_CASE("format and compact format") {
    CHECK(format(P("filter.cutoff"), 1200) == "1.20 kHz");
    CHECK(format(P("filter.cutoff"), 250) == "250 Hz");
    CHECK(format(P("env.attack"), 1500) == "1.50 s");
    CHECK(format(P("env.attack"), 5) == "5.00 ms");
    CHECK(format(P("out.level"), -3) == "-3.0 dB");
    CHECK(format(P("out.level"), 3) == "+3.0 dB");
    CHECK(format(P("out.level"), -60) == "-inf dB");
    CHECK(format(P("osc.fine"), 7) == "+7.00 ct");
    CHECK(format(P("filter.mode"), 1) == "Band-pass");
    CHECK(format(P("bypass"), 1) == "on");
    CHECK(format(P("voice.count"), 8) == "8");
    CHECK(format(P("mix"), 50) == "50.0 %");
    CHECK(formatCompact(P("filter.cutoff"), 1200) == "1.20k");
    CHECK(formatCompact(P("out.level"), -60) == "-inf");
    for (const ParamSpec& p : kT) {
        const double values[] = {p.min, p.def, p.max, 0.5 * (p.min + p.max)};
        for (double v : values) CHECK(formatCompact(p, v).size() <= 6);
    }
}

TEST_CASE("parse accepts units, labels and words; rejects junk and out-of-range") {
    CHECK(*parse(P("filter.cutoff"), "1.2 kHz") == doctest::Approx(1200));
    CHECK(*parse(P("filter.cutoff"), " 1200 ") == doctest::Approx(1200));
    CHECK(*parse(P("filter.cutoff"), "1.2k") == doctest::Approx(1200));
    CHECK(*parse(P("env.attack"), "0.25 s") == doctest::Approx(250));
    CHECK(*parse(P("out.level"), "-inf") == -60);
    CHECK(*parse(P("out.level"), "+3.0 dB") == doctest::Approx(3));
    CHECK(*parse(P("filter.mode"), "HIGHPASS") == 2);
    CHECK(*parse(P("filter.mode"), "Band-pass") == 1);
    CHECK(*parse(P("filter.mode"), "0") == 0);
    CHECK(*parse(P("bypass"), "true") == 1);
    CHECK(*parse(P("voice.count"), "3.6") == 4);
    auto junk = parse(P("filter.cutoff"), "1200 Hz please");
    REQUIRE_FALSE(junk);
    CHECK(junk.error().code == "params.parse");
    CHECK(junk.error().hint.find("1.00 kHz") != std::string::npos);
    auto high = parse(P("filter.cutoff"), "25 kHz");
    REQUIRE_FALSE(high);
    CHECK(high.error().code == "params.out_of_range");
    CHECK(high.error().hint == "valid range: 20.0 Hz .. 20.0 kHz");
    auto wrongUnit = parse(P("filter.cutoff"), "5 ms");
    REQUIRE_FALSE(wrongUnit);
    CHECK(wrongUnit.error().code == "params.parse");
    auto badEnum = parse(P("filter.mode"), "lowpas");
    REQUIRE_FALSE(badEnum);
    CHECK(badEnum.error().code == "params.enum");
    CHECK(badEnum.error().hint.find("\"lowpass\"") != std::string::npos);
    // Every value survives format -> parse within display precision.
    for (const ParamSpec& p : kT) {
        if (p.flags & kReadonly) continue;
        for (int i = 0; i <= 20; ++i) {
            const double v = fromNormalized(p, i / 20.0);
            auto back = parse(p, format(p, v));
            INFO(p.key, " ", format(p, v));
            REQUIRE(back);
            if (p.type == Type::Float) {
                if ((p.flags & kOffAtMin) && v <= p.min) continue;
                // Within half of the last displayed digit (digits are significant digits;
                // Hz and ms may display scaled by 1000, which only shrinks the step).
                const double mag = v == 0 ? 0 : std::floor(std::log10(std::fabs(v)));
                const double step = std::pow(10.0, std::max(mag - p.digits + 1, -6.0));
                CHECK(std::fabs(*back - v) <= 0.5 * step + 1e-9);
            } else {
                CHECK(*back == v);
            }
        }
    }
}

TEST_CASE("JSON values in physical units") {
    CHECK(json::writeCompact(toJson(P("filter.cutoff"), 1200)) == "1200.0");
    CHECK(json::writeCompact(toJson(P("filter.mode"), 2)) == "\"highpass\"");
    CHECK(json::writeCompact(toJson(P("bypass"), 1)) == "true");
    CHECK(json::writeCompact(toJson(P("voice.count"), 8)) == "8");
    CHECK(*fromJson(P("filter.cutoff"), json::Value(1200)) == 1200);
    CHECK(*fromJson(P("filter.cutoff"), json::Value("1.5 kHz")) == doctest::Approx(1500));
    CHECK_FALSE(fromJson(P("filter.cutoff"), json::Value("1.5 kHz"), false));
    CHECK(*fromJson(P("filter.mode"), json::Value("bandpass")) == 1);
    CHECK(*fromJson(P("filter.mode"), json::Value(2)) == 2);
    CHECK(*fromJson(P("bypass"), json::Value(false)) == 0);
    auto t = fromJson(P("bypass"), json::Value(3));
    REQUIRE_FALSE(t);
    CHECK(t.error().code == "params.type");
    CHECK(t.error().hint == "e.g. false");
    auto r = fromJson(P("filter.cutoff"), json::Value(5));
    REQUIRE_FALSE(r);
    CHECK(r.error().code == "params.out_of_range");
}

TEST_CASE("lookup and unknown-key hints") {
    CHECK(findById(kT, 4)->key == "voice.count");
    CHECK(findByKey(kT, "nope") == nullptr);
    CHECK(indexOf(kT, P("env.attack")) == 2);
    auto e = unknownKey(kT, "filter.cuttoff", "/params/filter.cuttoff");
    CHECK(e.code == "params.unknown_key");
    CHECK(e.path == "/params/filter.cuttoff");
    CHECK(e.hint.find("did you mean \"filter.cutoff\"?") == 0);
    CHECK(e.hint.find("filter.mode") != std::string::npos);
    auto far = unknownKey(kT, "zzzzzzzz");
    CHECK(far.hint.find("valid keys:") == 0);
}

TEST_CASE("describeTable golden output") {
    const std::string golden = R"({
  "schema": "supple.param-table/1.0",
  "params": {
    "filter.cutoff": {
      "id": 0,
      "name": "Cutoff",
      "group": "Filter",
      "type": "float",
      "unit": "Hz",
      "min": 20.0,
      "max": 20000.0,
      "default": 1000.0,
      "curve": "log",
      "tags": ["role:brightness", "stage:filter"],
      "description": "Low-pass corner frequency.",
      "flags": ["automatable", "modulatable"]
    },
    "filter.mode": {
      "id": 1,
      "name": "Mode",
      "group": "Filter",
      "type": "enum",
      "choices": ["lowpass", "bandpass", "highpass"],
      "default": "lowpass",
      "flags": ["automatable"]
    }
  }
}
)";
    const ParamSpec two[] = {kTable[0], kTable[1]};
    CHECK(json::writeCanonical(describeTable(two)) == golden);
    auto meter = describeParamJson(P("meter.out"));
    CHECK(meter.find("default") == nullptr);
    CHECK(json::writeCompact(*meter.find("flags")) == "[\"readonly\"]");
    CHECK(describeParam(P("filter.cutoff"), 1200) ==
          "Cutoff 1.20 kHz (filter.cutoff, 20.0 Hz..20.0 kHz, log)");
    CHECK(curveName({CurveKind::Pow, 3.0}, Type::Float) == "pow:3");
    CHECK(curveName({CurveKind::Bipolar, 2.5}, Type::Float) == "bipolar:2.5");
}

namespace {
double rtConvert(const ParamSpec& p, double v) SUPPLE_RT {
    return fromClap(p, toClap(p, v)) + fromNormalized(p, toNormalized(p, v)) + clampPhysical(p, v);
}
}  // namespace

TEST_CASE("conversions are real-time safe" * doctest::description("rt")) {
    double sum = 0;
    for (const ParamSpec& p : kT)
        for (int i = 0; i < 100; ++i) sum += rtConvert(p, p.min + (p.max - p.min) * i / 99.0);
    CHECK(std::isfinite(sum));
}

TEST_CASE("review fixes: boundary-local range slack, log range limit, no bare inf") {
    // A wide range must not widen the slack at the other boundary.
    constexpr ParamSpec wide{.id = 0, .key = "wide", .name = "Wide", .min = 0, .max = 1e9};
    CHECK(parse(wide, "-0.5").error().code == "params.out_of_range");
    CHECK(fromJson(wide, json::Value(-0.5)).error().code == "params.out_of_range");
    CHECK(*parse(wide, "0") == 0.0);
    CHECK(*parse(wide, "1000000000.0000001") == 1e9);  // within 1e-9 of the boundary itself

    constexpr ParamSpec extreme{.id = 0,
                                .key = "x",
                                .name = "X",
                                .min = 1e-300,
                                .max = 1e300,
                                .def = 1,
                                .curve = {CurveKind::Log}};
    auto bad = validateTable(Table(&extreme, 1));
    REQUIRE_FALSE(bad);
    CHECK(json::writeCompact(json::Value(bad.error().toString())).find("log_range") !=
          std::string::npos);
    constexpr ParamSpec span{.id = 0, .key = "x", .name = "X", .min = -1.7e308, .max = 1.7e308};
    CHECK_FALSE(validateTable(Table(&span, 1)).ok());

    CHECK_FALSE(parse(P("out.level"), "inf").ok());  // "+inf" is not "off"
    CHECK(*parse(P("out.level"), "off") == -60);
}

TEST_CASE("a description reads back into a spec that converts identically (hosts)") {
    for (const ParamSpec& p : kTable) {
        INFO(std::string(p.key));
        auto d = fromDescription(p.key, describeParamJson(p));
        REQUIRE_MESSAGE(d, (d ? std::string() : d.error().toString()));
        const ParamSpec& q = (*d)->spec;
        CHECK(q.id == p.id);
        CHECK(q.key == p.key);
        CHECK(q.type == p.type);
        CHECK(q.unit == p.unit);
        CHECK(q.choices.size() == p.choices.size());
        CHECK((q.flags & kReadonly) == (p.flags & kReadonly));
        for (double n : {0.0, 0.1, 0.25, 0.5, 0.8, 1.0}) {
            const double v = fromNormalized(p, n);
            CHECK(fromNormalized(q, n) == doctest::Approx(v).epsilon(1e-12));
            CHECK(toNormalized(q, v) == doctest::Approx(toNormalized(p, v)).epsilon(1e-12));
            CHECK(toClap(q, v) == doctest::Approx(toClap(p, v)).epsilon(1e-12));
        }
        if (!(p.flags & kReadonly)) CHECK(q.def == doctest::Approx(p.def));
    }
    CHECK_FALSE(fromDescription("x", json::Value(3.0)));
    CHECK_FALSE(fromDescription("x", json::parse(R"({"name": "X"})").value()));  // no id
    CHECK_FALSE(fromDescription("x", json::parse(R"({"id": 1, "unit": "furlong"})").value()));
    for (const auto* text :
         {R"({"id":-1})", R"({"id":0.5})", R"({"id":4294967296})", R"({"id":1e100})"}) {
        auto bad = fromDescription("x", json::parse(text).value());
        REQUIRE_FALSE(bad);
        CHECK(bad.error().path == "/x/id");
    }
}

TEST_CASE("parameter curve exponents must be finite") {
    auto p = kTable[0];
    p.curve = {CurveKind::Pow, std::numeric_limits<double>::infinity()};
    auto result = validateTable(std::span(&p, 1));
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "params.table.curve_k");
}

TEST_CASE("parameter descriptions reject non-string choices and flags") {
    for (const auto& [text, path] :
         {std::pair{R"({"id":1,"type":"enum","choices":[1,2]})", "/x/choices/0"},
          {R"({"id":1,"flags":[false]})", "/x/flags/0"}}) {
        auto value = json::parse(text);
        REQUIRE(value);
        auto result = fromDescription("x", *value);
        REQUIRE_FALSE(result);
        CHECK(result.error().code == "params.description");
        CHECK(result.error().path == path);
        CHECK_FALSE(result.error().hint.empty());
    }
}

TEST_CASE("R4 curve descriptions retain exact exponents") {
    for (auto kind : {CurveKind::Pow, CurveKind::Bipolar}) {
        for (double k :
             {0.0001, 1.2345678901234567, 3.0, 1e-100, 1e100,
              std::numeric_limits<double>::denorm_min(), std::numeric_limits<double>::max()}) {
            ParamSpec p = P("env.attack");
            p.curve = {kind, k};
            const auto description = describeParamJson(p);
            const auto back = fromDescription(p.key, description);
            REQUIRE(back);
            CHECK((*back)->spec.curve.k == k);
            CHECK(toNormalized((*back)->spec, p.min + (p.max - p.min) * 0.5) ==
                  toNormalized(p, p.min + (p.max - p.min) * 0.5));
        }
    }
    CHECK(describeParamJson(P("env.attack")).find("curve")->asString() == "pow:3");
    CHECK(describeParamJson(P("osc.fine")).find("curve")->asString() == "bipolar:2");
}
TEST_CASE("R4 curve descriptions reject invalid exponents") {
    for (const auto text :
         {"pow:0", "pow:-2", "pow:nan", "bipolar:inf", "pow:1e999", "pow:", "pow:2x"}) {
        auto description = describeParamJson(P("env.attack"));
        description["curve"] = text;
        auto back = fromDescription("env/attack", description);
        REQUIRE_FALSE(back);
        CHECK(back.error().code == "params.description");
        CHECK(back.error().path == "/env~1attack/curve");
        CHECK_FALSE(back.error().hint.empty());
    }
}
