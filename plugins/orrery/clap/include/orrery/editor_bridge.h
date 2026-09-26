// What an editor sees of an Orrery instance, and the hook through which the
// template-derived plugin adds its editor (clap.gui, clap.timer-support) to
// the shared CLAP core.
//
// Every EditorBridge call is main-thread only. Value edits are bracketed by
// begin/end so the host records automation: the core queues them to the
// audio thread, which applies them to the engine and reports them to the host
// as CLAP param events (or params.flush does, while the host is not
// processing). Structural edits are semantic merge patches — the same path as
// the semantic extension's set_patch — so a GUI can never express a patch the
// codec would reject.
#pragma once

#include <clap/clap.h>

#include <cstdint>
#include <string>

#include "orrery/patch.h"
#include "supple/base/result.h"
#include "supple/json/json.h"

namespace supple::orrery {

struct Telemetry {
    float peakL = 0.0f, peakR = 0.0f;  // linear, since the previous telemetry() call
    int voices = 0;                    // sounding voices (notes × unison)
    bool processing = false;
};

class EditorBridge {
  public:
    virtual ~EditorBridge() = default;

    // The patch as it is now (host automation, state loads and set_patch
    // included). revision() changes whenever structure or any value changes.
    virtual const Structure& structure() const = 0;
    virtual double value(std::size_t index) const = 0;  // physical, table() order
    virtual std::uint64_t revision() const = 0;

    // A gesture on one parameter; edits outside a gesture are sent as a
    // complete gesture of their own.
    virtual void beginEdit(std::size_t index) = 0;
    virtual void edit(std::size_t index, double physical) = 0;
    virtual void endEdit(std::size_t index) = 0;

    // Structure changes (add/remove slots, types, routes, fx chain): an RFC
    // 7386 merge patch over the semantic patch. Errors carry paths and hints.
    virtual Status merge(const json::Value& patch) = 0;
    virtual std::string patchText(bool changedOnly) const = 0;
    virtual std::string describe() const = 0;

    // Telemetry is measured only while enabled (an editor is showing).
    virtual void setTelemetryEnabled(bool on) = 0;
    virtual Telemetry telemetry() = 0;
};

// Extensions a plugin module adds to the core. `create` runs in init() (main
// thread) and returns the module's per-instance data; `get` returns static
// extension vtables for ids the core does not implement; `onMainThread` runs
// from clap_plugin.on_main_thread.
struct ClapExtensions {
    void* (*create)(const clap_plugin_t* plugin, const clap_host_t* host,
                    EditorBridge& bridge) = nullptr;
    void (*destroy)(void* data) = nullptr;
    const void* (*get)(const char* id) = nullptr;
    void (*onMainThread)(void* data) = nullptr;
};

// The module data `create` returned, for use inside extension callbacks.
void* extensionData(const clap_plugin_t* plugin);

}  // namespace supple::orrery
