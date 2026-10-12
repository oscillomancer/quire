// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Microtuning, for a product that switched it on (PRODUCT_TUNING ON in
// product.cmake). The processor asks ProcessorBase::retuning(key, channel,
// frame) how far a key sits from twelve-tone equal temperament at A4 = 440 Hz,
// in semitones, and adds that to its pitch:
//
//   const double pitch = key - 69 + retuning(key, channel) + bend + expression;
//   const double hz = 440.0 * std::exp2(pitch / 12.0);
//
// Ask while a note sounds, not only when it starts: a tuning may move (an
// MTS-ESP master retuning live). Once per block per voice is plenty.
//
// The plugin supplies the source: an MTS-ESP master when one is running,
// else the host's CLAP tuning for the note's channel, else none (0). A note
// on a key the tuning leaves unmapped never reaches the processor; the
// framework retires it at once.
#pragma once

#include <cstdint>

namespace supple {

class TuningSource {
  public:
    virtual ~TuningSource() = default;
    // Audio thread. Semitones from equal temperament for this key and MIDI
    // channel (-1 when unknown), at `frame` of the host's block.
    virtual double retuning(int key, int channel, uint32_t frame) = 0;
    // Audio thread. False for a key the tuning leaves unmapped.
    virtual bool plays(int key, int channel) = 0;
    // Audio thread, at the event's frame: the host chose tuning `id` for a
    // port and channel (-1: every one). CLAP tuning events.
    virtual void select(int port, int channel, uint32_t id) { (void)port, (void)channel, (void)id; }
};

}  // namespace supple
