// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Implemented in Plugin.cpp, which lives in the static implementation
// library; exported as clap_entry by Entry.cpp, which clap-wrapper compiles
// once per format (CLAP, VST3, AUv2) against that same library.
#pragma once

bool supple_entry_init(const char* pluginPath);
void supple_entry_deinit();
const void* supple_entry_get_factory(const char* factoryId);
