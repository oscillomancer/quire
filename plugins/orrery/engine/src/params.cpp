// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "orrery/params.h"

#include <cassert>
#include <deque>
#include <string>
#include <vector>

namespace supple::orrery {

using params::Choice;
using params::Curve;
using params::CurveKind;
using params::ParamSpec;
using params::Type;
using units::Unit;

namespace {

constexpr std::uint32_t kMod = params::kAutomatable | params::kModulatable;
constexpr std::uint32_t kAuto = params::kAutomatable;
constexpr std::uint32_t kLevel = kMod | params::kOffAtMin;

constexpr Curve kLog{CurveKind::Log};
constexpr Curve kPow3{CurveKind::Pow, 3.0};

constexpr Choice kVoiceModes[] = {{"poly", "Poly"}, {"mono", "Mono"}, {"legato", "Legato"}};
constexpr Choice kGlideModes[] = {{"always", "Always"}, {"legato", "Legato"}};
constexpr Choice kWaves[] = {{"saw", "Saw"},           {"square", "Square"},
                             {"pulse", "Pulse"},       {"triangle", "Triangle"},
                             {"sine", "Sine"},         {"half_sine", "Half sine"},
                             {"abs_sine", "Abs sine"}, {"square_sine", "Square sine"}};
constexpr Choice kPhaseModes[] = {{"free", "Free"}, {"reset", "Reset"}};
constexpr Choice kTables[] = {{"harmonic_ramp", "Harmonic ramp"},
                              {"pwm", "PWM"},
                              {"formant", "Formant"},
                              {"bell", "Bell"},
                              {"organ", "Organ"},
                              {"digital", "Digital"},
                              {"spectral_comb", "Spectral comb"}};
constexpr Choice kRatioModes[] = {{"harmonic", "Harmonic"}, {"free", "Free"}, {"fixed", "Fixed"}};
constexpr Choice kColors[] = {
    {"white", "White"}, {"pink", "Pink"}, {"brown", "Brown"}, {"blue", "Blue"}};
constexpr Choice kSlopes[] = {{"db12", "12 dB"}, {"db24", "24 dB"}};
constexpr Choice kSvfModes[] = {{"lowpass", "Low-pass"},   {"bandpass", "Band-pass"},
                                {"highpass", "High-pass"}, {"notch", "Notch"},
                                {"peak", "Peak"},          {"allpass", "All-pass"}};
constexpr Choice kEnvRetrigger[] = {{"always", "Always"}, {"legato", "Legato"}};
constexpr Choice kLfoWaves[] = {{"sine", "Sine"},
                                {"triangle", "Triangle"},
                                {"saw_up", "Saw up"},
                                {"saw_down", "Saw down"},
                                {"square", "Square"},
                                {"sample_hold", "Sample & hold"},
                                {"smooth_random", "Smooth random"}};
constexpr Choice kPolarities[] = {{"bipolar", "Bipolar"}, {"unipolar", "Unipolar"}};
constexpr Choice kNotes[] = {{"off", "off"},      {"n8_1", "8/1"},     {"n4_1", "4/1"},
                             {"n2_1", "2/1"},     {"n1_1", "1/1"},     {"n1_2", "1/2"},
                             {"n1_2d", "1/2."},   {"n1_2t", "1/2t"},   {"n1_4", "1/4"},
                             {"n1_4d", "1/4."},   {"n1_4t", "1/4t"},   {"n1_8", "1/8"},
                             {"n1_8d", "1/8."},   {"n1_8t", "1/8t"},   {"n1_16", "1/16"},
                             {"n1_16d", "1/16."}, {"n1_16t", "1/16t"}, {"n1_32", "1/32"}};
// Step sequencers always run synced: the same list without "off".
constexpr std::span<const Choice> kNotesNoOff{kNotes + 1, std::size(kNotes) - 1};

constexpr double idx(auto e) { return static_cast<double>(e); }

struct FieldDef {
    std::string_view name, label;
    Type type = Type::Float;
    Unit unit = Unit::none;
    double min = 0.0, max = 1.0, def = 0.0;
    Curve curve{};
    int digits = 3;
    std::span<const Choice> choices{};
    std::uint32_t flags = kMod;
    double smoothingMs = 0.0;
    std::string_view description;
    bool writeLabel = false;
};

FieldDef percent(std::string_view name, std::string_view label, double min, double max, double def,
                 std::string_view description) {
    return {.name = name,
            .label = label,
            .unit = Unit::percent,
            .min = min,
            .max = max,
            .def = def,
            .smoothingMs = 20.0,
            .description = description};
}
FieldDef choice(std::string_view name, std::string_view label, std::span<const Choice> choices,
                double def, std::string_view description, bool writeLabel = false) {
    return {.name = name,
            .label = label,
            .type = Type::Enum,
            .min = 0.0,
            .max = static_cast<double>(choices.size() - 1),
            .def = def,
            .choices = choices,
            .flags = kAuto,
            .description = description,
            .writeLabel = writeLabel};
}
FieldDef integer(std::string_view name, std::string_view label, Unit unit, double min, double max,
                 double def, std::string_view description) {
    return {.name = name,
            .label = label,
            .type = Type::Int,
            .unit = unit,
            .min = min,
            .max = max,
            .def = def,
            .flags = kAuto,
            .description = description};
}
FieldDef boolean(std::string_view name, std::string_view label, std::string_view description) {
    return {.name = name,
            .label = label,
            .type = Type::Bool,
            .max = 1.0,
            .flags = kAuto,
            .description = description};
}
FieldDef level(std::string_view name, std::string_view label, double max, double def,
               std::string_view description) {
    return {.name = name,
            .label = label,
            .unit = Unit::dB,
            .min = -60.0,
            .max = max,
            .def = def,
            .flags = kLevel,
            .smoothingMs = 20.0,
            .description = description};
}
FieldDef timeMs(std::string_view name, std::string_view label, double min, double max, double def,
                std::string_view description) {
    return {.name = name,
            .label = label,
            .unit = Unit::ms,
            .min = min,
            .max = max,
            .def = def,
            .curve = kPow3,
            .description = description};
}

std::vector<FieldDef> voiceFields() {
    return {
        choice("mode", "Mode", kVoiceModes, 0,
               "poly: every note gets its own voices; mono: one note at a time, envelopes "
               "restart; legato: one note, envelopes continue while keys overlap."),
        integer("polyphony", "Polyphony", Unit::none, 1, 32, 8,
                "Notes that can sound at once; each uses `unison` voices (64 voices at most)."),
        integer("unison", "Unison", Unit::none, 1, 8, 1, "Voices per note, detuned and spread."),
        {.name = "unison_detune",
         .label = "Unison detune",
         .unit = Unit::ct,
         .min = 0.0,
         .max = 100.0,
         .def = 10.0,
         .smoothingMs = 20.0,
         .description = "Detune of the outermost unison voices (the others are spread evenly)."},
        percent("unison_spread", "Unison spread", 0, 100, 50,
                "Stereo spread of the unison voices."),
        timeMs("glide", "Glide", 0, 5000, 0, "Portamento time from the previous note's pitch."),
        choice("glide_mode", "Glide mode", kGlideModes, 1,
               "always: every note glides; legato: only notes played while another is held."),
        {.name = "bend_range",
         .label = "Bend range",
         .type = Type::Int,
         .unit = Unit::st,
         .min = 0.0,
         .max = 48.0,
         .def = 2.0,
         .flags = kAuto,
         .description = "Pitch-bend range (MPE per-note bend is always 48 st)."},
        boolean("mpe", "MPE", "Per-note pitch bend, pressure and slide (MPE)."),
        percent("drift", "Drift", 0, 100, 0,
                "Slow, seeded per-voice pitch and cutoff drift (±5 ct / ±3 % at 100 %)."),
    };
}

std::vector<FieldDef> oscFields() {
    return {
        level("level", "Level", 6.0, -6.0, "Oscillator level; -60 dB is off."),
        percent("pan", "Pan", -100, 100, 0, "Stereo position (-100 left, +100 right)."),
        integer("coarse", "Coarse", Unit::st, -48, 48, 0, "Transpose in semitones."),
        {.name = "fine",
         .label = "Fine",
         .unit = Unit::ct,
         .min = -100.0,
         .max = 100.0,
         .def = 0.0,
         .smoothingMs = 20.0,
         .description = "Fine tune in cents."},
        choice("phase_mode", "Phase mode", kPhaseModes, 0,
               "free: free-running with a seeded start; reset: restart at `phase` on each note."),
        {.name = "phase",
         .label = "Phase",
         .unit = Unit::deg,
         .min = 0.0,
         .max = 360.0,
         .def = 0.0,
         .flags = kAuto | params::kPeriodic,
         .description = "Start phase when `phase_mode` is reset."},
        choice("wave", "Wave", kWaves, idx(Wave::Saw),
               "va: saw, square, pulse, triangle, sine; fm: sine, half_sine, abs_sine, "
               "square_sine; sub: sine, triangle, square."),
        percent("width", "Pulse width", 1, 99, 50, "Pulse width of the va `pulse` wave."),
        percent("shape", "Shape", 0, 100, 0, "va: morph toward the next wave in the list."),
        boolean("sync", "Sync", "va: hard-sync to osc1 (not available on osc1)."),
        choice("table", "Table", kTables, 0,
               "Wavetable (original, generated): harmonic_ramp, pwm, formant, bell, organ, "
               "digital, spectral_comb."),
        percent("position", "Position", 0, 100, 0, "Position within the wavetable."),
        {.name = "ratio",
         .label = "Ratio",
         .unit = Unit::x,
         .min = 0.25,
         .max = 32.0,
         .def = 1.0,
         .curve = kLog,
         .smoothingMs = 20.0,
         .description = "fm: frequency ratio to the note (stepped to 0.25 in harmonic mode)."},
        choice("ratio_mode", "Ratio mode", kRatioModes, 0,
               "fm: harmonic (ratio in steps of 0.25), free (any ratio) or fixed (fixed_freq)."),
        {.name = "fixed_freq",
         .label = "Fixed frequency",
         .unit = Unit::Hz,
         .min = 1.0,
         .max = 20000.0,
         .def = 440.0,
         .curve = kLog,
         .smoothingMs = 20.0,
         .description = "fm: frequency when ratio_mode is fixed."},
        percent("feedback", "Feedback", 0, 100, 0, "fm: operator self-feedback."),
        choice("color", "Color", kColors, 0, "noise: white, pink, brown or blue."),
        {.name = "tilt",
         .label = "Tilt",
         .unit = Unit::dB,
         .min = -6.0,
         .max = 6.0,
         .def = 0.0,
         .smoothingMs = 20.0,
         .description = "noise: spectral tilt in dB per octave."},
        integer("octave", "Octave", Unit::none, -2, -1, -1, "sub: octaves below the note."),
    };
}

std::vector<FieldDef> filtersFields() {
    return {percent("balance", "Balance", -100, 100, 0,
                    "Parallel routing: -100 only filter1, +100 only filter2.")};
}

std::vector<FieldDef> filterFields() {
    return {
        {.name = "cutoff",
         .label = "Cutoff",
         .unit = Unit::Hz,
         .min = 20.0,
         .max = 20000.0,
         .def = 1000.0,
         .curve = kLog,
         .smoothingMs = 20.0,
         .description = "Cutoff frequency (comb: comb frequency)."},
        percent("resonance", "Resonance", 0, 100, 0,
                "Resonance; ladder and svf self-oscillate near 100 %."),
        percent("keytrack", "Key tracking", 0, 200, 0,
                "100 %: cutoff follows the note 1:1 (reference C4)."),
        {.name = "drive",
         .label = "Drive",
         .unit = Unit::dB,
         .min = 0.0,
         .max = 24.0,
         .def = 0.0,
         .smoothingMs = 20.0,
         .description = "Input gain into the filter's nonlinearity."},
        level("level", "Level", 12.0, 0.0, "Filter output level."),
        choice("slope", "Slope", kSlopes, idx(Slope::Db24), "ladder: 12 dB or 24 dB per octave.",
               true),
        choice("mode", "Mode", kSvfModes, 0,
               "svf: lowpass, bandpass, highpass, notch, peak or allpass."),
        percent("feedback", "Feedback", -99, 99, 50, "comb: feedback; negative = odd harmonics."),
        percent("damping", "Damping", 0, 100, 20, "comb: high-frequency damping in the loop."),
        {.name = "vowel",
         .label = "Vowel",
         .min = 0.0,
         .max = 4.0,
         .def = 0.0,
         .smoothingMs = 20.0,
         .description = "formant: continuous vowel, 0 a, 1 e, 2 i, 3 o, 4 u."},
        {.name = "shift",
         .label = "Shift",
         .unit = Unit::st,
         .min = -12.0,
         .max = 12.0,
         .def = 0.0,
         .smoothingMs = 20.0,
         .description = "formant: formant shift."},
    };
}

std::vector<FieldDef> driveFields() {
    return {
        {.name = "amount",
         .label = "Amount",
         .unit = Unit::dB,
         .min = 0.0,
         .max = 36.0,
         .def = 0.0,
         .smoothingMs = 20.0,
         .description = "Drive gain (output is gain-compensated)."},
        percent("bias", "Bias", -100, 100, 0, "Asymmetry (even harmonics)."),
        percent("mix", "Mix", 0, 100, 100, "Dry/wet."),
    };
}

std::vector<FieldDef> ampFields() {
    return {
        level("level", "Level", 12.0, 0.0, "Voice level after the amp envelope."),
        percent("velocity", "Velocity", 0, 100, 50,
                "Velocity sensitivity of the level (100 % = 40 dB range)."),
        percent("pan", "Pan", -100, 100, 0, "Voice pan."),
    };
}

std::vector<FieldDef> envFields() {
    return {
        timeMs("delay", "Delay", 0, 10000, 0, "Time before the attack starts."),
        timeMs("attack", "Attack", 0, 30000, 5, "Attack time."),
        timeMs("hold", "Hold", 0, 10000, 0, "Time at full level after the attack."),
        timeMs("decay", "Decay", 1, 30000, 300, "Decay time to the sustain level."),
        percent("sustain", "Sustain", 0, 100, 70, "Sustain level."),
        timeMs("release", "Release", 1, 30000, 300, "Release time after the key is released."),
        percent("attack_curve", "Attack curve", -100, 100, 0,
                "0 linear, positive exponential-like, negative logarithmic."),
        percent("decay_curve", "Decay curve", -100, 100, 50, "Shape of the decay (as attack)."),
        percent("release_curve", "Release curve", -100, 100, 50,
                "Shape of the release (as attack)."),
        choice("retrigger", "Retrigger", kEnvRetrigger, 0,
               "Mono voice mode: always restarts on every note; legato continues while keys "
               "overlap. (Legato voice mode never restarts on overlapping keys.)"),
    };
}

std::vector<FieldDef> lfoFields() {
    return {
        choice("wave", "Wave", kLfoWaves, 0,
               "sine, triangle, saw_up, saw_down, square, sample_hold, smooth_random."),
        {.name = "rate",
         .label = "Rate",
         .unit = Unit::Hz,
         .min = 0.01,
         .max = 50.0,
         .def = 1.0,
         .curve = kLog,
         .smoothingMs = 20.0,
         .description = "Rate when `sync` is off."},
        choice("sync", "Sync", kNotes, 0,
               "off, or a note value (1/4, 1/8., 1/16t, ...); overrides `rate`.", true),
        {.name = "phase",
         .label = "Phase",
         .unit = Unit::deg,
         .min = 0.0,
         .max = 360.0,
         .def = 0.0,
         .flags = kAuto | params::kPeriodic,
         .description = "Start phase."},
        timeMs("fade", "Fade in", 0, 10000, 0, "Fade-in time after the (re)start."),
        choice("polarity", "Polarity", kPolarities, 0,
               "bipolar (-1..1) or unipolar (0..1) output."),
    };
}

std::vector<FieldDef> stepFields() {
    return {
        integer("length", "Length", Unit::none, 1, 16, 16, "Steps in the sequence."),
        choice("sync", "Rate", kNotesNoOff, idx(NoteValue::N1_16) - 1,
               "Step length as a note value (1/16, 1/8., ...).", true),
        percent("glide", "Glide", 0, 100, 0, "Smoothing between steps."),
    };
}

std::vector<FieldDef> macroFields() {
    return {percent("value", "Value", 0, 100, 0,
                    "Macro value; macros are the automation surface for modulation.")};
}

std::vector<FieldDef> chorusFields() {
    return {
        {.name = "rate",
         .label = "Rate",
         .unit = Unit::Hz,
         .min = 0.05,
         .max = 10.0,
         .def = 0.5,
         .curve = kLog,
         .smoothingMs = 20.0,
         .description = "Modulation rate."},
        percent("depth", "Depth", 0, 100, 30, "Modulation depth."),
        integer("voices", "Voices", Unit::none, 2, 4, 2, "Chorus voices."),
        percent("mix", "Mix", 0, 100, 50, "Dry/wet."),
    };
}

std::vector<FieldDef> phaserFields() {
    return {
        {.name = "rate",
         .label = "Rate",
         .unit = Unit::Hz,
         .min = 0.02,
         .max = 10.0,
         .def = 0.3,
         .curve = kLog,
         .smoothingMs = 20.0,
         .description = "Sweep rate."},
        percent("depth", "Depth", 0, 100, 50, "Sweep depth."),
        integer("stages", "Stages", Unit::none, 2, 12, 6, "All-pass stages (even)."),
        percent("feedback", "Feedback", -90, 90, 30, "Feedback."),
        percent("mix", "Mix", 0, 100, 50, "Dry/wet."),
    };
}

std::vector<FieldDef> delayFields() {
    return {
        choice("sync", "Sync", kNotes, idx(NoteValue::N1_8d),
               "off, or a note value; overrides `time`.", true),
        {.name = "time",
         .label = "Time",
         .unit = Unit::ms,
         .min = 1.0,
         .max = 4000.0,
         .def = 375.0,
         .curve = kLog,
         .smoothingMs = 50.0,
         .description = "Delay time when `sync` is off."},
        percent("feedback", "Feedback", 0, 110, 35, "Feedback (limited for safety above 100 %)."),
        boolean("ping_pong", "Ping-pong", "Alternate the repeats left and right."),
        percent("damping", "Damping", 0, 100, 30, "High-frequency loss per repeat."),
        percent("mix", "Mix", 0, 100, 25, "Dry/wet."),
    };
}

std::vector<FieldDef> reverbFields() {
    return {
        percent("size", "Size", 0, 100, 50, "Room size."),
        {.name = "decay",
         .label = "Decay",
         .unit = Unit::ms,
         .min = 100.0,
         .max = 30000.0,
         .def = 2000.0,
         .curve = kLog,
         .smoothingMs = 50.0,
         .description = "Decay time (RT60)."},
        {.name = "predelay",
         .label = "Pre-delay",
         .unit = Unit::ms,
         .min = 0.0,
         .max = 250.0,
         .def = 10.0,
         .smoothingMs = 50.0,
         .description = "Pre-delay."},
        percent("damping", "Damping", 0, 100, 40, "High-frequency damping."),
        percent("width", "Width", 0, 100, 100, "Stereo width of the tail."),
        percent("mix", "Mix", 0, 100, 20, "Dry/wet."),
    };
}

std::vector<FieldDef> eqFields() {
    const auto gain = [](std::string_view name, std::string_view label,
                         std::string_view description) {
        return FieldDef{.name = name,
                        .label = label,
                        .unit = Unit::dB,
                        .min = -18.0,
                        .max = 18.0,
                        .def = 0.0,
                        .smoothingMs = 20.0,
                        .description = description};
    };
    return {
        gain("low_gain", "Low", "Low shelf at 120 Hz."),
        {.name = "mid_freq",
         .label = "Mid frequency",
         .unit = Unit::Hz,
         .min = 200.0,
         .max = 8000.0,
         .def = 1000.0,
         .curve = kLog,
         .smoothingMs = 20.0,
         .description = "Mid bell frequency."},
        gain("mid_gain", "Mid", "Mid bell gain."),
        {.name = "mid_q",
         .label = "Mid Q",
         .min = 0.3,
         .max = 8.0,
         .def = 0.7,
         .curve = kLog,
         .smoothingMs = 20.0,
         .description = "Mid bell Q."},
        gain("high_gain", "High", "High shelf at 8 kHz."),
    };
}

std::vector<FieldDef> outputFields() {
    return {level("level", "Level", 12.0, 0.0,
                  "Output level (a safety soft clipper follows; it is inaudible below -1 dBFS).")};
}

const std::vector<FieldDef>& fields(Block b) {
    static const std::array<std::vector<FieldDef>, 16> all = {
        voiceFields(), oscFields(),    filtersFields(), filterFields(),
        driveFields(), ampFields(),    envFields(),     lfoFields(),
        stepFields(),  macroFields(),  chorusFields(),  phaserFields(),
        delayFields(), reverbFields(), eqFields(),      outputFields()};
    return all[static_cast<std::size_t>(b)];
}

constexpr std::array<SlotInfo, kSlotCount> kSlots = {{
    {Block::Voice, "voice", "Voice", 0},
    {Block::Osc, "osc1", "Oscillator 1", 100},
    {Block::Osc, "osc2", "Oscillator 2", 200},
    {Block::Osc, "osc3", "Oscillator 3", 300},
    {Block::Osc, "osc4", "Oscillator 4", 400},
    {Block::Filters, "filters", "Filters", 500},
    {Block::Filter, "filter1", "Filter 1", 510},
    {Block::Filter, "filter2", "Filter 2", 610},
    {Block::Drive, "drive", "Drive", 700},
    {Block::Amp, "amp", "Amp", 720},
    {Block::Env, "amp", "Amp envelope", 800},
    {Block::Env, "env2", "Envelope 2", 900},
    {Block::Env, "env3", "Envelope 3", 1000},
    {Block::Env, "env4", "Envelope 4", 1100},
    {Block::Lfo, "lfo1", "LFO 1", 1200},
    {Block::Lfo, "lfo2", "LFO 2", 1300},
    {Block::Lfo, "lfo3", "LFO 3", 1400},
    {Block::Lfo, "lfo4", "LFO 4", 1500},
    {Block::Step, "step1", "Step 1", 1600},
    {Block::Step, "step2", "Step 2", 1700},
    {Block::Macro, "macro1", "Macros", 1800},
    {Block::Macro, "macro2", "Macros", 1801},
    {Block::Macro, "macro3", "Macros", 1802},
    {Block::Macro, "macro4", "Macros", 1803},
    {Block::Macro, "macro5", "Macros", 1804},
    {Block::Macro, "macro6", "Macros", 1805},
    {Block::Macro, "macro7", "Macros", 1806},
    {Block::Macro, "macro8", "Macros", 1807},
    {Block::Chorus, "fx.chorus", "FX/Chorus", 2000},
    {Block::Phaser, "fx.phaser", "FX/Phaser", 2100},
    {Block::Delay, "fx.delay", "FX/Delay", 2200},
    {Block::Reverb, "fx.reverb", "FX/Reverb", 2300},
    {Block::Eq, "fx.eq", "FX/EQ", 2400},
    {Block::Output, "output", "Output", 2500},
}};

// Short display prefix for parameter names ("Osc 1 Level").
std::string displayPrefix(Slot s) {
    const SlotInfo& info = kSlots[s];
    switch (info.block) {
        case Block::Osc:
            return "Osc " + std::to_string(s - kOsc1 + 1);
        case Block::Filter:
            return "Filter " + std::to_string(s - kFilter1 + 1);
        case Block::Env:
            return s == kEnvAmp ? "Amp env" : "Env " + std::to_string(s - kEnvAmp + 1);
        case Block::Lfo:
            return "LFO " + std::to_string(s - kLfo1 + 1);
        case Block::Step:
            return "Step " + std::to_string(s - kStep1 + 1);
        case Block::Macro:
            return "Macro " + std::to_string(s - kMacro1 + 1);
        case Block::Eq:
            return "EQ";
        default: {
            std::string g(info.group);
            if (auto slash = g.find('/'); slash != std::string::npos) g = g.substr(slash + 1);
            return g;
        }
    }
}

struct Built {
    std::vector<ParamSpec> specs;
    std::deque<std::string> strings;  // stable storage for keys and names
    std::array<std::size_t, kSlotCount + 1> start{};
    std::vector<ParamLocation> where;
    std::vector<bool> label;

    std::string_view keep(std::string s) { return strings.emplace_back(std::move(s)); }
};

Built build() {
    Built b;
    for (int s = 0; s < kSlotCount; ++s) {
        const Slot slot = static_cast<Slot>(s);
        const SlotInfo& info = kSlots[slot];
        b.start[slot] = b.specs.size();
        const auto& defs = fields(info.block);
        for (std::size_t f = 0; f < defs.size(); ++f) {
            const FieldDef& d = defs[f];
            ParamSpec p;
            p.id = info.baseId + static_cast<std::uint32_t>(f);
            p.key = info.block == Block::Macro
                        ? info.name
                        : b.keep(std::string(info.name) + "." + std::string(d.name));
            p.name = info.block == Block::Macro
                         ? b.keep(displayPrefix(slot))
                         : b.keep(displayPrefix(slot) + " " + std::string(d.label));
            p.group = info.group;
            p.type = d.type;
            p.unit = d.unit;
            p.min = d.min;
            p.max = d.max;
            p.def = d.def;
            p.curve = d.curve;
            p.digits = d.digits;
            p.choices = d.choices;
            p.description = d.description;
            p.flags = d.flags;
            p.smoothingMs = d.smoothingMs;
            b.specs.push_back(p);
            b.where.push_back({slot, static_cast<std::uint16_t>(f)});
            b.label.push_back(d.writeLabel);
        }
    }
    b.start[kSlotCount] = b.specs.size();
    return b;
}

const Built& built() {
    static const Built b = build();
    return b;
}

// Applicability masks: bit per OscType / FilterType.
constexpr std::uint8_t kVa = 1, kWt = 2, kFm = 4, kNoise = 8, kSub = 16;
constexpr std::array<std::uint8_t, static_cast<std::size_t>(OscF::count)> kOscMask = {
    kVa | kWt | kFm | kNoise | kSub,  // level
    kVa | kWt | kFm | kNoise | kSub,  // pan
    kVa | kWt | kFm,                  // coarse
    kVa | kWt | kFm,                  // fine
    kVa | kWt | kFm | kSub,           // phase_mode
    kVa | kWt | kFm | kSub,           // phase
    kVa | kFm | kSub,                 // wave
    kVa,                              // width
    kVa,                              // shape
    kVa,                              // sync
    kWt,                              // table
    kWt,                              // position
    kFm,                              // ratio
    kFm,                              // ratio_mode
    kFm,                              // fixed_freq
    kFm,                              // feedback
    kNoise,                           // color
    kNoise,                           // tilt
    kSub,                             // octave
};
constexpr std::uint8_t kLadder = 1, kSvf = 2, kComb = 4, kFormant = 8;
constexpr std::array<std::uint8_t, static_cast<std::size_t>(FilterF::count)> kFilterMask = {
    kLadder | kSvf | kComb,             // cutoff
    kLadder | kSvf,                     // resonance
    kLadder | kSvf | kComb | kFormant,  // keytrack
    kLadder | kSvf | kComb | kFormant,  // drive
    kLadder | kSvf | kComb | kFormant,  // level
    kLadder,                            // slope
    kSvf,                               // mode
    kComb,                              // feedback
    kComb,                              // damping
    kFormant,                           // vowel
    kFormant,                           // shift
};

}  // namespace

const SlotInfo& slotInfo(Slot s) { return kSlots[s]; }

double noteQuarters(NoteValue v) {
    switch (v) {
        case NoteValue::Off:
            return 0.0;
        case NoteValue::N8_1:
            return 32.0;
        case NoteValue::N4_1:
            return 16.0;
        case NoteValue::N2_1:
            return 8.0;
        case NoteValue::N1_1:
            return 4.0;
        case NoteValue::N1_2:
            return 2.0;
        case NoteValue::N1_2d:
            return 3.0;
        case NoteValue::N1_2t:
            return 4.0 / 3.0;
        case NoteValue::N1_4:
            return 1.0;
        case NoteValue::N1_4d:
            return 1.5;
        case NoteValue::N1_4t:
            return 2.0 / 3.0;
        case NoteValue::N1_8:
            return 0.5;
        case NoteValue::N1_8d:
            return 0.75;
        case NoteValue::N1_8t:
            return 1.0 / 3.0;
        case NoteValue::N1_16:
            return 0.25;
        case NoteValue::N1_16d:
            return 0.375;
        case NoteValue::N1_16t:
            return 1.0 / 6.0;
        case NoteValue::N1_32:
            return 0.125;
    }
    return 0.0;
}

std::string_view oscTypeName(OscType t) {
    constexpr std::string_view names[] = {"va", "wavetable", "fm", "noise", "sub"};
    return names[static_cast<int>(t)];
}

std::string_view filterTypeName(FilterType t) {
    constexpr std::string_view names[] = {"ladder", "svf", "comb", "formant"};
    return names[static_cast<int>(t)];
}

bool oscFieldApplies(OscType t, OscF f) {
    return (kOscMask[static_cast<std::size_t>(f)] >> static_cast<int>(t)) & 1u;
}

bool filterFieldApplies(FilterType t, FilterF f) {
    return (kFilterMask[static_cast<std::size_t>(f)] >> static_cast<int>(t)) & 1u;
}

bool waveApplies(OscType t, Wave w) {
    switch (t) {
        case OscType::Va:
            return w <= Wave::Sine;
        case OscType::Fm:
            return w >= Wave::Sine;
        case OscType::Sub:
            return w == Wave::Sine || w == Wave::Triangle || w == Wave::Square;
        default:
            return false;
    }
}

Wave defaultWave(OscType t) { return t == OscType::Va ? Wave::Saw : Wave::Sine; }

params::Table table() { return built().specs; }
std::size_t paramCount() { return built().specs.size(); }

std::size_t index(Slot s, std::uint16_t field) {
    const Built& b = built();
    assert(b.start[s] + field < b.start[s + 1]);
    return b.start[s] + field;
}

ParamLocation locate(std::size_t index) { return built().where[index]; }

std::uint16_t fieldCount(Block b) { return static_cast<std::uint16_t>(fields(b).size()); }

std::string_view fieldName(Block b, std::uint16_t field) { return fields(b)[field].name; }
std::string_view fieldLabel(Block b, std::uint16_t field) { return fields(b)[field].label; }

std::optional<std::uint16_t> findField(Block b, std::string_view name) {
    const auto& defs = fields(b);
    for (std::size_t i = 0; i < defs.size(); ++i)
        if (defs[i].name == name) return static_cast<std::uint16_t>(i);
    return std::nullopt;
}

bool writesLabel(std::size_t index) { return built().label[index]; }

}  // namespace supple::orrery
