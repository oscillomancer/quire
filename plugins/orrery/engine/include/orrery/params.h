// Orrery's parameter table (spec: docs/specs/synth.md §4, §8).
//
// Every non-structure field of every slot — existing in the patch or not — is
// a core-params parameter with a stable CLAP id. The table is built once from
// per-block field lists: ids are <slot base> + <field offset>, and offsets are
// append-only within a block, so ids never move.
//
// Table order is slot order, then field order, so the audio thread finds a
// value with index(slot, field) — no lookups.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

#include "supple/params/params.h"

namespace supple::orrery {

// --- slots -------------------------------------------------------------------------

enum class Block : std::uint8_t {
    Voice,
    Osc,
    Filters,
    Filter,
    Drive,
    Amp,
    Env,
    Lfo,
    Step,
    Macro,
    Chorus,
    Phaser,
    Delay,
    Reverb,
    Eq,
    Output,
};

enum Slot : std::uint8_t {
    kVoice,
    kOsc1,
    kOsc2,
    kOsc3,
    kOsc4,
    kFilters,
    kFilter1,
    kFilter2,
    kDrive,
    kAmp,
    kEnvAmp,
    kEnv2,
    kEnv3,
    kEnv4,
    kLfo1,
    kLfo2,
    kLfo3,
    kLfo4,
    kStep1,
    kStep2,
    kMacro1,
    kMacro2,
    kMacro3,
    kMacro4,
    kMacro5,
    kMacro6,
    kMacro7,
    kMacro8,
    kChorus,
    kPhaser,
    kDelay,
    kReverb,
    kEq,
    kOutput,
    kSlotCount
};

inline constexpr int kOscCount = 4, kFilterCount = 2, kEnvCount = 4, kLfoCount = 4, kStepCount = 2,
                     kMacroCount = 8, kFxCount = 5, kMaxSteps = 16;

struct SlotInfo {
    Block block;
    std::string_view name;   // key prefix and patch slot name: "osc1", "amp" (the envelope)
    std::string_view group;  // CLAP group: "Oscillator 1"
    std::uint32_t baseId;
};
const SlotInfo& slotInfo(Slot s);
constexpr Slot osc(int i) { return static_cast<Slot>(kOsc1 + i); }        // i = 0..3
constexpr Slot filter(int i) { return static_cast<Slot>(kFilter1 + i); }  // 0..1
constexpr Slot env(int i) { return static_cast<Slot>(kEnvAmp + i); }      // 0 = amp
constexpr Slot lfo(int i) { return static_cast<Slot>(kLfo1 + i); }
constexpr Slot step(int i) { return static_cast<Slot>(kStep1 + i); }
constexpr Slot macro(int i) { return static_cast<Slot>(kMacro1 + i); }

// --- fields (offsets within a block; append-only) ------------------------------------

enum class VoiceF : std::uint16_t {
    mode,
    polyphony,
    unison,
    unison_detune,
    unison_spread,
    glide,
    glide_mode,
    bend_range,
    mpe,
    drift,
    count
};
enum class OscF : std::uint16_t {
    level,
    pan,
    coarse,
    fine,
    phase_mode,
    phase,
    wave,
    width,
    shape,
    sync,
    table,
    position,
    ratio,
    ratio_mode,
    fixed_freq,
    feedback,
    color,
    tilt,
    octave,
    count
};
enum class FiltersF : std::uint16_t { balance, count };
enum class FilterF : std::uint16_t {
    cutoff,
    resonance,
    keytrack,
    drive,
    level,
    slope,
    mode,
    feedback,
    damping,
    vowel,
    shift,
    count
};
enum class DriveF : std::uint16_t { amount, bias, mix, count };
enum class AmpF : std::uint16_t { level, velocity, pan, count };
enum class EnvF : std::uint16_t {
    delay,
    attack,
    hold,
    decay,
    sustain,
    release,
    attack_curve,
    decay_curve,
    release_curve,
    retrigger,
    count
};
enum class LfoF : std::uint16_t { wave, rate, sync, phase, fade, polarity, count };
enum class StepF : std::uint16_t { length, sync, glide, count };
enum class MacroF : std::uint16_t { value, count };
enum class ChorusF : std::uint16_t { rate, depth, voices, mix, count };
enum class PhaserF : std::uint16_t { rate, depth, stages, feedback, mix, count };
enum class DelayF : std::uint16_t { sync, time, feedback, ping_pong, damping, mix, count };
enum class ReverbF : std::uint16_t { size, decay, predelay, damping, width, mix, count };
enum class EqF : std::uint16_t { low_gain, mid_freq, mid_gain, mid_q, high_gain, count };
enum class OutputF : std::uint16_t { level, count };

// --- enum choices (indices are the physical values) ------------------------------------

enum class VoiceMode : std::uint8_t { Poly, Mono, Legato };
enum class GlideMode : std::uint8_t { Always, Legato };
enum class Wave : std::uint8_t {
    Saw,
    Square,
    Pulse,
    Triangle,
    Sine,
    HalfSine,
    AbsSine,
    SquareSine
};
enum class PhaseMode : std::uint8_t { Free, Reset };
enum class Table : std::uint8_t { HarmonicRamp, Pwm, Formant, Bell, Organ, Digital, SpectralComb };
enum class RatioMode : std::uint8_t { Harmonic, Free, Fixed };
enum class NoiseColor : std::uint8_t { White, Pink, Brown, Blue };
enum class Slope : std::uint8_t { Db12, Db24 };
enum class SvfMode : std::uint8_t { Lowpass, Bandpass, Highpass, Notch, Peak, Allpass };
enum class EnvRetrigger : std::uint8_t { Always, Legato };
enum class LfoWave : std::uint8_t {
    Sine,
    Triangle,
    SawUp,
    SawDown,
    Square,
    SampleHold,
    SmoothRandom
};
enum class Polarity : std::uint8_t { Bipolar, Unipolar };
// Note values; index 0 of a sync field that allows it is "off".
enum class NoteValue : std::uint8_t {
    Off,
    N8_1,
    N4_1,
    N2_1,
    N1_1,
    N1_2,
    N1_2d,
    N1_2t,
    N1_4,
    N1_4d,
    N1_4t,
    N1_8,
    N1_8d,
    N1_8t,
    N1_16,
    N1_16d,
    N1_16t,
    N1_32
};
// Length of a note value in quarter notes (Off → 0).
double noteQuarters(NoteValue v);

// --- structure-dependent applicability -----------------------------------------------

enum class OscType : std::uint8_t { Va, Wavetable, Fm, Noise, Sub };
enum class FilterType : std::uint8_t { Ladder, Svf, Comb, Formant };
inline constexpr int kOscTypeCount = 5, kFilterTypeCount = 4;
std::string_view oscTypeName(OscType t);
std::string_view filterTypeName(FilterType t);

// The fields an oscillator / filter of this type has in the patch (others are
// errors there). Every osc/filter field applies to at least one type.
bool oscFieldApplies(OscType t, OscF f);
bool filterFieldApplies(FilterType t, FilterF f);
// Wave choices valid for the type (va: saw..sine; fm: sine variants; sub:
// sine/triangle/square), and the type's default wave.
bool waveApplies(OscType t, Wave w);
Wave defaultWave(OscType t);

// --- the table ---------------------------------------------------------------------------

params::Table table();
std::size_t paramCount();

// Table index of (slot, field offset). The slot's block must own the field.
std::size_t index(Slot s, std::uint16_t field);
template <class F>
std::size_t index(Slot s, F field) {
    return index(s, static_cast<std::uint16_t>(field));
}

struct ParamLocation {
    Slot slot;
    std::uint16_t field;
};
ParamLocation locate(std::size_t index);
std::uint16_t fieldCount(Block b);
// The field's patch name ("cutoff") — the key without the slot prefix.
std::string_view fieldName(Block b, std::uint16_t field);
// The field's display label without the slot prefix ("Level"; the CLAP name
// is "Osc 1 Level").
std::string_view fieldLabel(Block b, std::uint16_t field);
std::optional<std::uint16_t> findField(Block b, std::string_view name);
// True for fields written with their choice label in patches ("24 dB", "1/8.").
bool writesLabel(std::size_t index);

}  // namespace supple::orrery
