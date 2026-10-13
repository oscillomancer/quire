// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Current plugin problems (docs/specs/plugin-status-extension.md).
#pragma once

#include <clap/clap.h>

static const char CLAP_EXT_SUPPLE_PLUGIN_STATUS[] = "com.suppleaudio.plugin-status/1";

typedef struct clap_plugin_supple_plugin_status {
    uint32_t struct_size;
    // [main-thread] Writes supple.plugin-status/1.0 JSON. Non-destructive;
    // false on stream failure. Problems are observations, never patch state.
    bool(CLAP_ABI* get_status)(const clap_plugin_t* plugin, const clap_ostream_t* out);
} clap_plugin_supple_plugin_status_t;
