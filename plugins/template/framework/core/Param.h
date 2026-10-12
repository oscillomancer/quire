// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Parameter descriptions, and the conversions and text that follow from them.
//
// A product describes each parameter exactly once, in product/Params.h, using
// the factories at the bottom of this file. The CLAP parameter list (and so
// VST3 and AU through the wrapper), the editor, saved state, the offline
// renderer and the tests all take names, ranges, tapers, units and display
// text from that one description. Nothing else may restate them.
//
// Three value spaces, kept strictly apart:
//
//   plain       engineering units: Hz, dB, ms, a fraction for percentages, a
//               choice index. This is what DSP reads.
//   host        what the host stores, automates and modulates. Equal to plain
//               for linear and stepped parameters. For a tapered parameter it
//               is the 0..1 position on the taper, so that a host's linear
//               automation lane and generic slider move the way the editor's
//               knob does: a frequency lane drawn in Hz spends 99% of its
//               height above 200 Hz.
//   normalized  0..1, what a knob or slider displays. Linear in host space,
//               except for a parameter marked plainHostValue() (below).
//
// plainHostValue() keeps a tapered parameter's host value in plain units: the
// knob still follows the taper, but hosts store and automate Hz, not the
// position. It exists for products that moved onto this framework with
// sessions and automation already saved that way; a new parameter should not
// use it.
//
// Host text is ASCII (hosts draw it in their own fonts); the editor may
// prettify it, e.g. with a true minus sign, a degree sign or an infinity.
//
// Beyond the factories, a parameter may be:
//
//   .output()         read-only: the processor reports it (setOutput), hosts
//                     and the editor show it, state does not keep it. A gain
//                     reduction meter a host can display and record.
//   .offAtMinimum()   its minimum means off: "-inf dB" for a level, "Off"
//                     otherwise. ParamValues::gain() is exactly 0 there.
//   .smoothed(ms)     the framework glides it, per sample, for Block::smoothed.
//   .formatted(f, p)  its own text, both ways, for what no Unit describes.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace supple {

enum class ParamKind : uint8_t {
    Continuous,  // any value in [min, max]
    Integer,     // whole numbers in [min, max]
    Choice,      // an index into labels
    Toggle,      // 0 or 1
};

enum class Taper : uint8_t {
    Linear,  // host value is the plain value
    Log,     // equal ratios per unit of travel; requires min > 0
    Power,   // plain = min + (max - min) * t^curve; curve > 1 favours the low end
};

enum class Unit : uint8_t {
    None,
    Decibels,
    Hertz,         // shown as Hz or kHz
    Milliseconds,  // shown as ms or s
    Seconds,       // shown as ms or s
    Percent,       // plain value is a 0..1 fraction, shown and typed as percent
    Ratio,         // shown as "4.0:1"
    Semitones,
    Cents,
    Degrees,  // "deg" to hosts, a degree sign in the editor
    Samples,
    Octaves,
    Bpm,
    Pan,      // -1 (left) to +1 (right): "50L", "C", "50R"
    Note,     // a MIDI note number, by name: 60 is "C4", 69 is "A4"
};

namespace pflag {
inline constexpr uint32_t kAutomatable = 1u << 0;
inline constexpr uint32_t kModulatable = 1u << 1;  // accepts CLAP non-destructive modulation
inline constexpr uint32_t kHidden = 1u << 2;       // retired: still loads from old sessions
inline constexpr uint32_t kBypass = 1u << 3;       // the framework's latency-compensated bypass
inline constexpr uint32_t kRestart = 1u << 4;      // changing it while active asks the host to restart
inline constexpr uint32_t kPlainHost = 1u << 5;    // tapered, but the host value is the plain value
inline constexpr uint32_t kOutput = 1u << 6;       // read-only, reported by the processor, never saved
inline constexpr uint32_t kOffAtMin = 1u << 7;     // the minimum means off: "-inf dB", "Off"
inline constexpr uint32_t kPolyMod = 1u << 8;      // accepts CLAP modulation per note, too
inline constexpr uint32_t kNormalHost = 1u << 9;   // continuous: the host value is the 0..1 position, linear too
}  // namespace pflag

inline constexpr const char* kOffOnLabels[] = {"Off", "On"};

// A value split for display: the editor draws number and unit separately.
struct ValueText {
    char number[32];
    char unit[16];
};

struct ParamDef;
// A parameter's own text, for what no Unit describes (see .formatted()).
// The formatter writes ASCII from a plain value; the parser reads back what
// it wrote, plus whatever a person would type, and returns false otherwise.
// The framework clamps and rounds what the parser returns, and handles an
// offAtMinimum() parameter's "off" itself before either is called.
using Formatter = void (*)(const ParamDef& p, double plain, ValueText& out);
using Parser = bool (*)(const ParamDef& p, const char* text, double& plain);

struct ParamDef {
    uint32_t id = 0;
    const char* name = "";
    ParamKind kind = ParamKind::Continuous;
    Unit unit = Unit::None;
    Taper taper = Taper::Linear;
    double min = 0.0;
    double max = 1.0;
    double def = 0.0;
    double curve = 1.0;            // exponent for Taper::Power
    int precision = -1;            // decimals; -1 picks them from unit and magnitude
    const char* group = "";        // CLAP module path, e.g. "Filter" or "Band 3/Dynamics"
    const char* label = nullptr;   // short editor label; the name when absent
    const char* suffix = nullptr;  // replaces the unit text for non-scaling units ("dBTP", "x")
    const char* const* labels = nullptr;  // Choice and Toggle display text
    int labelCount = 0;
    uint32_t flags = pflag::kAutomatable;
    double smoothing = 0.0;          // ms the framework glides it over; 0 when it does not
    Formatter formatter = nullptr;   // custom text, with parser; null for the unit's
    Parser parser = nullptr;
    const char* description = nullptr;  // a sentence of help: the editor's tooltip

    // Modifiers. Each returns an adjusted copy, so a table reads as one
    // expression per parameter: frequency(...).in("Filter").modulatable().
    constexpr ParamDef in(const char* path) const { ParamDef p = *this; p.group = path; return p; }
    constexpr ParamDef shortLabel(const char* text) const { ParamDef p = *this; p.label = text; return p; }
    constexpr ParamDef decimals(int digits) const { ParamDef p = *this; p.precision = digits; return p; }
    constexpr ParamDef unitText(const char* text) const { ParamDef p = *this; p.suffix = text; return p; }
    // What it does, in a sentence: the editor shows it as the control's
    // tooltip. Hosts have nowhere to show it.
    constexpr ParamDef help(const char* text) const { ParamDef p = *this; p.description = text; return p; }
    constexpr ParamDef modulatable() const { return with(pflag::kModulatable); }
    // Modulatable per note as well (CLAP polyphonic modulation): the host
    // may modulate each voice apart, and the processor receives it through
    // noteModulation(). Needs note input.
    constexpr ParamDef polyModulatable() const { return with(pflag::kModulatable | pflag::kPolyMod); }
    constexpr ParamDef bypass() const { return with(pflag::kBypass); }
    constexpr ParamDef restartOnChange() const { return with(pflag::kRestart); }
    // See the top of this file: only for sessions saved before the move.
    constexpr ParamDef plainHostValue() const { return with(pflag::kPlainHost); }
    // A continuous parameter's host value is its 0..1 position along the
    // taper even when the taper is linear: every continuous parameter then
    // reaches hosts the same way, as hosts that keep normalized values
    // expect. A tapered one is so already.
    constexpr ParamDef normalizedHostValue() const { return with(pflag::kNormalHost); }
    constexpr ParamDef notAutomatable() const { return without(pflag::kAutomatable | pflag::kModulatable | pflag::kPolyMod); }
    // Keep a removed parameter's ID reserved and loadable, invisible to hosts.
    constexpr ParamDef retired() const { return with(pflag::kHidden).notAutomatable(); }
    // Read-only: reported by the processor with setOutput(), shown to hosts
    // and the editor, never automated and never saved.
    constexpr ParamDef output() const { return notAutomatable().with(pflag::kOutput); }
    // The minimum means off: a level reads "-inf dB" there and
    // ParamValues::gain() is exactly 0; anything else reads "Off".
    constexpr ParamDef offAtMinimum() const { return with(pflag::kOffAtMin); }
    // The framework glides this parameter to every change with a time
    // constant of `ms` (in log space for a log taper), per sample, and the
    // processor reads it from Block::smoothed(id). It settles exactly.
    constexpr ParamDef smoothed(double ms) const { ParamDef p = *this; p.smoothing = ms; return p; }
    constexpr ParamDef formatted(Formatter format, Parser parse) const {
        ParamDef p = *this;
        p.formatter = format;
        p.parser = parse;
        return p;
    }

    constexpr bool is(uint32_t flag) const { return (flags & flag) != 0; }
    constexpr bool stepped() const { return kind != ParamKind::Continuous; }
    constexpr bool tapered() const { return kind == ParamKind::Continuous && taper != Taper::Linear; }
    // The host value is the taper position (0..1) rather than the plain value.
    constexpr bool hostTapered() const {
        return kind == ParamKind::Continuous && !is(pflag::kPlainHost) &&
               (taper != Taper::Linear || is(pflag::kNormalHost));
    }
    constexpr bool bipolar() const { return min < 0.0 && max > 0.0; }
    constexpr double hostMin() const { return hostTapered() ? 0.0 : min; }
    constexpr double hostMax() const { return hostTapered() ? 1.0 : max; }
    constexpr const char* displayLabel() const { return label ? label : name; }
    constexpr bool isOutput() const { return is(pflag::kOutput); }
    // Off, for a parameter marked offAtMinimum(): exactly its minimum.
    constexpr bool off(double plain) const { return is(pflag::kOffAtMin) && plain <= min; }

  private:
    constexpr ParamDef with(uint32_t f) const { ParamDef p = *this; p.flags |= f; return p; }
    constexpr ParamDef without(uint32_t f) const { ParamDef p = *this; p.flags &= ~f; return p; }
};

// --- conversions ------------------------------------------------------------

// A level in decibels as a linear gain: exactly 0 at the minimum of a
// parameter marked offAtMinimum(), exactly 1 at 0 dB. For a level the
// framework smooths, convert each sample of Block::smoothed with this.
inline double levelGain(const ParamDef& p, double db) {
    return p.off(db) ? 0.0 : db == 0.0 ? 1.0 : std::pow(10.0, db / 20.0);
}

double toPlain(const ParamDef& p, double host);
double toHost(const ParamDef& p, double plain);
double hostDefault(const ParamDef& p);
// Clamps into the host range, rounds stepped values, and maps non-finite
// input to the default. Every value arriving from outside goes through here.
double sanitizeHost(const ParamDef& p, double host);
// The plain value a modulation offset of `amount` host units makes of
// `plain`, clamped to the range: a voice's own value under per-note
// modulation (NoteModulation).
inline double modulatedPlain(const ParamDef& p, double plain, double amount) {
    return amount == 0.0 ? plain : toPlain(p, sanitizeHost(p, toHost(p, plain) + amount));
}
double toNormalized(const ParamDef& p, double host);
double fromNormalized(const ParamDef& p, double normalized);

// --- text -------------------------------------------------------------------

ValueText describe(const ParamDef& p, double host);
// Host-facing text: number and unit joined, ASCII only.
void formatValue(const ParamDef& p, double host, char* out, size_t size);
// Accepts what formatValue prints, plus the obvious variants a person types:
// "1.2k", "1200 hz", "50" for a percentage, "250ms" or "0.25 s", a choice's
// label in any case, "L30" or "-30" for a pan, "c#4" or "61" for a note.
// Rejects empty, non-numeric and non-finite text, and text followed by a unit
// this parameter does not use. Clamps to the range. "-inf" is accepted only
// by a level marked offAtMinimum(), and means its minimum.
bool parseValue(const ParamDef& p, const char* text, double& host);
// A MIDI key's name, as midiNote() parameters show it: 60 is "C4".
void keyName(int key, char* out, size_t size);

// --- tempo --------------------------------------------------------------------

// Every straight, triplet and dotted note from 1/64 to a whole note, then two
// and four bars, shortest first: the options of a noteValue() parameter.
inline constexpr const char* kNoteValueLabels[] = {
    "1/64T", "1/64", "1/32T", "1/64.", "1/32", "1/16T", "1/32.", "1/16", "1/8T", "1/16.", "1/8", "1/4T",
    "1/8.",  "1/4",  "1/2T",  "1/4.",  "1/2",  "1/1T",  "1/2.",  "1/1",  "1/1.", "2/1",   "4/1"};
// Their lengths in quarter notes (beats), in the same order.
inline constexpr double kNoteValueBeats[] = {
    1.0 / 24.0, 1.0 / 16.0, 1.0 / 12.0, 3.0 / 32.0, 1.0 / 8.0, 1.0 / 6.0, 3.0 / 16.0, 1.0 / 4.0,
    1.0 / 3.0,  3.0 / 8.0,  1.0 / 2.0,  2.0 / 3.0,  3.0 / 4.0, 1.0,       4.0 / 3.0,  3.0 / 2.0,
    2.0,        8.0 / 3.0,  3.0,        4.0,        6.0,       8.0,       16.0};
static_assert(std::size(kNoteValueLabels) == std::size(kNoteValueBeats));

constexpr double noteValueBeats(int index) {
    return kNoteValueBeats[index < 0 ? 0 : index >= static_cast<int>(std::size(kNoteValueBeats))
                                               ? static_cast<int>(std::size(kNoteValueBeats)) - 1
                                               : index];
}

namespace detail {
constexpr bool sameLabel(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}
constexpr int noteValueIndex(const char* label) {
    for (int i = 0; i < static_cast<int>(std::size(kNoteValueLabels)); ++i)
        if (sameLabel(label, kNoteValueLabels[i])) return i;
    return -1;  // the table's range check reports it
}
}  // namespace detail

// --- factories ----------------------------------------------------------------

constexpr ParamDef continuous(uint32_t id, const char* name, double min, double max, double def,
                              Unit unit = Unit::None, Taper taper = Taper::Linear,
                              double curve = 1.0) {
    ParamDef p;
    p.id = id;
    p.name = name;
    p.min = min;
    p.max = max;
    p.def = def;
    p.unit = unit;
    p.taper = taper;
    p.curve = curve;
    return p;
}

constexpr ParamDef decibels(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Decibels);
}

constexpr ParamDef frequency(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Hertz, Taper::Log);
}

// Log when the range excludes zero, otherwise a cubic so short times still get
// most of the travel.
constexpr ParamDef milliseconds(uint32_t id, const char* name, double min, double max, double def) {
    return min > 0.0 ? continuous(id, name, min, max, def, Unit::Milliseconds, Taper::Log)
                     : continuous(id, name, min, max, def, Unit::Milliseconds, Taper::Power, 3.0);
}

constexpr ParamDef seconds(uint32_t id, const char* name, double min, double max, double def) {
    return min > 0.0 ? continuous(id, name, min, max, def, Unit::Seconds, Taper::Log)
                     : continuous(id, name, min, max, def, Unit::Seconds, Taper::Power, 3.0);
}

// The plain value is a fraction; defaultPercent is given the way it is shown.
constexpr ParamDef percent(uint32_t id, const char* name, double defaultPercent,
                           double minPercent = 0.0, double maxPercent = 100.0) {
    return continuous(id, name, minPercent * 0.01, maxPercent * 0.01, defaultPercent * 0.01,
                      Unit::Percent);
}

constexpr ParamDef ratio(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Ratio, Taper::Log);
}

constexpr ParamDef semitones(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Semitones);
}

constexpr ParamDef cents(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Cents);
}

constexpr ParamDef degrees(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Degrees);
}

constexpr ParamDef octaves(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Octaves);
}

constexpr ParamDef bpm(uint32_t id, const char* name, double min, double max, double def) {
    return continuous(id, name, min, max, def, Unit::Bpm);
}

// Left to right, -1 to +1; shown as "50L", "C", "50R".
constexpr ParamDef pan(uint32_t id, const char* name, double def = 0.0) {
    return continuous(id, name, -1.0, 1.0, def, Unit::Pan);
}

constexpr ParamDef integer(uint32_t id, const char* name, int min, int max, int def, Unit unit = Unit::None) {
    ParamDef p = continuous(id, name, min, max, def, unit);
    p.kind = ParamKind::Integer;
    return p;
}

constexpr ParamDef samples(uint32_t id, const char* name, int min, int max, int def) {
    return integer(id, name, min, max, def, Unit::Samples);
}

// A MIDI note number, shown by name ("C4" is 60).
constexpr ParamDef midiNote(uint32_t id, const char* name, int def = 60, int min = 0, int max = 127) {
    return integer(id, name, min, max, def, Unit::Note);
}

template <size_t N>
constexpr ParamDef choice(uint32_t id, const char* name, const char* const (&labels)[N], int def) {
    static_assert(N >= 2, "a choice needs at least two options");
    ParamDef p = continuous(id, name, 0.0, static_cast<double>(N - 1), def);
    p.kind = ParamKind::Choice;
    p.labels = labels;
    p.labelCount = static_cast<int>(N);
    return p;
}

constexpr ParamDef toggle(uint32_t id, const char* name, bool def,
                          const char* const (&labels)[2] = kOffOnLabels) {
    ParamDef p = continuous(id, name, 0.0, 1.0, def ? 1.0 : 0.0);
    p.kind = ParamKind::Toggle;
    p.labels = labels;
    p.labelCount = 2;
    return p;
}

// A tempo-synced length, one of kNoteValueLabels, the default given by its
// label: noteValue(kTime, "Time", "1/8."). ParamValues::beats() reads it.
constexpr ParamDef noteValue(uint32_t id, const char* name, const char* def) {
    return choice(id, name, kNoteValueLabels, detail::noteValueIndex(def));
}

// A noteValue() parameter's plain value for a label, for presets:
// {kTime, noteValueOf("1/4")}. -1 (rejected by the preset checks) if none.
constexpr double noteValueOf(const char* label) { return detail::noteValueIndex(label); }

}  // namespace supple
