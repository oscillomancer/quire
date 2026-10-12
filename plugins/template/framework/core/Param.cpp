// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "core/Param.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <initializer_list>

namespace supple {
namespace {

double clampTo(double v, double lo, double hi) { return v < lo ? lo : (v > hi ? hi : v); }

// --- locale-independent numbers ----------------------------------------------
//
// printf and strtod follow the process locale, and a plugin does not own the
// process: a host running with a German locale would otherwise see "1,5 dB"
// and fail to parse "1.5". Parameter text is therefore built and read here
// with a fixed '.' separator.

constexpr double kPow10[] = {1e0, 1e1, 1e2, 1e3, 1e4, 1e5, 1e6, 1e7, 1e8, 1e9};

int clampDecimals(int d) { return d < 0 ? 0 : (d > 9 ? 9 : d); }

double roundTo(double v, int decimals) {
    const double s = kPow10[clampDecimals(decimals)];
    return std::round(v * s) / s;
}

void writeFixed(double v, int decimals, bool plus, char* out, size_t size) {
    decimals = clampDecimals(decimals);
    const double scaled = std::round(v * kPow10[decimals]);
    // Anything a parameter displays is far inside this; refuse to overflow.
    const double limit = 9.0e17;
    const long long n = static_cast<long long>(clampTo(scaled, -limit, limit));
    const bool negative = n < 0;
    const unsigned long long u = static_cast<unsigned long long>(negative ? -n : n);
    const unsigned long long unit = static_cast<unsigned long long>(kPow10[decimals]);
    const char* sign = (negative && u) ? "-" : (plus && u) ? "+" : "";
    char text[64];
    if (decimals == 0)
        std::snprintf(text, sizeof(text), "%s%llu", sign, u);
    else
        std::snprintf(text, sizeof(text), "%s%llu.%0*llu", sign, u / unit, decimals, u % unit);
    if (size == 0) return;
    const size_t length = std::min(std::strlen(text), size - 1);
    std::memcpy(out, text, length);
    out[length] = '\0';
}

// Decimal places from magnitude, re-chosen once on the rounded value so that a
// number rounding across a threshold (99.96 -> 100) is shown the way the
// parsed-back value would be. That is what makes text round-trip exactly.
template <class Rule>
double settle(double v, Rule rule, int& decimals) {
    decimals = rule(std::fabs(v));
    double r = roundTo(v, decimals);
    const int again = rule(std::fabs(r));
    if (again != decimals) {
        decimals = again;
        r = roundTo(v, decimals);
    }
    return r;
}

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
char lower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

// Reads [sign] digits [. digits] [e [sign] digits]. Accepts U+2212 as a minus,
// since that is what the editor draws. Fails on no digits or a non-finite
// result, which is how "nan", "inf" and "1e999" are rejected.
bool readNumber(const char*& s, double& out) {
    while (isSpace(*s)) ++s;
    bool negative = false;
    if (*s == '+' || *s == '-') {
        negative = *s == '-';
        ++s;
    } else if (static_cast<unsigned char>(s[0]) == 0xE2 && static_cast<unsigned char>(s[1]) == 0x88 &&
               static_cast<unsigned char>(s[2]) == 0x92) {
        negative = true;
        s += 3;
    }
    double mantissa = 0.0;
    int exponent = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') {
        mantissa = mantissa * 10.0 + (*s - '0');
        ++digits;
        ++s;
    }
    if (*s == '.') {
        ++s;
        while (*s >= '0' && *s <= '9') {
            mantissa = mantissa * 10.0 + (*s - '0');
            --exponent;
            ++digits;
            ++s;
        }
    }
    if (digits == 0) return false;
    if ((*s == 'e' || *s == 'E') &&
        ((s[1] >= '0' && s[1] <= '9') ||
         ((s[1] == '+' || s[1] == '-') && s[2] >= '0' && s[2] <= '9'))) {
        ++s;
        const bool expNegative = *s == '-';
        if (*s == '+' || *s == '-') ++s;
        int e = 0;
        while (*s >= '0' && *s <= '9') {
            if (e < 100000) e = e * 10 + (*s - '0');
            ++s;
        }
        exponent += expNegative ? -e : e;
    }
    double v = mantissa * std::pow(10.0, exponent);
    if (negative) v = -v;
    if (!std::isfinite(v)) return false;
    out = v;
    return true;
}

// The rest of the text after the number, lower-cased with spaces removed.
void restToken(const char* s, char* token, size_t size) {
    size_t n = 0;
    for (; *s && n + 1 < size; ++s)
        if (!isSpace(*s)) token[n++] = lower(*s);
    token[n] = '\0';
    if (*s) token[0] = '\x01';  // too long to be any unit: never matches
}

bool tokenIs(const char* token, const char* candidate) {
    if (!candidate) return false;
    char want[32];
    restToken(candidate, want, sizeof(want));
    return std::strcmp(token, want) == 0;
}

bool sameText(const char* a, const char* b) {
    while (isSpace(*a)) ++a;
    while (isSpace(*b)) ++b;
    size_t na = std::strlen(a), nb = std::strlen(b);
    while (na && isSpace(a[na - 1])) --na;
    while (nb && isSpace(b[nb - 1])) --nb;
    if (na != nb) return false;
    for (size_t i = 0; i < na; ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

bool isPlainInteger(const char* s, long& out) {
    while (isSpace(*s)) ++s;
    const char* start = s;
    long v = 0;
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        if (v > 1000000) return false;
        ++s;
    }
    if (s == start) return false;
    while (isSpace(*s)) ++s;
    if (*s) return false;
    out = v;
    return true;
}

const char* baseUnitText(Unit unit) {
    switch (unit) {
        case Unit::Decibels: return "dB";
        case Unit::Hertz: return "Hz";
        case Unit::Milliseconds: return "ms";
        case Unit::Seconds: return "s";
        case Unit::Percent: return "%";
        case Unit::Ratio: return ":1";
        case Unit::Semitones: return "st";
        case Unit::Cents: return "ct";
        case Unit::Degrees: return "deg";
        case Unit::Samples: return "smp";
        case Unit::Octaves: return "oct";
        case Unit::Bpm: return "BPM";
        case Unit::Pan:
        case Unit::Note:
        case Unit::None: break;
    }
    return "";
}

void copyText(char* out, size_t size, const char* text) { std::snprintf(out, size, "%s", text ? text : ""); }

const char* const kNoteNames[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

// "C4" is 60: octave numbers change at C, and key 0 is "C-1".
void noteName(double key, char* out, size_t size) {
    const long k = std::lround(key);
    const long octave = (k >= 0 ? k / 12 : (k - 11) / 12) - 1;
    std::snprintf(out, size, "%s%ld", kNoteNames[((k % 12) + 12) % 12], octave);
}

// A note name, any case, with an optional sharp or flat ("c#4", "Db3", "A-1").
bool readNoteName(const char* s, double& key) {
    while (isSpace(*s)) ++s;
    static const int kOffsets[] = {9, 11, 0, 2, 4, 5, 7};  // a b c d e f g
    const char letter = lower(*s);
    if (letter < 'a' || letter > 'g') return false;
    int semitone = kOffsets[letter - 'a'];
    ++s;
    if (*s == '#') {
        ++semitone;
        ++s;
    } else if (*s == 'b' && (s[1] == '-' || (s[1] >= '0' && s[1] <= '9'))) {
        --semitone;
        ++s;
    }
    bool negative = false;
    if (*s == '-') {
        negative = true;
        ++s;
    }
    if (*s < '0' || *s > '9') return false;
    int octave = 0;
    while (*s >= '0' && *s <= '9') {
        octave = octave * 10 + (*s - '0');
        if (octave > 100) return false;
        ++s;
    }
    while (isSpace(*s)) ++s;
    if (*s) return false;
    key = (negative ? -octave : octave) * 12.0 + 12.0 + semitone;
    return true;
}

// "50L", "L 50", "C", "centre", "50R", or a signed percentage ("-50" is 50L).
bool readPan(const char* text, double& plain) {
    char token[32];
    restToken(text, token, sizeof(token));
    if (!std::strcmp(token, "c") || !std::strcmp(token, "center") || !std::strcmp(token, "centre")) {
        plain = 0.0;
        return true;
    }
    double side = 0.0;
    const char* s = token;
    if (*s == 'l' || *s == 'r') {
        side = *s == 'l' ? -1.0 : 1.0;
        ++s;
    }
    double v = 0.0;
    if (!readNumber(s, v)) return false;
    if (*s == '%') ++s;
    if (side == 0.0 && (*s == 'l' || *s == 'r')) {
        side = *s == 'l' ? -1.0 : 1.0;
        ++s;
    }
    if (*s) return false;
    if (side != 0.0) {
        if (v < 0.0) return false;  // "-50L" means nothing
        v *= side;
    }
    plain = v / 100.0;
    return std::isfinite(plain);
}

// What an offAtMinimum() parameter shows at its minimum, and accepts for it.
void offText(const ParamDef& p, ValueText& t) {
    if (p.unit == Unit::Decibels) {
        copyText(t.number, sizeof(t.number), "-inf");
        copyText(t.unit, sizeof(t.unit), p.suffix ? p.suffix : "dB");
    } else {
        copyText(t.number, sizeof(t.number), "Off");
        t.unit[0] = '\0';
    }
}

bool readOff(const ParamDef& p, const char* text) {
    char token[32];
    restToken(text, token, sizeof(token));
    if (!std::strcmp(token, "off")) return true;
    if (p.unit != Unit::Decibels) return false;
    // "-inf", in ASCII or as the editor draws it, with or without the unit.
    for (const char* minus : {"-", "\xE2\x88\x92"})
        for (const char* inf : {"inf", "infinity", "\xE2\x88\x9E"})
            for (const char* unit : {"", "db", p.suffix}) {
                if (!unit) continue;
                char want[48];
                std::snprintf(want, sizeof(want), "%s%s%s", minus, inf, unit);
                char wanted[48];
                restToken(want, wanted, sizeof(wanted));
                if (!std::strcmp(token, wanted)) return true;
            }
    return false;
}


// Milliseconds, switching to seconds at one second.
void describeTime(const ParamDef& p, double ms, ValueText& t) {
    const bool plus = p.bipolar();
    int d = 0;
    auto msRule = [&](double x) { return p.precision >= 0 ? p.precision : x < 10.0 ? 2 : x < 100.0 ? 1 : 0; };
    const double r = settle(ms, msRule, d);
    if (std::fabs(r) < 1000.0) {
        writeFixed(r, d, plus, t.number, sizeof(t.number));
        copyText(t.unit, sizeof(t.unit), "ms");
        return;
    }
    auto sRule = [&](double x) { return p.precision >= 0 ? p.precision : x < 10.0 ? 2 : x < 100.0 ? 1 : 0; };
    const double rs = settle(ms * 0.001, sRule, d);
    writeFixed(rs, d, plus, t.number, sizeof(t.number));
    copyText(t.unit, sizeof(t.unit), "s");
}

}  // namespace

// --- conversions --------------------------------------------------------------

namespace {

// Plain value <-> position along the taper, 0..1.
double fromPosition(const ParamDef& p, double t) {
    t = clampTo(t, 0.0, 1.0);
    const double v = p.taper == Taper::Log ? p.min * std::pow(p.max / p.min, t)
                                           : p.min + (p.max - p.min) * std::pow(t, p.curve);
    return clampTo(v, p.min, p.max);
}

double toPosition(const ParamDef& p, double plain) {
    const double v = clampTo(plain, p.min, p.max);
    if (p.max <= p.min) return 0.0;
    const double t = p.taper == Taper::Log ? std::log(v / p.min) / std::log(p.max / p.min)
                                           : std::pow((v - p.min) / (p.max - p.min), 1.0 / p.curve);
    return clampTo(t, 0.0, 1.0);
}

}  // namespace

double toPlain(const ParamDef& p, double host) {
    if (p.hostTapered()) return fromPosition(p, host);
    const double v = clampTo(host, p.min, p.max);
    return p.stepped() ? std::round(v) : v;
}

double toHost(const ParamDef& p, double plain) {
    if (p.hostTapered()) return toPosition(p, plain);
    const double v = clampTo(plain, p.min, p.max);
    return p.stepped() ? std::round(v) : v;
}

double hostDefault(const ParamDef& p) { return toHost(p, p.def); }

double sanitizeHost(const ParamDef& p, double host) {
    if (!std::isfinite(host)) return hostDefault(p);
    const double v = clampTo(host, p.hostMin(), p.hostMax());
    return p.stepped() ? std::round(v) : v;
}

double toNormalized(const ParamDef& p, double host) {
    // A plain host value still moves the knob along its taper.
    if (p.tapered() && !p.hostTapered()) return toPosition(p, host);
    const double lo = p.hostMin(), hi = p.hostMax();
    return hi > lo ? clampTo((host - lo) / (hi - lo), 0.0, 1.0) : 0.0;
}

double fromNormalized(const ParamDef& p, double normalized) {
    if (p.tapered() && !p.hostTapered()) return fromPosition(p, normalized);
    const double lo = p.hostMin(), hi = p.hostMax();
    const double v = lo + clampTo(normalized, 0.0, 1.0) * (hi - lo);
    return p.stepped() ? std::round(v) : v;
}

// --- text -------------------------------------------------------------------

ValueText describe(const ParamDef& p, double host) {
    ValueText t{};
    const double v = toPlain(p, sanitizeHost(p, host));
    const bool plus = p.bipolar();

    if (p.off(v)) {
        offText(p, t);
        return t;
    }
    if (p.formatter) {
        p.formatter(p, v, t);
        t.number[sizeof(t.number) - 1] = '\0';
        t.unit[sizeof(t.unit) - 1] = '\0';
        return t;
    }

    if (p.kind == ParamKind::Choice || p.kind == ParamKind::Toggle) {
        const int i = static_cast<int>(std::lround(v - p.min));
        if (p.labels && i >= 0 && i < p.labelCount)
            copyText(t.number, sizeof(t.number), p.labels[i]);
        else
            writeFixed(v, 0, false, t.number, sizeof(t.number));
        return t;
    }
    if (p.kind == ParamKind::Integer) {
        if (p.unit == Unit::Note) {
            noteName(v, t.number, sizeof(t.number));
            return t;
        }
        writeFixed(v, 0, plus, t.number, sizeof(t.number));
        copyText(t.unit, sizeof(t.unit), p.suffix ? p.suffix : baseUnitText(p.unit));
        return t;
    }

    int d = 0;
    switch (p.unit) {
        case Unit::Hertz: {
            auto hzRule = [&](double x) { return p.precision >= 0 ? p.precision : x < 100.0 ? 1 : 0; };
            const double r = settle(v, hzRule, d);
            if (std::fabs(r) < 1000.0) {
                writeFixed(r, d, plus, t.number, sizeof(t.number));
                copyText(t.unit, sizeof(t.unit), "Hz");
            } else {
                auto kRule = [&](double x) { return p.precision >= 0 ? p.precision : x < 10.0 ? 2 : 1; };
                const double rounded = settle(v * 0.001, kRule, d);
                writeFixed(rounded, d, plus, t.number, sizeof(t.number));
                copyText(t.unit, sizeof(t.unit), "kHz");
            }
            return t;
        }
        case Unit::Milliseconds: describeTime(p, v, t); return t;
        case Unit::Seconds: describeTime(p, v * 1000.0, t); return t;
        case Unit::Percent: {
            auto rule = [&](double) { return p.precision >= 0 ? p.precision : 0; };
            const double rounded = settle(v * 100.0, rule, d);
            writeFixed(rounded, d, plus, t.number, sizeof(t.number));
            copyText(t.unit, sizeof(t.unit), p.suffix ? p.suffix : "%");
            return t;
        }
        case Unit::Ratio: {
            auto rule = [&](double x) { return p.precision >= 0 ? p.precision : x < 10.0 ? 1 : 0; };
            const double rounded = settle(v, rule, d);
            writeFixed(rounded, d, false, t.number, sizeof(t.number));
            copyText(t.unit, sizeof(t.unit), p.suffix ? p.suffix : ":1");
            return t;
        }
        case Unit::Degrees:
        case Unit::Samples: {
            // Degrees: whole above ten. Samples: like a time, fractions only
            // where they are a visible part of the value.
            const bool samples = p.unit == Unit::Samples;
            auto rule = [&](double x) {
                if (p.precision >= 0) return p.precision;
                if (x < 10.0) return samples ? 2 : 1;
                return samples && x < 100.0 ? 1 : 0;
            };
            const double rounded = settle(v, rule, d);
            writeFixed(rounded, d, plus, t.number, sizeof(t.number));
            copyText(t.unit, sizeof(t.unit), p.suffix ? p.suffix : baseUnitText(p.unit));
            return t;
        }
        case Unit::Pan: {
            auto rule = [&](double) { return p.precision >= 0 ? p.precision : 0; };
            const double rounded = settle(std::fabs(v) * 100.0, rule, d);
            if (rounded == 0.0) {
                copyText(t.number, sizeof(t.number), "C");
                return t;
            }
            char amount[24];
            writeFixed(rounded, d, false, amount, sizeof(amount));
            std::snprintf(t.number, sizeof(t.number), "%s%s", amount, v < 0.0 ? "L" : "R");
            return t;
        }
        case Unit::Note:  // a continuous note is a number of semitones
        case Unit::Decibels:
        case Unit::Semitones:
        case Unit::Cents:
        case Unit::Octaves:
        case Unit::Bpm:
        case Unit::None: {
            const int fallback = p.unit == Unit::None || p.unit == Unit::Octaves ? 2 : p.unit == Unit::Cents ? 0 : 1;
            auto rule = [&](double) { return p.precision >= 0 ? p.precision : fallback; };
            const double rounded = settle(v, rule, d);
            writeFixed(rounded, d, plus, t.number, sizeof(t.number));
            copyText(t.unit, sizeof(t.unit), p.suffix ? p.suffix : baseUnitText(p.unit));
            return t;
        }
    }
    return t;
}

void formatValue(const ParamDef& p, double host, char* out, size_t size) {
    if (!out || size == 0) return;
    const ValueText t = describe(p, host);
    if (!t.unit[0])
        std::snprintf(out, size, "%s", t.number);
    else if (t.unit[0] == ':')
        std::snprintf(out, size, "%s%s", t.number, t.unit);
    else
        std::snprintf(out, size, "%s %s", t.number, t.unit);
}

void keyName(int key, char* out, size_t size) { noteName(key, out, size); }

bool parseValue(const ParamDef& p, const char* text, double& host) {
    if (!text) return false;

    if (p.is(pflag::kOffAtMin) && readOff(p, text)) {
        host = toHost(p, p.min);
        return true;
    }
    if (p.parser) {
        double plain = 0.0;
        if (!p.parser(p, text, plain) || !std::isfinite(plain)) return false;
        host = toHost(p, plain);
        return true;
    }
    if (p.unit == Unit::Pan) {
        double plain = 0.0;
        if (!readPan(text, plain)) return false;
        host = toHost(p, plain);
        return true;
    }
    if (p.unit == Unit::Note && p.kind == ParamKind::Integer) {
        double key = 0.0;
        if (readNoteName(text, key)) {
            host = toHost(p, key);
            return true;
        }
        // otherwise a plain note number, below
    }

    if (p.kind == ParamKind::Choice || p.kind == ParamKind::Toggle) {
        for (int i = 0; p.labels && i < p.labelCount; ++i) {
            if (sameText(text, p.labels[i])) {
                host = p.min + i;
                return true;
            }
        }
        if (p.kind == ParamKind::Toggle) {
            for (const char* on : {"on", "true", "yes"})
                if (sameText(text, on)) { host = 1.0; return true; }
            for (const char* off : {"off", "false", "no"})
                if (sameText(text, off)) { host = 0.0; return true; }
        }
        long index = 0;
        if (isPlainInteger(text, index) && index >= p.min && index <= p.max) {
            host = static_cast<double>(index);
            return true;
        }
        return false;
    }

    const char* s = text;
    double v = 0.0;
    if (!readNumber(s, v)) return false;
    char token[32];
    restToken(s, token, sizeof(token));
    const bool bare = token[0] == '\0';

    double scale = 1.0;
    bool ok = bare;
    switch (p.unit) {
        case Unit::Hertz:
            if (tokenIs(token, "hz")) ok = true;
            else if (tokenIs(token, "k") || tokenIs(token, "khz")) { ok = true; scale = 1000.0; }
            break;
        case Unit::Milliseconds:
            if (tokenIs(token, "ms")) ok = true;
            else if (tokenIs(token, "s") || tokenIs(token, "sec")) { ok = true; scale = 1000.0; }
            break;
        case Unit::Seconds:
            if (tokenIs(token, "s") || tokenIs(token, "sec")) ok = true;
            else if (tokenIs(token, "ms")) { ok = true; scale = 0.001; }
            break;
        case Unit::Percent:
            ok = bare || tokenIs(token, "%") || tokenIs(token, p.suffix);
            scale = 0.01;  // typed the way it is shown, with or without the sign
            break;
        case Unit::Ratio: ok = bare || tokenIs(token, ":1") || tokenIs(token, p.suffix); break;
        case Unit::Decibels: ok = bare || tokenIs(token, "db") || tokenIs(token, p.suffix); break;
        case Unit::Semitones:
            ok = bare || tokenIs(token, "st") || tokenIs(token, "semi") || tokenIs(token, "semitones") ||
                 tokenIs(token, p.suffix);
            break;
        case Unit::Cents:
            ok = bare || tokenIs(token, "ct") || tokenIs(token, "c") || tokenIs(token, "cents") ||
                 tokenIs(token, p.suffix);
            break;
        case Unit::Degrees:
            ok = bare || tokenIs(token, "deg") || tokenIs(token, "degrees") || tokenIs(token, "\xC2\xB0") ||
                 tokenIs(token, p.suffix);
            break;
        case Unit::Samples:
            ok = bare || tokenIs(token, "smp") || tokenIs(token, "spl") || tokenIs(token, "sample") ||
                 tokenIs(token, "samples") || tokenIs(token, p.suffix);
            break;
        case Unit::Octaves:
            ok = bare || tokenIs(token, "oct") || tokenIs(token, "octave") || tokenIs(token, "octaves") ||
                 tokenIs(token, p.suffix);
            break;
        case Unit::Bpm: ok = bare || tokenIs(token, "bpm") || tokenIs(token, p.suffix); break;
        case Unit::Pan:
        case Unit::Note:
        case Unit::None: ok = bare || tokenIs(token, p.suffix); break;
    }
    if (!ok) return false;
    v *= scale;
    if (!std::isfinite(v)) return false;
    host = toHost(p, v);
    return true;
}

}  // namespace supple
