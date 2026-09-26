// Orrery as a CLAP instrument: params, state, note/audio ports and the
// com.suppleaudio.semantic-params/1 extension over the structured patch.
// The template-derived plugin (plugins/orrery, with the editor) and the
// headless module (orrery-headless.clap, tests and the sandbox) share it.
#pragma once

#include <clap/clap.h>

#include "orrery/editor_bridge.h"

namespace supple::orrery {

const clap_plugin_descriptor_t* clapDescriptor();
// Creates an instance (plugin_data owned by the returned plugin; destroy()
// frees it). Returns null for an incompatible host. `extensions` (static
// storage) adds a module's own extensions, e.g. the editor.
const clap_plugin_t* createClapPlugin(const clap_host_t* host,
                                      const ClapExtensions* extensions = nullptr);

}  // namespace supple::orrery
