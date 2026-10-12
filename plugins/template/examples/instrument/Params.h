// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The parameter table: the only description of this product's parameters.
// IDs are session ABI: append, never reorder, renumber or reuse.
#pragma once

#include <cstdint>

#include "core/Param.h"

namespace product {

enum ParamId : uint32_t {
    kWaveform = 0,
    kAttack = 1,
    kRelease = 2,
    kLevel = 3,
    kBendRange = 4,
    kDecay = 5,
    kSustain = 6,
    kMode = 7,
    kGlide = 8,
};

inline constexpr const char* kWaveformLabels[] = {"Sine", "Triangle", "Saw"};
inline constexpr const char* kModeLabels[] = {"Poly", "Mono", "Legato"};

inline constexpr supple::ParamDef kParams[] = {
    supple::choice(kWaveform, "Waveform", kWaveformLabels, 0)
        .help("Sine is pure; the triangle adds odd harmonics, the saw every harmonic."),
    supple::milliseconds(kAttack, "Attack", 1.0, 2000.0, 5.0).modulatable()
        .help("How long a note takes to rise to full level."),
    supple::milliseconds(kRelease, "Release", 5.0, 5000.0, 250.0).modulatable()
        .help("How long a released note takes to fall silent from full level."),
    // A host can modulate each voice's level apart (CLAP polyphonic modulation).
    supple::decibels(kLevel, "Level", -48.0, 6.0, -6.0).polyModulatable().help("The level of every voice."),
    // How far a full MIDI pitch bend moves every note on its channel.
    supple::integer(kBendRange, "Bend Range", 0, 24, 2).unitText("st")
        .help("How far a full MIDI pitch bend moves the notes on its channel, in semitones."),
    supple::milliseconds(kDecay, "Decay", 1.0, 5000.0, 200.0).modulatable()
        .help("Decay time over the full level range; a higher sustain level is reached sooner."),
    supple::percent(kSustain, "Sustain", 100.0).modulatable().help("The level a held note settles at."),
    supple::choice(kMode, "Mode", kModeLabels, 0)
        .help("Poly plays chords. Mono plays one note, the newest held; Legato too, without restarting the "
              "envelope when one note runs into the next."),
    supple::milliseconds(kGlide, "Glide", 0.0, 1000.0, 0.0)
        .help("Each note slides from the last one's pitch over this long."),
};

}  // namespace product
