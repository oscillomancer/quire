// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The layer of a plugin made from a script (plugin/Layer.h): the
// semantic-params extension (semantic_layer.h), the code the processor let
// go of freed on the main thread, and what the script's status says in the
// host's log -- once per occurrence, with the line it was on: a loop that
// spent its budget and was stopped, an index outside an array. The player's
// layer derives from it to follow the script's file. Main thread only.
#pragma once

#include <cstdint>

#include "supple/suite/script_definition.h"
#include "supple/suite/script_processor.h"
#include "supple/suite/semantic_layer.h"

namespace supple::suite {

class ScriptLayer : public SemanticLayer {
  public:
    explicit ScriptLayer(plugin::Instance& instance) : SemanticLayer(instance) {}

    void tick() override;
    void redefined() override;

  protected:
    // A file member's file decodes as audio.
    bool fileReadable(size_t index, const std::string& path, std::string& why) const override;
    const ScriptDefinition& definition();
    ScriptProcessor& processor();
    // The status is for code now running: what was reported is forgotten.
    void statusRestarted() { budgetReported_ = outOfRangeReported_ = 0; }

  private:
    void reportStatus();

    uint32_t budgetReported_ = 0, outOfRangeReported_ = 0;
};

}  // namespace supple::suite
