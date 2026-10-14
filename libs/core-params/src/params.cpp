// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/params/params.h"

#include <clap/ext/params.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <set>
#include <string>
#include <vector>

#include "supple/base/closest.h"
#include "supple/json/pointer.h"
#include "supple/json/write.h"

namespace supple::params {

namespace {

constexpr std::string_view kRoles[] = {
    "level",  "gain",    "mix",       "brightness", "resonance", "time",      "rate",
    "depth",  "pitch",   "width",     "pan",        "drive",     "threshold", "ratio",
    "attack", "release", "frequency", "q",          "mode",      "enable",    "seed"};

bool validKey(std::string_view key) {
    // [a-z][a-z0-9_]*(\.[a-z][a-z0-9_]*)*
    bool segmentStart = true;
    if (key.empty()) return false;
    for (char c : key) {
        if (segmentStart) {
            if (c < 'a' || c > 'z') return false;
            segmentStart = false;
        } else if (c == '.') {
            segmentStart = true;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return !segmentStart;
}

bool validChoiceKey(std::string_view key) {
    if (key.empty() || key[0] < 'a' || key[0] > 'z') return false;
    return std::all_of(key.begin(), key.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// The canonical unit of each dimension (core-base rule): every Unit value is
// canonical except none, which is only valid for Bool/Enum/dimensionless.
bool isInteger(double v) { return std::isfinite(v) && std::floor(v) == v; }

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

std::string fixed(double v, int decimals) {
    char buf[64];
    auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), v, std::chars_format::fixed, decimals);
    if (ec != std::errc{}) return "?";
    std::string s(buf, end);
    if (s == "-0" || s.rfind("-0.", 0) == 0) {
        // Avoid "-0.00" for values that round to zero.
        bool allZero =
            std::all_of(s.begin() + 1, s.end(), [](char c) { return c == '0' || c == '.'; });
        if (allZero) s.erase(0, 1);
    }
    return s;
}

int decimalsFor(double v, int digits) {
    if (digits <= 0) digits = 3;
    if (v == 0.0 || !std::isfinite(v)) return std::min(1, digits - 1);
    const int magnitude = static_cast<int>(std::floor(std::log10(std::fabs(v))));
    return std::clamp(digits - 1 - magnitude, 0, 6);
}

struct Display {
    double value;
    std::string_view unit;  // label to print after the number ("" for none)
};

Display scaleForDisplay(const ParamSpec& p, double v) {
    switch (p.unit) {
        case Unit::Hz:
            if (std::fabs(v) >= 1000.0) return {v / 1000.0, "kHz"};
            return {v, "Hz"};
        case Unit::ms:
            if (std::fabs(v) >= 1000.0) return {v / 1000.0, "s"};
            return {v, "ms"};
        case Unit::none:
            return {v, ""};
        default:
            return {v, units::unitName(p.unit)};
    }
}

std::string formatNumber(const ParamSpec& p, double v, bool withUnit) {
    const Display d = scaleForDisplay(p, v);
    const int decimals = p.type == Type::Int ? 0 : decimalsFor(d.value, p.digits);
    std::string s = fixed(d.value, decimals);
    if (p.min < 0.0 && s[0] != '-' && d.value != 0.0 &&
        s.find_first_not_of("0.") != std::string::npos)
        s.insert(0, "+");
    if (!withUnit) return s;
    if (p.unit == Unit::ratio) return s + ":1";
    if (p.unit == Unit::percent) return s + " %";
    if (!d.unit.empty()) return s + " " + std::string(d.unit);
    return s;
}

std::size_t choiceCount(const ParamSpec& p) { return p.choices.size(); }

Error rangeError(const ParamSpec& p, double v, std::string_view path) {
    Error e("params.out_of_range",
            std::string(p.key) + ": " + formatNumber(p, v, true) + " is out of range",
            std::string(path), "valid range: " + format(p, p.min) + " .. " + format(p, p.max));
    return e;
}

std::string exampleText(const ParamSpec& p) { return format(p, p.def); }

}  // namespace

std::span<const std::string_view> roleVocabulary() { return kRoles; }

// --- validation ------------------------------------------------------------------

namespace {
// In range, allowing a relative 1e-9 slack local to each boundary (so a
// boundary value that went through text or JSON still counts), never a slack
// scaled by the other, possibly much larger, endpoint.
bool withinRange(const ParamSpec& p, double v) {
    const double lo = 1e-9 * std::max(1.0, std::fabs(p.min));
    const double hi = 1e-9 * std::max(1.0, std::fabs(p.max));
    return v >= p.min - lo && v <= p.max + hi;
}
}  // namespace

Status validateTable(Table table) {
    std::vector<Error> errors;
    std::set<std::uint32_t> ids;
    std::set<std::string_view> keys;
    int bypassCount = 0;
    for (std::size_t i = 0; i < table.size(); ++i) {
        const ParamSpec& p = table[i];
        const std::string where = "/params/" + std::to_string(i);
        auto fail = [&](std::string code, std::string message, std::string hint = {}) {
            errors.emplace_back("params.table." + code, std::string(p.key) + ": " + message, where,
                                std::move(hint));
        };
        if (!ids.insert(p.id).second)
            fail("duplicate_id", "id " + std::to_string(p.id) + " is used twice");
        if (!validKey(p.key))
            fail("bad_key", "key must be dotted lowercase segments", "e.g. \"filter.cutoff\"");
        else if (!keys.insert(p.key).second)
            fail("duplicate_key", "key is used twice");
        if (p.name.empty()) fail("no_name", "name is empty");
        if (!std::isfinite(p.min) || !std::isfinite(p.max) || !std::isfinite(p.def))
            fail("not_finite", "min, max and default must be finite");
        switch (p.type) {
            case Type::Float:
                if (!(p.min < p.max)) fail("range", "min must be below max");
                if (!std::isfinite(p.max - p.min))
                    fail("range_span", "max - min must be finite", "use a smaller range");
                if (p.curve.kind == CurveKind::Log && !(p.min > 0.0))
                    fail("log_min", "a log curve needs min > 0");
                else if (p.curve.kind == CurveKind::Log && !(p.max / p.min <= 1e15))
                    fail("log_range", "a log curve spans at most 15 decades (max/min <= 1e15)");
                if ((p.curve.kind == CurveKind::Pow || p.curve.kind == CurveKind::Bipolar) &&
                    !(std::isfinite(p.curve.k) && p.curve.k > 0.0))
                    fail("curve_k", "pow/bipolar curves need finite k > 0");
                break;
            case Type::Int:
                if (!(p.min < p.max)) fail("range", "min must be below max");
                if (!isInteger(p.min) || !isInteger(p.max) || !isInteger(p.def))
                    fail("int_range", "int parameters need integral min, max and default");
                if (p.curve.kind != CurveKind::Linear)
                    fail("int_curve", "int parameters are linear");
                break;
            case Type::Bool:
                if (p.min != 0.0 || p.max != 1.0 || (p.def != 0.0 && p.def != 1.0))
                    fail("bool_range", "bool parameters use min 0, max 1, default 0 or 1");
                if (p.unit != Unit::none) fail("unit", "bool parameters have no unit");
                break;
            case Type::Enum: {
                if (choiceCount(p) < 2) fail("choices", "enum parameters need at least 2 choices");
                std::set<std::string_view> choiceKeys;
                for (const Choice& c : p.choices) {
                    if (!validChoiceKey(c.key))
                        fail("choice_key", "choice key \"" + std::string(c.key) +
                                               "\" is not lowercase snake case");
                    else if (!choiceKeys.insert(c.key).second)
                        fail("choice_dup",
                             "choice key \"" + std::string(c.key) + "\" is used twice");
                }
                if (p.min != 0.0 || p.max != static_cast<double>(choiceCount(p)) - 1.0)
                    fail("enum_range", "enum range must be 0 .. choices - 1",
                         "set .max = " +
                             std::to_string(choiceCount(p) > 0 ? choiceCount(p) - 1 : 0));
                if (!isInteger(p.def)) fail("enum_default", "enum default must be a choice index");
                if (p.unit != Unit::none) fail("unit", "enum parameters have no unit");
                break;
            }
        }
        if (p.def < p.min || p.def > p.max) fail("default", "default is outside [min, max]");
        for (std::string_view tag : p.tags) {
            const bool role =
                tag.rfind("role:", 0) == 0 &&
                std::find(std::begin(kRoles), std::end(kRoles), tag.substr(5)) != std::end(kRoles);
            const bool stage = tag.rfind("stage:", 0) == 0 && tag.size() > 6;
            if (!role && !stage && tag != "macro")
                fail("tag", "tag \"" + std::string(tag) + "\" is not in the vocabulary",
                     "use role:<name>, stage:<name> or macro");
        }
        if ((p.flags & kReadonly) && (p.flags & kModulatable))
            fail("readonly_mod", "readonly params cannot be modulatable");
        if (p.flags & kBypass) {
            ++bypassCount;
            if (p.type != Type::Bool) fail("bypass_type", "the bypass param must be a bool");
        }
        if (p.digits <= 0 || p.digits > 9) fail("digits", "digits must be 1..9");
    }
    if (bypassCount > 1)
        errors.emplace_back("params.table.bypass_count", "more than one bypass parameter",
                            "/params");
    if (errors.empty()) return okStatus();
    if (errors.size() == 1) return std::move(errors.front());
    Error e("params.table.invalid",
            std::to_string(errors.size()) + " problems in the parameter table", "/params");
    e.details = std::move(errors);
    return e;
}

// --- conversions -----------------------------------------------------------------

double clampPhysical(const ParamSpec& p, double v) SUPPLE_RT {
    if (std::isnan(v)) v = p.def;
    v = std::clamp(v, p.min, p.max);
    if (p.type != Type::Float) v = std::round(v);
    return v;
}

double toNormalized(const ParamSpec& p, double physical) SUPPLE_RT {
    const double v = clampPhysical(p, physical);
    if (v <= p.min) return 0.0;
    if (v >= p.max) return 1.0;
    double n = 0.0;
    if (p.type != Type::Float) {
        n = (v - p.min) / (p.max - p.min);
    } else {
        switch (p.curve.kind) {
            case CurveKind::Linear:
                n = (v - p.min) / (p.max - p.min);
                break;
            case CurveKind::Log:
                n = std::log(v / p.min) / std::log(p.max / p.min);
                break;
            case CurveKind::Pow:
                n = std::pow((v - p.min) / (p.max - p.min), 1.0 / p.curve.k);
                break;
            case CurveKind::Bipolar: {
                const double mid = 0.5 * (p.min + p.max);
                const double half = 0.5 * (p.max - p.min);
                const double u = (v - mid) / half;
                const double t = std::copysign(std::pow(std::fabs(u), 1.0 / p.curve.k), u);
                n = 0.5 * (t + 1.0);
                break;
            }
        }
    }
    return std::clamp(n, 0.0, 1.0);
}

double fromNormalized(const ParamSpec& p, double normalized) SUPPLE_RT {
    if (std::isnan(normalized)) return p.def;
    const double n = std::clamp(normalized, 0.0, 1.0);
    if (n <= 0.0) return p.min;
    if (n >= 1.0) return p.max;
    if (p.type != Type::Float) return clampPhysical(p, p.min + n * (p.max - p.min));
    double v = p.min;
    switch (p.curve.kind) {
        case CurveKind::Linear:
            v = p.min + n * (p.max - p.min);
            break;
        case CurveKind::Log:
            v = p.min * std::exp(n * std::log(p.max / p.min));
            break;
        case CurveKind::Pow:
            v = p.min + (p.max - p.min) * std::pow(n, p.curve.k);
            break;
        case CurveKind::Bipolar: {
            const double mid = 0.5 * (p.min + p.max);
            const double half = 0.5 * (p.max - p.min);
            const double t = 2.0 * n - 1.0;
            v = mid + half * std::copysign(std::pow(std::fabs(t), p.curve.k), t);
            break;
        }
    }
    return std::clamp(v, p.min, p.max);
}

double toClap(const ParamSpec& p, double physical) SUPPLE_RT {
    if (p.type == Type::Float) return toNormalized(p, physical);
    return clampPhysical(p, physical);
}

double fromClap(const ParamSpec& p, double clapValue) SUPPLE_RT {
    if (p.type == Type::Float) return fromNormalized(p, clapValue);
    return clampPhysical(p, clapValue);
}

ClapRange clapRange(const ParamSpec& p) {
    if (p.type == Type::Float) return {0.0, 1.0, toNormalized(p, p.def), false, false};
    return {p.min, p.max, clampPhysical(p, p.def), true, p.type == Type::Enum};
}

std::uint32_t clapFlags(const ParamSpec& p) {
    std::uint32_t f = 0;
    if (p.type != Type::Float) f |= CLAP_PARAM_IS_STEPPED;
    if (p.type == Type::Enum) f |= CLAP_PARAM_IS_ENUM;
    if (p.flags & kReadonly) {
        f |= CLAP_PARAM_IS_READONLY;
    } else {
        if (p.flags & kAutomatable) f |= CLAP_PARAM_IS_AUTOMATABLE;
        if (p.flags & kModulatable) f |= CLAP_PARAM_IS_MODULATABLE;
    }
    if (p.flags & kHidden) f |= CLAP_PARAM_IS_HIDDEN;
    if (p.flags & kBypass) f |= CLAP_PARAM_IS_BYPASS;
    if (p.flags & kPeriodic) f |= CLAP_PARAM_IS_PERIODIC;
    return f;
}

// --- text ------------------------------------------------------------------------

std::string format(const ParamSpec& p, double physical) {
    const double v = clampPhysical(p, physical);
    switch (p.type) {
        case Type::Enum: {
            const auto i = static_cast<std::size_t>(v);
            return i < p.choices.size() ? std::string(p.choices[i].label) : std::to_string(i);
        }
        case Type::Bool:
            return v >= 0.5 ? "on" : "off";
        default:
            break;
    }
    if ((p.flags & kOffAtMin) && v <= p.min) return p.unit == Unit::dB ? "-inf dB" : "off";
    return formatNumber(p, v, true);
}

std::string formatCompact(const ParamSpec& p, double physical) {
    const double v = clampPhysical(p, physical);
    switch (p.type) {
        case Type::Enum: {
            const auto i = static_cast<std::size_t>(v);
            std::string label =
                i < p.choices.size() ? std::string(p.choices[i].label) : std::to_string(i);
            return label.substr(0, 6);
        }
        case Type::Bool:
            return v >= 0.5 ? "on" : "off";
        default:
            break;
    }
    if ((p.flags & kOffAtMin) && v <= p.min) return p.unit == Unit::dB ? "-inf" : "off";
    const Display d = scaleForDisplay(p, v);
    std::string s = formatNumber(p, v, false);
    if (d.unit == "kHz") s += "k";
    if (d.unit == "s") s += "s";
    if (s.size() > 6) s = fixed(d.value, 0) + (d.unit == "kHz" ? "k" : d.unit == "s" ? "s" : "");
    return s.substr(0, 6);
}

Result<double> parse(const ParamSpec& p, std::string_view text) {
    const std::string_view t = trim(text);
    const std::string low = lower(t);
    if (t.empty())
        return Error("params.parse", std::string(p.key) + ": empty value", {},
                     "e.g. \"" + exampleText(p) + "\"");
    if (p.type == Type::Enum) {
        for (std::size_t i = 0; i < p.choices.size(); ++i)
            if (low == lower(p.choices[i].key) || low == lower(p.choices[i].label))
                return static_cast<double>(i);
        std::size_t index = 0;
        auto [end, ec] = std::from_chars(t.data(), t.data() + t.size(), index);
        if (ec == std::errc{} && end == t.data() + t.size() && index < p.choices.size())
            return static_cast<double>(index);
        std::vector<std::string_view> names;
        for (const Choice& c : p.choices) names.push_back(c.key);
        std::string hint;
        const std::string closest = closestMatch(t, names);
        if (!closest.empty()) hint = "did you mean \"" + closest + "\"? ";
        hint += "choices:";
        for (std::string_view n : names) hint += " " + std::string(n);
        return Error("params.enum",
                     std::string(p.key) + ": \"" + std::string(t) + "\" is not a choice", {},
                     std::move(hint));
    }
    if (p.type == Type::Bool) {
        if (low == "on" || low == "true" || low == "yes" || low == "1") return 1.0;
        if (low == "off" || low == "false" || low == "no" || low == "0") return 0.0;
        return Error("params.parse",
                     std::string(p.key) + ": \"" + std::string(t) + "\" is not on/off", {},
                     "use on, off, true or false");
    }
    if (p.flags & kOffAtMin) {
        if (low == "off" || low == "-inf" || low == "-inf db" || low == "-∞" || low == "-∞ db")
            return p.min;
    }
    auto q = units::parseQuantity(t, p.unit);
    if (!q)
        return Error("params.parse",
                     std::string(p.key) + ": cannot read \"" + std::string(t) + "\" as " +
                         std::string(units::dimensionName(p.unit)),
                     {}, "e.g. \"" + exampleText(p) + "\"");
    const double v = *q;
    if (!withinRange(p, v)) return rangeError(p, v, {});
    return clampPhysical(p, v);
}

// --- JSON ------------------------------------------------------------------------

json::Value toJson(const ParamSpec& p, double physical) {
    const double v = clampPhysical(p, physical);
    switch (p.type) {
        case Type::Float:
            return json::Value(v);
        case Type::Int:
            return json::Value(static_cast<long long>(v));
        case Type::Bool:
            return json::Value(v >= 0.5);
        case Type::Enum: {
            const auto i = static_cast<std::size_t>(v);
            return i < p.choices.size() ? json::Value(std::string(p.choices[i].key))
                                        : json::Value(static_cast<long long>(i));
        }
    }
    return {};
}

Result<double> fromJson(const ParamSpec& p, const json::Value& v, bool acceptUnitStrings) {
    auto typeError = [&]() {
        return Error("params.type", std::string(p.key) + ": wrong JSON type", {},
                     "e.g. " + json::writeCompact(toJson(p, p.def)));
    };
    switch (p.type) {
        case Type::Bool:
            if (v.isBool()) return v.asBool() ? 1.0 : 0.0;
            if (v.isString() && acceptUnitStrings) return parse(p, v.asString());
            return typeError();
        case Type::Enum:
            if (v.isString()) return parse(p, v.asString());
            if (v.isInt()) {
                const auto i = v.asInt();
                if (i >= 0 && static_cast<std::size_t>(i) < p.choices.size())
                    return static_cast<double>(i);
                return parse(p, std::to_string(i));
            }
            return typeError();
        case Type::Float:
        case Type::Int: {
            if (v.isNumber()) {
                const double x = v.asDouble();
                if (!std::isfinite(x)) return typeError();
                if (!withinRange(p, x)) return rangeError(p, x, {});
                return clampPhysical(p, x);
            }
            if (v.isString() && acceptUnitStrings) return parse(p, v.asString());
            return typeError();
        }
    }
    return typeError();
}

// --- lookup and description ------------------------------------------------------

const ParamSpec* findById(Table table, std::uint32_t id) {
    for (const ParamSpec& p : table)
        if (p.id == id) return &p;
    return nullptr;
}

const ParamSpec* findByKey(Table table, std::string_view key) {
    for (const ParamSpec& p : table)
        if (p.key == key) return &p;
    return nullptr;
}

std::size_t indexOf(Table table, const ParamSpec& p) {
    return static_cast<std::size_t>(&p - table.data());
}

Error unknownKey(Table table, std::string_view key, std::string_view jsonPath) {
    std::vector<std::string_view> keys;
    for (const ParamSpec& p : table) keys.push_back(p.key);
    const std::string closest = closestMatch(key, keys);
    std::string hint;
    if (!closest.empty()) {
        hint = "did you mean \"" + closest + "\"?";
        const ParamSpec* match = findByKey(table, closest);
        if (match && !match->group.empty()) {
            hint += " Parameters in " + std::string(match->group) + ":";
            for (const ParamSpec& p : table)
                if (p.group == match->group && !(p.flags & kReadonly))
                    hint += " " + std::string(p.key);
        }
    } else {
        hint = "valid keys:";
        std::size_t shown = 0;
        for (std::string_view k : keys) {
            if (shown++ == 12) {
                hint += " …";
                break;
            }
            hint += " " + std::string(k);
        }
    }
    return Error("params.unknown_key", "unknown parameter \"" + std::string(key) + "\"",
                 std::string(jsonPath), std::move(hint));
}

std::string curveName(const Curve& c, Type type) {
    if (type != Type::Float) return "linear";
    auto k = [&] {
        std::string s = fixed(c.k, 3);
        while (s.size() > 1 && s.back() == '0') s.pop_back();
        if (!s.empty() && s.back() == '.') s.pop_back();
        double parsed = 0;
        const auto old = std::from_chars(s.data(), s.data() + s.size(), parsed);
        if (old.ec == std::errc{} && old.ptr == s.data() + s.size() && parsed == c.k) return s;
        // General format without a precision emits the shortest round-trip text.
        std::array<char, 64> text{};
        const auto result = std::to_chars(text.data(), text.data() + text.size(), c.k);
        return std::string(text.data(), result.ptr);
    };
    switch (c.kind) {
        case CurveKind::Linear:
            return "linear";
        case CurveKind::Log:
            return "log";
        case CurveKind::Pow:
            return "pow:" + k();
        case CurveKind::Bipolar:
            return "bipolar:" + k();
    }
    return "linear";
}

json::Value describeParamJson(const ParamSpec& p) {
    json::Value e = json::Value::object();
    e["id"] = static_cast<long long>(p.id);
    e["name"] = std::string(p.name);
    if (!p.group.empty()) e["group"] = std::string(p.group);
    static constexpr std::string_view kTypes[] = {"float", "int", "bool", "enum"};
    e["type"] = std::string(kTypes[static_cast<int>(p.type)]);
    if (p.type == Type::Float || p.type == Type::Int) {
        if (p.unit != Unit::none) e["unit"] = std::string(units::unitName(p.unit));
        e["min"] =
            p.type == Type::Int ? json::Value(static_cast<long long>(p.min)) : json::Value(p.min);
        e["max"] =
            p.type == Type::Int ? json::Value(static_cast<long long>(p.max)) : json::Value(p.max);
    }
    if (p.type == Type::Enum) {
        json::Value choices = json::Value::array();
        for (const Choice& c : p.choices) choices.push_back(std::string(c.key));
        e["choices"] = std::move(choices);
    }
    if (!(p.flags & kReadonly)) e["default"] = toJson(p, p.def);
    if (p.type == Type::Float) e["curve"] = curveName(p.curve, p.type);
    if (!p.tags.empty()) {
        json::Value tags = json::Value::array();
        for (std::string_view t : p.tags) tags.push_back(std::string(t));
        e["tags"] = std::move(tags);
    }
    if (!p.description.empty()) e["description"] = std::string(p.description);
    json::Value flags = json::Value::array();
    if (p.flags & kReadonly) flags.push_back("readonly");
    if (!(p.flags & kReadonly) && (p.flags & kAutomatable)) flags.push_back("automatable");
    if (!(p.flags & kReadonly) && (p.flags & kModulatable)) flags.push_back("modulatable");
    if (p.flags & kHidden) flags.push_back("hidden");
    if (p.flags & kOffAtMin) flags.push_back("off_at_min");
    if (p.flags & kBypass) flags.push_back("bypass");
    if (p.flags & kPeriodic) flags.push_back("periodic");
    e["flags"] = std::move(flags);
    return e;
}

json::Value describeTable(Table table) {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.param-table/1.0";
    json::Value params = json::Value::object();
    for (const ParamSpec& p : table) params[p.key] = describeParamJson(p);
    doc["params"] = std::move(params);
    return doc;
}

std::string describeParam(const ParamSpec& p, double physical) {
    std::string s = std::string(p.name) + " " + format(p, physical) + " (" + std::string(p.key);
    if (p.type == Type::Float || p.type == Type::Int)
        s += ", " + format(p, p.min) + ".." + format(p, p.max);
    if (p.type == Type::Float) s += ", " + curveName(p.curve, p.type);
    if (p.type == Type::Enum) {
        s += ", one of";
        for (const Choice& c : p.choices) s += " " + std::string(c.key);
    }
    return s + ")";
}

// --- read back a description (hosts) ----------------------------------------------

Result<std::unique_ptr<DescribedParam>> fromDescription(std::string_view key,
                                                        const json::Value& e) {
    if (!e.isObject()) return Error("params.description", "a parameter description is an object");
    auto out = std::make_unique<DescribedParam>();
    out->key = std::string(key);
    ParamSpec& p = out->spec;
    auto num = [&](const char* name, double fallback) {
        const json::Value* v = e.find(name);
        return v && v->isNumber() ? v->asDouble() : fallback;
    };
    auto str = [&](const char* name) {
        const json::Value* v = e.find(name);
        return v && v->isString() ? v->asString() : std::string();
    };
    const json::Value* id = e.find("id");
    if (!id || !id->isNumber()) return Error("params.description", "no id", "/" + out->key);
    if (!isInteger(id->asDouble()) || id->asDouble() < 0 ||
        id->asDouble() > std::numeric_limits<std::uint32_t>::max())
        return Error("params.description", "id must be an integer 0..4294967295",
                     json::pointerJoin("", out->key) + "/id");
    p.id = static_cast<std::uint32_t>(id->asDouble());
    out->name = str("name");
    out->group = str("group");
    const std::string type = str("type");
    p.type = type == "int"    ? Type::Int
             : type == "bool" ? Type::Bool
             : type == "enum" ? Type::Enum
                              : Type::Float;
    if (const std::string unit = str("unit"); !unit.empty()) {
        bool known = false;
        for (int u = 0; u <= static_cast<int>(Unit::none); ++u)
            if (units::unitName(static_cast<Unit>(u)) == unit) {
                p.unit = static_cast<Unit>(u);
                known = true;
            }
        if (!known) return Error("params.description", "unknown unit " + unit, "/" + out->key);
    }
    if (const json::Value* c = e.find("choices"); c && c->isArray()) {
        for (std::size_t i = 0; i < c->size(); ++i) {
            if (!c->at(i).isString())
                return Error("params.description", "choice must be a string",
                             json::pointerJoin(json::pointerJoin("", out->key), "choices") + "/" +
                                 std::to_string(i),
                             "use a string choice key");
            out->choiceKeys.push_back(c->at(i).asString());
        }
        for (const std::string& k : out->choiceKeys) out->choices.push_back({k, k});
        p.choices = out->choices;
        p.min = 0;
        p.max = static_cast<double>(out->choices.size()) - 1;
    } else if (p.type == Type::Bool) {
        p.min = 0;
        p.max = 1;
    } else {
        p.min = num("min", 0.0);
        p.max = num("max", 1.0);
    }
    if (const std::string curve = str("curve"); !curve.empty()) {
        // "linear", "log", "pow:<k>", "bipolar:<k>"
        const auto colon = curve.find(':');
        const std::string kind = curve.substr(0, colon);
        double k = 1.0;
        if (colon != std::string::npos) {
            const char* begin = curve.data() + colon + 1;
            const char* end = curve.data() + curve.size();
            const auto parsed = std::from_chars(begin, end, k);
            if (parsed.ec != std::errc{} || parsed.ptr != end || !std::isfinite(k) || k <= 0)
                return Error("params.description", "curve exponent must be positive and finite",
                             json::pointerJoin(json::pointerJoin("", out->key), "curve"),
                             "use pow:<k> or bipolar:<k> with a positive finite exponent");
        }
        p.curve.kind = kind == "log"       ? CurveKind::Log
                       : kind == "pow"     ? CurveKind::Pow
                       : kind == "bipolar" ? CurveKind::Bipolar
                                           : CurveKind::Linear;
        p.curve.k = k;
    }
    p.key = out->key;
    p.name = out->name;
    p.group = out->group;
    p.flags = 0;
    if (const json::Value* f = e.find("flags"); f && f->isArray())
        for (std::size_t i = 0; i < f->size(); ++i) {
            if (!f->at(i).isString())
                return Error("params.description", "flag must be a string",
                             json::pointerJoin(json::pointerJoin("", out->key), "flags") + "/" +
                                 std::to_string(i),
                             "use a string flag name");
            const std::string flag = f->at(i).asString();
            p.flags |= flag == "automatable"   ? kAutomatable
                       : flag == "modulatable" ? kModulatable
                       : flag == "readonly"    ? kReadonly
                       : flag == "hidden"      ? kHidden
                       : flag == "off_at_min"  ? kOffAtMin
                       : flag == "bypass"      ? kBypass
                       : flag == "periodic"    ? kPeriodic
                                               : 0u;
        }
    if (const json::Value* d = e.find("default")) {
        auto v = fromJson(p, *d, false);
        p.def = v ? *v : p.min;
    } else {
        p.def = p.min;
    }
    return out;
}

}  // namespace supple::params
