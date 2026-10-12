// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The exported clap_entry symbol, and nothing else. clap-wrapper compiles this
// file once per output format, each time linking the same implementation
// library, so every format runs the same code.
#include <clap/clap.h>

#include "plugin/Entry.h"

extern "C" {
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif

CLAP_EXPORT extern const clap_plugin_entry_t clap_entry;
const clap_plugin_entry_t clap_entry = {CLAP_VERSION_INIT, supple_entry_init, supple_entry_deinit,
                                        supple_entry_get_factory};

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}
