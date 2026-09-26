// Semantic parameter definitions (spec: docs/specs/core-params.md, ADR-0010).
//
// A plugin declares one constexpr table of ParamSpec. Everything else — CLAP
// param_info, value text, patches, the GUI, docs and the bus — derives from
// it through the functions below, so they agree by construction.
//
//   inline constexpr supple::params::Choice kModes[] = {
//       {"lowpass", "Low-pass"}, {"highpass", "High-pass"}};
//   inline constexpr std::string_view kCutoffTags[] = {"role:brightness"};
//   inline constexpr supple::params::ParamSpec kParams[] = {
//       {.id = 0, .key = "filter.cutoff", .name = "Cutoff", .group = "Filter",
//        .type = Type::Float, .unit = Unit::Hz, .min = 20, .max = 20000, .def = 1000,
//        .curve = {CurveKind::Log}, .digits = 3, .tags = kCutoffTags,
//        .description = "Low-pass corner frequency.",
//        .flags = kAutomatable | kModulatable, .smoothingMs = 20},
//       {.id = 1, .key = "filter.mode", .name = "Mode", .group = "Filter",
//        .type = Type::Enum, .max = 1, .choices = kModes, .flags = kAutomatable}};
//
// Value domains: *physical* (bus, patches, envelopes: Hz, ms, dB, enum index,
// 0/1), *normalized* (curve position in [0, 1]) and the *CLAP value* a generic
// host sees (normalized for Float, the value for Int, the index for Enum, 0/1
// for Bool). Conversions are RT-safe; text and JSON functions are not.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "supple/base/result.h"
#include "supple/base/rt.h"
#include "supple/base/units.h"
#include "supple/json/value.h"

namespace supple::params {

using units::Unit;

enum class Type : std::uint8_t { Float, Int, Bool, Enum };

enum class CurveKind : std::uint8_t {
    Linear,   // n = (v - min) / (max - min)
    Log,      // n = ln(v / min) / ln(max / min); 0 < min < max
    Pow,      // v = min + (max - min) * n^k; k > 0
    Bipolar,  // t = 2n - 1; v = mid + half * sign(t) * |t|^k
};

struct Curve {
    CurveKind kind = CurveKind::Linear;
    double k = 1.0;
};

struct Choice {
    std::string_view key;    // stable machine value: "lowpass"
    std::string_view label;  // display: "Low-pass"
};

enum Flag : std::uint32_t {
    kAutomatable = 1u << 0,
    kModulatable = 1u << 1,
    kReadonly = 1u << 2,
    kHidden = 1u << 3,
    kOffAtMin = 1u << 4,
    kBypass = 1u << 5,
    kPeriodic = 1u << 6,
};

struct ParamSpec {
    std::uint32_t id = 0;    // CLAP param id: session ABI, append-only
    std::string_view key;    // "filter.cutoff"
    std::string_view name;   // "Cutoff"
    std::string_view group;  // "Filter" ('/' separates levels)
    Type type = Type::Float;
    Unit unit = Unit::none;                  // canonical unit of its dimension; none for Bool/Enum
    double min = 0.0, max = 1.0, def = 0.0;  // physical (Enum: index range, Bool: 0..1)
    Curve curve{};                           // Float only
    int digits = 3;                          // significant digits for display
    std::span<const Choice> choices{};
    std::span<const std::string_view> tags{};
    std::string_view description;
    std::uint32_t flags = kAutomatable;
    double smoothingMs = 0.0;
};

using Table = std::span<const ParamSpec>;

// --- validation --------------------------------------------------------------

// Checks every table rule in the spec (unique ids/keys, ranges, curves,
// choices, units, tags, flags). Plugins run it in a unit test.
Status validateTable(Table table);

// The controlled role vocabulary for tags ("role:<name>").
std::span<const std::string_view> roleVocabulary();

// --- conversions (RT-safe) -------------------------------------------------------

double clampPhysical(const ParamSpec& p, double v) SUPPLE_RT;  // NaN -> def; rounds Int/Enum/Bool
double toNormalized(const ParamSpec& p, double physical) SUPPLE_RT;
double fromNormalized(const ParamSpec& p, double normalized) SUPPLE_RT;
double toClap(const ParamSpec& p, double physical) SUPPLE_RT;
double fromClap(const ParamSpec& p, double clapValue) SUPPLE_RT;

struct ClapRange {
    double min, max, def;
    bool stepped;
    bool isEnum;
};
ClapRange clapRange(const ParamSpec& p);
std::uint32_t clapFlags(const ParamSpec& p);  // CLAP_PARAM_IS_* bits

// --- text ------------------------------------------------------------------------

std::string format(const ParamSpec& p, double physical);  // "1.20 kHz", "-inf dB", "Low-pass"
std::string formatCompact(const ParamSpec& p, double physical);  // knob labels, <= 6 chars
Result<double> parse(const ParamSpec& p,
                     std::string_view text);  // whole string; no silent clamping

// --- JSON ------------------------------------------------------------------------

json::Value toJson(const ParamSpec& p, double physical);  // number / enum key / bool
Result<double> fromJson(const ParamSpec& p, const json::Value& v, bool acceptUnitStrings = true);

// --- lookup and description ------------------------------------------------------

const ParamSpec* findById(Table table, std::uint32_t id);
const ParamSpec* findByKey(Table table, std::string_view key);
std::size_t indexOf(Table table, const ParamSpec& p);  // position in the table
// params.unknown_key with a closest-match hint and the keys of the likely group.
Error unknownKey(Table table, std::string_view key, std::string_view jsonPath = {});

json::Value describeTable(Table table);  // {"schema": "supple.param-table/1.0", "params": {...}}
json::Value describeParamJson(const ParamSpec& p);  // one entry of describeTable's "params"
std::string describeParam(
    const ParamSpec& p,
    double physical);  // "Cutoff 1.20 kHz (filter.cutoff, 20.0 Hz..20.0 kHz, log)"
std::string curveName(const Curve& c, Type type);  // "linear", "log", "pow:3", "bipolar:2"

}  // namespace supple::params
