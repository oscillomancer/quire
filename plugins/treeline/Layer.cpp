// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The layer (plugin/Layer.h): the suite's script layer, with the
// semantic-params extension, the script's status in the host's log, and the
// script's own editor when it draws one (the automatic one otherwise).
#include <memory>

#include "supple/suite/script_layer.h"

std::unique_ptr<supple::plugin::Layer> product::makeLayer(supple::plugin::Instance& instance) {
    return std::make_unique<supple::suite::ScriptLayer>(instance);
}
