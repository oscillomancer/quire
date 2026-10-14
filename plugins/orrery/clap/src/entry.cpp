// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// orrery-headless.clap: Orrery without an editor (tests, the sandbox, the
// engine). The template-derived plugin in plugins/orrery adds the editor.
#include <clap/clap.h>

#include <cstring>

#include "orrery/clap_plugin.h"

namespace {

const clap_plugin_factory_t kFactory = {
    [](const clap_plugin_factory_t*) -> uint32_t { return 1; },
    [](const clap_plugin_factory_t*, uint32_t i) {
        return i == 0 ? supple::orrery::clapDescriptor() : nullptr;
    },
    [](const clap_plugin_factory_t*, const clap_host_t* host, const char* id) {
        return std::strcmp(id, supple::orrery::clapDescriptor()->id)
                   ? nullptr
                   : supple::orrery::createClapPlugin(host);
    }};

}  // namespace

extern "C" CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    CLAP_VERSION_INIT, [](const char*) { return true; }, [] {},
    [](const char* id) -> const void* {
        return std::strcmp(id, CLAP_PLUGIN_FACTORY_ID) ? nullptr : &kFactory;
    }};
