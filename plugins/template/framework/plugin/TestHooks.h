// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// For the in-process plugin tests only: reach what an open editor would,
// without opening a window. Hidden like everything else; never exported.
#pragma once

#include <clap/clap.h>

#include <cstdint>

namespace supple::gui {
class EditorHost;
}

// The plugin's editor-facing interface (null without a GUI build).
supple::gui::EditorHost* supple_test_editor_host(const clap_plugin_t* plugin);
// What opening and closing an editor does to telemetry.
void supple_test_set_metering(const clap_plugin_t* plugin, bool on);
// Debug builds: a bit per entry point (plugin/Plugin.cpp's Site) the host
// called on a thread CLAP does not allow. 0 in a release build.
uint64_t supple_test_wrong_threads(const clap_plugin_t* plugin);
// What the audio thread found wrong (Plugin.cpp's Report bits).
uint32_t supple_test_reports(const clap_plugin_t* plugin);
// Whether the host last said it renders offline (CLAP render).
bool supple_test_offline(const clap_plugin_t* plugin);
// PRODUCT_TUNING: what retuning() answers now, as if from the audio thread.
double supple_test_retuning(const clap_plugin_t* plugin, int key, int channel);
