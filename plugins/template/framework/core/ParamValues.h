// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The parameter values a processor reads, in plain units, plus which of them
// changed since the last update(). Indexed by parameter ID:
//
//   void Processor::update(const supple::ParamValues& v) {
//       if (v.changed(kCutoff)) filter_.setCutoff(v[kCutoff], v.initial());
//       drive_.set(dsp::dbToGain(v[kDrive]), v.initial());
//       mode_ = v.choice(kMode);
//   }
//
// Values already include CLAP modulation and are clamped to range. An output
// parameter's value here means nothing: the processor reports outputs
// (ProcessorBase::setOutput), it does not read them.
#pragma once

#include <cassert>
#include <cmath>
#include <cstdint>

#include "core/ParamTable.h"

namespace supple {

class ParamValues {
  public:
    ParamValues() {
        for (size_t i = 0; i < params::kCount; ++i) {
            plain_[i] = params::at(i).def;
            changed_[i] = true;
        }
    }

    double operator[](uint32_t id) const { return plain_[slot(id)]; }
    int choice(uint32_t id) const { return static_cast<int>(std::lround(plain_[slot(id)])); }
    bool on(uint32_t id) const { return plain_[slot(id)] > 0.5; }
    // A level in decibels as a linear gain: exactly 0 at the minimum of a
    // parameter marked offAtMinimum(), exactly 1 at 0 dB.
    double gain(uint32_t id) const {
        const size_t i = slot(id);
        assert(params::at(i).unit == Unit::Decibels && "gain() reads a decibel parameter");
        return levelGain(params::at(i), plain_[i]);
    }
    // At the minimum of a parameter marked offAtMinimum().
    bool off(uint32_t id) const { return params::at(slot(id)).off(plain_[slot(id)]); }
    // A noteValue() parameter's length in quarter notes.
    double beats(uint32_t id) const {
        assert(params::at(slot(id)).labels == kNoteValueLabels && "beats() reads a noteValue() parameter");
        return noteValueBeats(choice(id));
    }
    // The plain value a per-note modulation amount (NoteModulation, host
    // units) makes of this parameter's current value, clamped to its range.
    double modulated(uint32_t id, double amount) const {
        const size_t i = slot(id);
        return modulatedPlain(params::at(i), plain_[i], amount);
    }
    bool changed(uint32_t id) const { return changed_[slot(id)]; }
    // True for the update that follows prepare() or reset(): jump, don't glide.
    bool initial() const { return initial_; }
    bool anyChanged() const { return any_; }

    // Framework side.
    double atIndex(size_t index) const { return plain_[index]; }
    void setIndex(size_t index, double plain) {
        if (plain_[index] == plain) return;
        plain_[index] = plain;
        changed_[index] = true;
        any_ = true;
    }
    void markAll() {
        for (size_t i = 0; i < params::kCount; ++i) changed_[i] = true;
        any_ = true;
    }
    void clearChanges() {
        for (size_t i = 0; i < params::kCount; ++i) changed_[i] = false;
        any_ = false;
        initial_ = false;
    }
    void setInitial(bool initial) { initial_ = initial; }

  private:
    static size_t slot(uint32_t id) {
        const int i = params::indexOf(id);
        assert(i >= 0 && "ParamValues read with an ID that is not in product::kParams");
        return i >= 0 ? static_cast<size_t>(i) : 0;
    }

    double plain_[params::kCount];
    bool changed_[params::kCount];
    bool any_ = true;
    bool initial_ = true;
};

}  // namespace supple
