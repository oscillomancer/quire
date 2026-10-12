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
#include <algorithm>
#include <cstdint>
#include <memory>

#include "core/Definition.h"

namespace supple {

class ParamValues {
  public:
    // Every parameter of `table` at its default. The table outlives this.
    explicit ParamValues(const ParamTable& table)
        : table_(&table),
          count_(table.count()),
          plain_(std::make_unique<double[]>(count_ > 0 ? count_ : 1)),
          changed_(std::make_unique<bool[]>(count_ > 0 ? count_ : 1)) {
        for (size_t i = 0; i < count_; ++i) {
            plain_[i] = table.at(i).def;
            changed_[i] = true;
        }
    }
    // A product with tables: its own.
    ParamValues() : ParamValues(productDefinition()->params()) {}
    ParamValues(const ParamValues& o) : ParamValues(*o.table_) { *this = o; }
    ParamValues& operator=(const ParamValues& o) {
        if (this == &o) return *this;
        if (o.count_ != count_) {
            plain_ = std::make_unique<double[]>(o.count_ > 0 ? o.count_ : 1);
            changed_ = std::make_unique<bool[]>(o.count_ > 0 ? o.count_ : 1);
        }
        table_ = o.table_;
        count_ = o.count_;
        std::copy(o.plain_.get(), o.plain_.get() + count_, plain_.get());
        std::copy(o.changed_.get(), o.changed_.get() + count_, changed_.get());
        any_ = o.any_;
        initial_ = o.initial_;
        return *this;
    }

    const ParamTable& table() const { return *table_; }

    double operator[](uint32_t id) const { return plain_[slot(id)]; }
    int choice(uint32_t id) const { return static_cast<int>(std::lround(plain_[slot(id)])); }
    bool on(uint32_t id) const { return plain_[slot(id)] > 0.5; }
    // A level in decibels as a linear gain: exactly 0 at the minimum of a
    // parameter marked offAtMinimum(), exactly 1 at 0 dB.
    double gain(uint32_t id) const {
        const size_t i = slot(id);
        assert(table_->at(i).unit == Unit::Decibels && "gain() reads a decibel parameter");
        return levelGain(table_->at(i), plain_[i]);
    }
    // At the minimum of a parameter marked offAtMinimum().
    bool off(uint32_t id) const { return table_->at(slot(id)).off(plain_[slot(id)]); }
    // A noteValue() parameter's length in quarter notes.
    double beats(uint32_t id) const {
        assert(table_->at(slot(id)).labels == kNoteValueLabels && "beats() reads a noteValue() parameter");
        return noteValueBeats(choice(id));
    }
    // The plain value a per-note modulation amount (NoteModulation, host
    // units) makes of this parameter's current value, clamped to its range.
    double modulated(uint32_t id, double amount) const {
        const size_t i = slot(id);
        return modulatedPlain(table_->at(i), plain_[i], amount);
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
        for (size_t i = 0; i < count_; ++i) changed_[i] = true;
        any_ = true;
    }
    void clearChanges() {
        for (size_t i = 0; i < count_; ++i) changed_[i] = false;
        any_ = false;
        initial_ = false;
    }
    void setInitial(bool initial) { initial_ = initial; }

  private:
    size_t slot(uint32_t id) const {
        const int i = table_->indexOf(id);
        assert(i >= 0 && "ParamValues read with an ID that is not in the parameter table");
        return i >= 0 ? static_cast<size_t>(i) : 0;
    }

    const ParamTable* table_;
    size_t count_;
    std::unique_ptr<double[]> plain_;
    std::unique_ptr<bool[]> changed_;
    bool any_ = true;
    bool initial_ = true;
};

}  // namespace supple
