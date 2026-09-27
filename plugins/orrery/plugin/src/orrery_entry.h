// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Entry points implemented in orrery_clap.cpp (the static impl library) and
// exported by orrery_entry.cpp. Split this way because clap-wrapper recompiles
// the export fragment once per plugin format while linking the same impl.
#pragma once

bool orrery_entry_init(const char* plugin_path);
void orrery_entry_deinit();
const void* orrery_entry_get_factory(const char* factory_id);
