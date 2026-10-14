// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Orrery patches (spec: docs/specs/synth.md §3, §5–§7).
//
// A patch is *structure* (which slots exist and their types, audio routing,
// modulation routes, step values, FX order, macro names, quality) plus
// *values* (one physical value per parameter in orrery::table(), indexed like
// the table). Values of absent slots are kept at their defaults and are not
// written.
//
// The document is nested by block ("oscillators": {"osc1": {...}}); the codec
// validates it as a whole with JSON Pointer paths and concrete hints. Main
// thread only.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "orrery/params.h"
#include "supple/base/result.h"
#include "supple/json/json.h"
#include "supple/patch/patch.h"

namespace supple::orrery {

// --- modulation --------------------------------------------------------------------------

enum class Source : std::uint8_t {
    Amp,
    Env2,
    Env3,
    Env4,
    Lfo1,
    Lfo2,
    Lfo3,
    Lfo4,
    Step1,
    Step2,
    Velocity,
    ReleaseVelocity,
    Key,
    Random,
    Gate,
    Pressure,
    Slide,
    Modwheel,
    Expression,
    Breath,
    Pitchbend,
    Aftertouch,
    Macro1,
    Macro2,
    Macro3,
    Macro4,
    Macro5,
    Macro6,
    Macro7,
    Macro8,
    Osc1,
    Osc2,
    Osc3,
    Osc4,
    Count
};
std::string_view sourceName(Source s);
std::optional<Source> findSource(std::string_view name);
bool isAudioSource(Source s);  // osc1..osc4

// Where a route lands: a modulatable parameter, or an oscillator's pitch /
// phase-modulation input / ring-modulation amount.
struct Dest {
    enum class Kind : std::uint8_t { Param, Pitch, Fm, Am };
    Kind kind = Kind::Param;
    std::uint16_t index = 0;  // table index (Param) or oscillator 0..3
    friend bool operator==(const Dest&, const Dest&) = default;
};
std::string destName(const Dest& d);  // "filter1.cutoff", "osc2.pitch"

// The unit a route amount is written in for this destination: oct for Hz
// parameters, st for pitch (st/ct), dB for levels, x for oscN.fm, % for the
// rest (of the parameter's range, applied in the normalized domain).
units::Unit modUnit(const Dest& d);

struct Route {
    Source source = Source::Amp;
    Dest dest;
    double amount = 0.0;        // in modUnit(dest)
    std::optional<Source> via;  // scales the amount (0..1 sources) — e.g. modwheel
    double curve = 0.0;         // % -100..100 response curve on the source
    friend bool operator==(const Route&, const Route&) = default;
};
inline constexpr std::size_t kMaxRoutes = 64;

// --- structure ---------------------------------------------------------------------------

enum class OscTarget : std::uint8_t { Filter1, Filter2, Both, Direct, None };
enum class FilterRouting : std::uint8_t { Serial, Parallel };
enum class DriveType : std::uint8_t { Off, Soft, Tape, Fold, Hard };
enum class Retrigger : std::uint8_t { Voice, Free, Mono };
enum class Quality : std::uint8_t { Eco, Normal, High };
enum class FxKind : std::uint8_t { Chorus, Phaser, Delay, Reverb, Eq };
std::string_view fxName(FxKind k);
int oversampling(Quality q);  // 1, 2, 4

struct Structure {
    Quality quality = Quality::Normal;
    std::array<std::optional<OscType>, kOscCount> osc{};
    std::array<OscTarget, kOscCount> to{};
    FilterRouting routing = FilterRouting::Serial;
    std::array<std::optional<FilterType>, kFilterCount> filter{};
    DriveType drive = DriveType::Off;
    std::array<bool, kEnvCount> env{true, false, false, false};  // [0] amp: always
    std::array<bool, kLfoCount> lfo{};
    std::array<Retrigger, kLfoCount> lfoRetrigger{};
    std::array<bool, kStepCount> step{};
    std::array<Retrigger, kStepCount> stepRetrigger{};
    std::array<std::vector<double>, kStepCount> stepValues{};  // % -100..100, ≤ 16
    std::array<std::string, kMacroCount> macroName{};          // empty = "Macro N"
    std::vector<FxKind> chain;
    std::vector<Route> routes;  // canonical order: source, then destination

    bool has(Slot s) const;  // does the slot exist in this patch?
    friend bool operator==(const Structure&, const Structure&) = default;
};

// True when the source's value is shared by all voices (global modulators,
// performance controllers, macros, free/mono LFOs and steps).
bool isGlobalSource(Source s, const Structure& st);
// True when the destination is a global parameter (voice.*, fx.*, output.*,
// macros, and the parameters of free/mono LFOs and steps).
bool isGlobalDest(const Dest& d, const Structure& st);

struct Body {
    Structure structure;
    std::vector<double> values;  // physical, indexed like table()
    friend bool operator==(const Body&, const Body&) = default;
};
using Patch = patch::Patch<Body>;

// --- codec -------------------------------------------------------------------------------

class PatchType {
  public:
    PatchType();

    Patch defaults() const;  // an init patch: osc1 saw → amp, no filters
    Result<Patch> fromJson(const json::Value& document,
                           std::vector<patch::Warning>* warnings = nullptr) const;
    Result<Patch> parse(std::string_view text,
                        std::vector<patch::Warning>* warnings = nullptr) const;
    json::Value toJson(const Patch& p, patch::Detail detail = patch::Detail::Full) const;
    std::string serialize(const Patch& p, patch::Detail detail = patch::Detail::Full) const;
    // RFC 7396 merge onto the patch's minimal form, then full validation. A
    // merge that changes a slot's type drops the old type's fields (and values
    // the new type cannot take); one that removes an FX from fx.chain drops its
    // block. A flat "params" member ({"filter1.cutoff": 800}) sets values by
    // parameter key (the semantic-params flat convention); its keys must
    // belong to existing slots.
    Result<Patch> merge(const Patch& current, const json::Value& mergePatch) const;
    json::Value diff(const Patch& a, const Patch& b) const;
    json::Value jsonSchema() const;
    const std::string& schemaId() const { return codec_.schemaId(); }
    const patch::Codec& codec() const { return codec_; }

  private:
    patch::Codec codec_;
};

// The value a field takes when a slot omits it (per-type for oscillator waves).
double defaultValue(const Structure& st, std::size_t index);
// Whether the parameter belongs to a slot that exists and has this field for
// its type (inactive parameters are ignored by the DSP and not written).
bool paramActive(const Structure& st, std::size_t index);
// The parameter's location in the nested patch document
// ({"oscillators", "osc1", "level"}).
std::vector<std::string> patchPath(std::size_t index);
// ≤ 600 characters (synth.md §7). `changedOnly` skips default-valued details.
std::string describe(const Patch& p, bool changedOnly = true);

}  // namespace supple::orrery
