// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <supple/base/units.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <sstream>
#include "supple/base/chars.h"
namespace supple::units {
namespace {
struct Alias {
    std::string_view name;
    Unit unit;
    double scale;
};
constexpr Alias aliases[] = {
    {"hz", Unit::Hz, 1},        {"khz", Unit::Hz, 1000}, {"k", Unit::Hz, 1000},
    {"ms", Unit::ms, 1},        {"s", Unit::ms, 1000},   {"sec", Unit::ms, 1000},
    {"us", Unit::ms, .001},     {"µs", Unit::ms, .001},  {"db", Unit::dB, 1},
    {"dbfs", Unit::dB, 1},      {"lufs", Unit::LUFS, 1}, {"lkfs", Unit::LUFS, 1},
    {"lu", Unit::LU, 1},        {"st", Unit::st, 1},     {"semi", Unit::st, 1},
    {"semitones", Unit::st, 1}, {"ct", Unit::ct, 1},     {"cent", Unit::ct, 1},
    {"cents", Unit::ct, 1},     {"oct", Unit::oct, 1},   {"octave", Unit::oct, 1},
    {"octaves", Unit::oct, 1},  {"%", Unit::percent, 1}, {":1", Unit::ratio, 1},
    {"ratio", Unit::ratio, 1},  {"x", Unit::x, 1},       {"×", Unit::x, 1},
    {"deg", Unit::deg, 1},      {"°", Unit::deg, 1},     {"bpm", Unit::bpm, 1},
    {"beats", Unit::beats, 1},  {"qn", Unit::beats, 1},  {"samples", Unit::samples, 1},
    {"smp", Unit::samples, 1}};
std::string lower(std::string_view s) {
    std::string o(s);
    for (char& c : o)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return o;
}
}  // namespace
std::string_view unitName(Unit u) {
    constexpr std::string_view names[] = {"Hz",  "ms",    "dB",      "LUFS",  "LU", "st",
                                          "ct",  "oct",   "%",       "ratio", "x",  "deg",
                                          "bpm", "beats", "samples", "none"};
    return names[static_cast<std::size_t>(u)];
}
std::string_view dimensionName(Unit u) {
    constexpr std::string_view names[] = {
        "frequency",      "time",       "level",           "loudness",     "loudness difference",
        "pitch interval", "fine pitch", "octave interval", "proportion",   "compression ratio",
        "multiplier",     "angle",      "tempo",           "musical time", "sample count",
        "dimensionless"};
    return names[static_cast<std::size_t>(u)];
}
Result<double> parseQuantity(std::string_view text, Unit fieldUnit) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    if (text.empty())
        return Error("schema.unit", "empty quantity", {},
                     "use a number and " + std::string(unitName(fieldUnit)));
    if (text.front() == '+') text.remove_prefix(1);
    double value = 0;
    auto [p, ec] = ::supple::fromChars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc{} || p == text.data() || !std::isfinite(value))
        return Error("schema.unit", "invalid quantity number", {},
                     "use a finite number and " + std::string(unitName(fieldUnit)));
    std::string_view suffix(p, static_cast<std::size_t>(text.data() + text.size() - p));
    while (!suffix.empty() && std::isspace(static_cast<unsigned char>(suffix.front())))
        suffix.remove_prefix(1);
    if (suffix.empty()) return value;
    auto norm = lower(suffix);
    for (const auto& a : aliases)
        if (a.name == norm) {
            if (a.unit != fieldUnit)
                return Error("schema.unit", "incompatible unit", {},
                             "expected " + std::string(dimensionName(fieldUnit)) + ", e.g. 1 " +
                                 std::string(unitName(fieldUnit)));
            if (norm == "dbfs")
                return Error("schema.unit", "dBFS requires an absolute level field", {}, "use dB");
            const double converted = value * a.scale;
            if (!std::isfinite(converted))
                return Error("schema.unit", "quantity overflows its base unit", {},
                             "use a smaller finite quantity");
            return converted;
        }
    return Error("schema.unit", "unknown unit", {},
                 "use a " + std::string(dimensionName(fieldUnit)) + " unit, e.g. 1 " +
                     std::string(unitName(fieldUnit)));
}
std::string formatQuantity(double v, Unit u, int digits) {
    std::string_view label = unitName(u);
    if (u == Unit::Hz && std::fabs(v) >= 1000) {
        v /= 1000;
        label = "kHz";
    }
    int decimals = 0;
    if (std::isfinite(v) && v != 0)
        decimals =
            std::clamp(digits - 1 - static_cast<int>(std::floor(std::log10(std::fabs(v)))), 0, 9);
    std::ostringstream os;
    if (u == Unit::st && v >= 0) os << '+';
    os << std::fixed << std::setprecision(decimals) << v;
    if (u == Unit::ratio)
        os << ":1";
    else if (u != Unit::none)
        os << ' ' << label;
    return os.str();
}
}  // namespace supple::units
