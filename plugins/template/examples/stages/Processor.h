// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The product's DSP: every stage's gain and the Output level, multiplied,
// glided over 20 ms. What it shows is not the sound but where the stages
// come from: a run-time product's processor is made from the Definition its
// instance runs (Catalog.h), and reads its parameters by ID as any other.
#pragma once

#include <cstddef>
#include <vector>

#include "Catalog.h"
#include "core/Dsp.h"
#include "core/ParamValues.h"
#include "core/ProcessorBase.h"

namespace product {

class Processor : public supple::ProcessorBase<Processor> {
  public:
    explicit Processor(const supple::DefinitionPtr& definition)
        : stages_(static_cast<const StagesDefinition&>(*definition).stages().size()) {}

    size_t stages() const { return stages_; }

    void prepare(const supple::ProcessSetup& setup, const supple::ParamValues&) {
        gain_.setTime(20.0, setup.sampleRate);
    }

    void update(const supple::ParamValues& v) {
        double g = v.gain(kOutput);
        for (size_t k = 0; k < stages_; ++k) g *= v.gain(kStage0 + static_cast<uint32_t>(k));
        gain_.set(g, v.initial());
    }

    void reset() { gain_.set(gain_.target(), true); }

    void process(supple::Block& block) {
        for (int i = 0; i < block.frames; ++i) {
            const double g = gain_.next();
            for (int c = 0; c < block.channels; ++c) block.data[c][i] *= g;
        }
    }

  private:
    size_t stages_;
    supple::dsp::Smoother gain_;
};

}  // namespace product
