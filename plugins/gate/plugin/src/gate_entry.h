// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Entry points implemented in gate_clap.cpp (the static impl library) and
// exported by gate_entry.cpp. Split this way because clap-wrapper recompiles
// the export fragment once per plugin format while linking the same impl.
#pragma once

bool gate_entry_init(const char* plugin_path);
void gate_entry_deinit();
const void* gate_entry_get_factory(const char* factory_id);
