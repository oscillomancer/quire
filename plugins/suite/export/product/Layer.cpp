// SPDX-License-Identifier: @LICENSE@
// Copyright (c) @YEAR@ @HOLDER@
//
// The layer (plugin/Layer.h): the suite's script layer, with the
// semantic-params extension and the script's status in the host's log.
#include <memory>

#include "supple/suite/script_layer.h"

std::unique_ptr<supple::plugin::Layer> product::makeLayer(supple::plugin::Instance& instance) {
    return std::make_unique<supple::suite::ScriptLayer>(instance);
}
