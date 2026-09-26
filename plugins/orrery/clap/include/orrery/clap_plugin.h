// Orrery as a CLAP instrument: params, state, note/audio ports and the
// com.suppleaudio.semantic-params/1 extension over the structured patch.
// The template-derived plugin (plugins/orrery, with the editor) and the
// headless module (orrery-headless.clap, tests and the sandbox) share it.
#pragma once

#include <clap/clap.h>

namespace supple::orrery {

const clap_plugin_descriptor_t* clapDescriptor();
// Creates an instance (plugin_data owned by the returned plugin; destroy()
// frees it). Returns null for an incompatible host.
const clap_plugin_t* createClapPlugin(const clap_host_t* host);

}  // namespace supple::orrery
