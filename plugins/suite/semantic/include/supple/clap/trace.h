// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
/*
 * CLAP extension: com.suppleaudio.trace/1
 * Spec: docs/specs/trace-extension.md.
 *
 * Lets a host see values a plugin's own code traces as it runs (a DSP
 * script's trace(name, value)), over time, for whoever is debugging it: a
 * developer, or an agent that cannot attach a debugger. Hosts that do not
 * know it lose nothing.
 *
 * C ABI; depends only on the CLAP headers. Both functions are main-thread,
 * and work only while the plugin is inactive, so the audio thread's writes
 * and the host's reads never meet.
 */
#pragma once

#include <clap/clap.h>

#ifdef __cplusplus
extern "C" {
#endif

static const CLAP_CONSTEXPR char CLAP_EXT_SUPPLE_TRACE[] = "com.suppleaudio.trace/1";

typedef struct clap_plugin_supple_trace {
    /* sizeof(clap_plugin_supple_trace_t) of the header the plugin was built
     * with. Hosts must not call members beyond it; new members are only ever
     * appended. */
    uint32_t struct_size;

    /* [main-thread, inactive] Starts a trace, replacing any before it: from
     * the first block processed after this call, each name the plugin traces
     * gets `bins` bins of `interval` frames. bins 0 stops tracing. Returns
     * false, changing nothing, while the plugin is active, for an interval of
     * 0, or for more bins than it keeps (at least 4096). */
    bool(CLAP_ABI* start)(const clap_plugin_t* plugin, uint32_t interval, uint32_t bins);

    /* [main-thread, inactive] Writes what the trace holds to `out` as UTF-8
     * JSON with the schema supple.trace/1.0 (see the spec). Returns false,
     * writing nothing, while the plugin is active or when no trace was
     * started. */
    bool(CLAP_ABI* read)(const clap_plugin_t* plugin, const clap_ostream_t* out);
} clap_plugin_supple_trace_t;

#ifdef __cplusplus
}
#endif
