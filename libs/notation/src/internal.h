#pragma once

#include <supple/notation/notation.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace supple::notation::internal {

inline constexpr char kStaccato = '\'';
inline constexpr char kAccent = '>';

inline const std::map<std::string, int>& dynamics() {
    static const std::map<std::string, int> values = {{"ppp", 16}, {"pp", 32},  {"p", 48},
                                                      {"mp", 64},  {"mf", 80},  {"f", 96},
                                                      {"ff", 112}, {"fff", 127}};
    return values;
}

inline const std::map<std::string, int>& ccNames() {
    static const std::map<std::string, int> names = {
        {"mod", 1},        {"breath", 2},     {"foot", 4},        {"porta_time", 5},
        {"volume", 7},     {"pan", 10},       {"expression", 11}, {"sustain", 64},
        {"porta", 65},     {"sostenuto", 66}, {"soft", 67},       {"legato", 68},
        {"resonance", 71}, {"release", 72},   {"attack", 73},     {"cutoff", 74}};
    return names;
}

inline const std::map<std::string, std::vector<int>>& chordQualities() {
    static const std::map<std::string, std::vector<int>> qualities = {{"", {0, 4, 7}},
                                                                      {"maj", {0, 4, 7}},
                                                                      {"m", {0, 3, 7}},
                                                                      {"min", {0, 3, 7}},
                                                                      {"dim", {0, 3, 6}},
                                                                      {"aug", {0, 4, 8}},
                                                                      {"+", {0, 4, 8}},
                                                                      {"sus2", {0, 2, 7}},
                                                                      {"sus4", {0, 5, 7}},
                                                                      {"sus", {0, 5, 7}},
                                                                      {"6", {0, 4, 7, 9}},
                                                                      {"m6", {0, 3, 7, 9}},
                                                                      {"7", {0, 4, 7, 10}},
                                                                      {"maj7", {0, 4, 7, 11}},
                                                                      {"M7", {0, 4, 7, 11}},
                                                                      {"m7", {0, 3, 7, 10}},
                                                                      {"min7", {0, 3, 7, 10}},
                                                                      {"mMaj7", {0, 3, 7, 11}},
                                                                      {"dim7", {0, 3, 6, 9}},
                                                                      {"m7b5", {0, 3, 6, 10}},
                                                                      {"ø", {0, 3, 6, 10}},
                                                                      {"7sus4", {0, 5, 7, 10}},
                                                                      {"add9", {0, 4, 7, 14}},
                                                                      {"9", {0, 4, 7, 10, 14}},
                                                                      {"maj9", {0, 4, 7, 11, 14}},
                                                                      {"m9", {0, 3, 7, 10, 14}},
                                                                      {"7b9", {0, 4, 7, 10, 13}},
                                                                      {"7#9", {0, 4, 7, 10, 15}},
                                                                      {"11", {0, 7, 10, 14, 17}},
                                                                      {"13", {0, 4, 10, 14, 21}},
                                                                      {"5", {0, 7}}};
    return qualities;
}

inline std::optional<std::int64_t> integer(std::string_view text) {
    std::int64_t value = 0;
    auto [p, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || p != text.data() + text.size()) return std::nullopt;
    return value;
}

inline std::optional<double> number(std::string_view text) {
    if (!text.empty() && text.front() == '+') text.remove_prefix(1);
    double value = 0;
    auto [p, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || p != text.data() + text.size() || !std::isfinite(value))
        return std::nullopt;
    return value;
}

inline std::int64_t barTicks(midi::Meter meter, int ppq) {
    if (meter.numerator < 1 || meter.denominator < 1 || meter.denominator > 128 ||
        (meter.denominator & (meter.denominator - 1)) != 0 || ppq < 1)
        return 0;
    return (std::int64_t(ppq) * 4 * meter.numerator) / meter.denominator;
}

inline std::optional<int> pitch(std::string_view text) {
    if (text.size() >= 2 && text[0] == 'n') {
        auto n = integer(text.substr(1));
        if (n && *n >= 0 && *n <= 127) return static_cast<int>(*n);
        return std::nullopt;
    }
    if (text.size() < 2) return std::nullopt;
    const char letter = static_cast<char>(std::toupper(static_cast<unsigned char>(text[0])));
    int semitone = 0;
    switch (letter) {
        case 'C':
            semitone = 0;
            break;
        case 'D':
            semitone = 2;
            break;
        case 'E':
            semitone = 4;
            break;
        case 'F':
            semitone = 5;
            break;
        case 'G':
            semitone = 7;
            break;
        case 'A':
            semitone = 9;
            break;
        case 'B':
            semitone = 11;
            break;
        default:
            return std::nullopt;
    }
    std::size_t i = 1;
    for (int j = 0; j < 2 && i < text.size() && (text[i] == '#' || text[i] == 'b'); ++j, ++i)
        semitone += text[i] == '#' ? 1 : -1;
    auto octave = integer(text.substr(i));
    if (!octave || *octave < -1 || *octave > 9) return std::nullopt;
    const auto result = (*octave + 1) * 12 + semitone;
    if (result < 0 || result > 127) return std::nullopt;
    return static_cast<int>(result);
}

inline std::string pitchName(int pitch) {
    static constexpr std::array<std::string_view, 12> names = {"C",  "C#", "D",  "Eb", "E",  "F",
                                                               "F#", "G",  "Ab", "A",  "Bb", "B"};
    return std::string(names[static_cast<std::size_t>(pitch % 12)]) +
           std::to_string(pitch / 12 - 1);
}

inline std::optional<std::int64_t> duration(std::string_view token, int ppq, bool& fraction) {
    fraction = false;
    std::int64_t total = 0;
    while (!token.empty()) {
        const auto split = token.find('+');
        const auto part = token.substr(0, split);
        if (part.empty()) return std::nullopt;
        std::int64_t numerator = 0, denominator = 1;
        if (part.size() > 1 && part.back() == 't') {
            auto n = integer(part.substr(0, part.size() - 1));
            if (!n || *n < 0) return std::nullopt;
            numerator = *n;
        } else {
            int divisor = 0;
            switch (part[0]) {
                case 'w':
                    divisor = 1;
                    break;
                case 'h':
                    divisor = 2;
                    break;
                case 'q':
                    divisor = 4;
                    break;
                case 'e':
                    divisor = 8;
                    break;
                case 's':
                    divisor = 16;
                    break;
                case 't':
                    divisor = 32;
                    break;
                case 'x':
                    divisor = 64;
                    break;
                default:
                    return std::nullopt;
            }
            numerator = std::int64_t(ppq) * 4;
            denominator = divisor;
            const auto suffix = part.substr(1);
            if (suffix == ".") {
                numerator *= 3;
                denominator *= 2;
            } else if (suffix == "..") {
                numerator *= 7;
                denominator *= 4;
            } else if (!suffix.empty()) {
                auto n = integer(suffix);
                if (!n || *n < 3 || *n > 1000000) return std::nullopt;
                int power = 1;
                while (power * 2 < *n) power *= 2;
                numerator *= power;
                denominator *= *n;
            }
        }
        if (numerator % denominator != 0) {
            fraction = true;
            return std::nullopt;
        }
        if (numerator / denominator > std::numeric_limits<std::int64_t>::max() - total)
            return std::nullopt;
        total += numerator / denominator;
        if (split == std::string_view::npos) break;
        token.remove_prefix(split + 1);
    }
    return total;
}

inline std::string durationName(std::int64_t ticks, int ppq) {
    static constexpr std::array<char, 7> units = {'w', 'h', 'q', 'e', 's', 't', 'x'};
    std::vector<std::pair<std::int64_t, std::string>> choices;
    for (const std::string& suffix :
         {std::string{}, std::string{"."}, std::string{".."}, std::string{"3"}, std::string{"5"},
          std::string{"6"}, std::string{"7"}, std::string{"9"}}) {
        for (const char unit : units) {
            const std::string name = std::string(1, unit) + suffix;
            bool fraction = false;
            auto value = duration(name, ppq, fraction);
            if (value && *value > 0) choices.emplace_back(*value, name);
        }
    }
    for (const auto& [value, name] : choices)
        if (value == ticks) return name;
    std::int64_t largestFirst = -1;
    std::string best;
    for (const auto& [a, aname] : choices)
        for (const auto& [b, bname] : choices)
            if (a <= ticks && b == ticks - a && a >= b && a > largestFirst) {
                largestFirst = a;
                best = aname + "+" + bname;
            }
    return largestFirst >= 0 ? best : std::to_string(ticks) + "t";
}

inline const std::vector<std::pair<std::string, std::uint8_t>>& gmDrums() {
    static const std::vector<std::pair<std::string, std::uint8_t>> drums = {
        {"kick", 36},    {"kick2", 35},     {"rim", 37},      {"snare", 38},  {"clap", 39},
        {"snare2", 40},  {"hat", 42},       {"pedal", 44},    {"ohat", 46},   {"tom_floor", 41},
        {"tom_low", 45}, {"tom_mid", 47},   {"tom_high", 50}, {"crash", 49},  {"crash2", 57},
        {"ride", 51},    {"ride_bell", 53}, {"china", 52},    {"splash", 55}, {"tamb", 54},
        {"cowbell", 56}, {"shaker", 70},    {"clave", 75},    {"cabasa", 69}};
    return drums;
}

}  // namespace supple::notation::internal
