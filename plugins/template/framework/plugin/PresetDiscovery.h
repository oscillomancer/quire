// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// CLAP's preset-discovery factory, for a product with presets: hosts list the
// factory presets and the user's folder in their own browsers, and load one
// through the plugin's preset-load extension.
#pragma once

#include <clap/clap.h>

namespace supple::plugin {
const clap_preset_discovery_factory_t* presetDiscoveryFactory();
}  // namespace supple::plugin
