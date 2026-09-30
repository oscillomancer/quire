// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The exported clap_entry symbol. Deliberately tiny: clap-wrapper compiles this
// file once per output format (CLAP, VST3, AUv2), each time linking the same
// static implementation library, so every format runs identical DSP.
#include <clap/clap.h>

#include "analyzer_entry.h"

extern "C" {
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wattributes"
#endif

const CLAP_EXPORT struct clap_plugin_entry clap_entry = {
    CLAP_VERSION, analyzer_entry_init, analyzer_entry_deinit, analyzer_entry_get_factory};

#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif
}
