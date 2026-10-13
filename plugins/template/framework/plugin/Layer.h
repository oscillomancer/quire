// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A layer between the framework and a family of products: what a suite of
// plugins shares beyond the framework -- a CLAP extension of its own, state
// it keeps beside the framework's, work on the main thread -- without a line
// of it in framework/.
//
// A product with PRODUCT_LAYER ON in product.cmake defines, in one of its
// sources (or a library it links),
//
//   std::unique_ptr<supple::plugin::Layer> product::makeLayer(supple::plugin::Instance& instance);
//
// and every plugin instance gets one, made after the host's init and gone
// before destroy. A layer and its Instance live on the main thread only.
//
//   - CLAP extensions: extension(id) is asked before the framework's own,
//     and Layer::of(plugin) finds the layer from a callback's clap_plugin_t.
//   - State: chunkTag() names a chunk saved in every state after the
//     framework's (core/StateFormat.h). saveChunk() writes it from the
//     snapshot being saved; loadChunk() reads it back into the snapshot
//     being loaded, before anything is applied, and may refuse the whole
//     load. A state without the chunk loads as it would without the layer.
//   - Time: tick() about 30 times a second while the instance lives (the
//     host's timer), and onMainThread() after Instance::requestLayerCallback().
//   - Redefinition: Instance::redefine() changes what the plugin is -- its
//     parameters, buses, name -- keeping its ID. The framework asks the host
//     to restart and swaps at the deactivation (CLAP_PARAM_RESCAN_ALL),
//     carrying the state over through saveChunk/loadChunk and by parameter
//     ID, then calls redefined().
//   - The editor: createEditor() may make every editor the instance opens,
//     in place of product::createEditor (a family's editors, made from what
//     only the layer knows); null leaves it to the product.
//     Instance::rebuildEditor() makes an open one again, and
//     Instance::drawEditor() draws one without a window (gui/Offscreen.h),
//     after Instance::watch(true) and some audio has filled its meters.
#pragma once

#include <clap/clap.h>

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/Definition.h"
#include "core/Picture.h"
#include "core/StateFormat.h"

namespace supple::gui {
class EditorBase;
class EditorHost;
}  // namespace supple::gui

namespace supple::plugin {

// One plugin instance, as a layer sees it. Main thread only.
class Instance {
  public:
    virtual ~Instance() = default;

    virtual const clap_plugin_t* clapPlugin() const = 0;
    virtual const clap_host_t* clapHost() const = 0;
    // The host's extension with this ID, or null.
    virtual const void* hostExtension(const char* id) const = 0;
    // What the plugin is now.
    virtual const DefinitionPtr& currentDefinition() const = 0;
    virtual bool isActive() const = 0;

    // The whole state as a snapshot (host values by table index, every
    // property), and a snapshot applied as a state load applies it: the
    // audio thread hears it at its next block, a file property's file is
    // loaded first, and hosts are told the values moved. apply() is a state
    // load in its context (a session's also restores the editor's size and
    // which preset it was); applyValues() sets every value and property and
    // nothing else, as a change made inside the plugin.
    virtual state::Snapshot snapshot() const = 0;
    virtual void apply(const state::Snapshot& snapshot, state::Context context) = 0;
    virtual void applyValues(const state::Snapshot& snapshot) = 0;
    // The session changed from inside the plugin: the host should save it.
    virtual void markDirty() = 0;

    // The processor (a product::Processor), for what is handed to it
    // without a lock: an atomic pointer it takes at a block's start. Nothing
    // else of it is the main thread's to touch while the plugin is active.
    virtual void* processor() = 0;

    // Changes what the plugin is (see the top of this file). False, changing
    // nothing, for a definition with problems, another ID or state tag.
    virtual bool redefine(DefinitionPtr next) = 0;

    // An open editor made again, by the layer's createEditor() or the
    // product's, as after a redefinition: at the same scale, if the host
    // agrees to its size. Nothing when none is open.
    virtual void rebuildEditor() = 0;
    // Telemetry (and the audio feed) as an open editor gets them, while on,
    // window or not: what plays meanwhile reaches drawEditor()'s meters.
    virtual void watch(bool on) = 0;
    // The editor as it would open now, drawn without a window at `scale`
    // times its design size (gui/Offscreen.h), after the pointer input, in
    // order: what a click or a drag edits reaches the host as a user's edit.
    // False, saying why, in a build without an editor or where nothing can
    // be drawn offscreen.
    virtual bool drawEditor(double scale, std::span<const PointerInput> input, Picture& picture,
                            std::string& why) = 0;
    bool drawEditor(double scale, Picture& picture, std::string& why) {
        return drawEditor(scale, {}, picture, why);
    }

    // Any thread: a call to the layer's onMainThread() soon.
    virtual void requestLayerCallback() = 0;
    virtual void log(clap_log_severity severity, std::string_view text) const = 0;
};

class Layer {
  public:
    virtual ~Layer() = default;

    // A CLAP extension of the layer's, or null for the framework's.
    virtual const void* extension(const char* id) {
        (void)id;
        return nullptr;
    }

    // The tag of the layer's state chunk; 0 for none.
    virtual uint32_t chunkTag() const { return 0; }
    // The chunk's payload for this snapshot (the state being saved).
    virtual std::vector<uint8_t> saveChunk(const state::Snapshot& snapshot, state::Context context) {
        (void)snapshot, (void)context;
        return {};
    }
    // Reads the payload into the snapshot being loaded; false refuses the
    // whole load, and nothing is applied.
    virtual bool loadChunk(const uint8_t* data, size_t size, state::Snapshot& snapshot, state::Context context) {
        (void)data, (void)size, (void)snapshot, (void)context;
        return true;
    }

    virtual void tick() {}
    virtual void onMainThread() {}
    // After the host's activation and deactivation.
    virtual void activated() {}
    virtual void deactivated() {}
    // After a redefinition took effect.
    virtual void redefined() {}

#if SUPPLE_WITH_GUI
    // The editor for `host` (the plugin), or null for product::createEditor.
    // Main thread, whenever one opens or is rebuilt. Only in a build with an
    // editor: override it inside #if SUPPLE_WITH_GUI.
    virtual std::unique_ptr<gui::EditorBase> createEditor(gui::EditorHost& host);
#endif

    // The layer of the plugin a CLAP callback was given, or null.
    static Layer* of(const clap_plugin_t* plugin);
};

}  // namespace supple::plugin

#if SUPPLE_PRODUCT_LAYER
namespace product {
std::unique_ptr<supple::plugin::Layer> makeLayer(supple::plugin::Instance& instance);
}  // namespace product
#endif
