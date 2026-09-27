// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The Orrery plugin module: the shared CLAP core (plugins/orrery/clap: params,
// state, note/audio ports, the semantic extension, the engine) plus this
// template-derived editor. The core owns everything the host can observe; this
// file only adds clap.gui and clap.timer-support through ClapExtensions, and
// the factory and entry points the wrappers consume.
#include <clap/clap.h>

#include <cstdint>
#include <cstring>

#include "orrery/clap_plugin.h"
#include "orrery_entry.h"

#if ORRERY_WITH_GUI
#include "../gui/Gui.h"
#endif

namespace {

using supple::orrery::EditorBridge;

#if ORRERY_WITH_GUI

// Per-instance editor state, owned by the core (ClapExtensions::create/destroy).
struct Module {
    const clap_host_t* host = nullptr;
    EditorBridge* bridge = nullptr;
    orrery::gui::Gui* gui = nullptr;
    bool timerRunning = false;
    clap_id timerId = CLAP_INVALID_ID;
};

Module* M(const clap_plugin_t* p) { return static_cast<Module*>(supple::orrery::extensionData(p)); }

#if defined(_WIN32)
constexpr const char* kNativeApi = CLAP_WINDOW_API_WIN32;
#elif defined(__APPLE__)
constexpr const char* kNativeApi = CLAP_WINDOW_API_COCOA;
#else
constexpr const char* kNativeApi = CLAP_WINDOW_API_X11;
#endif

constexpr uint32_t kGuiTimerMs = 16;  // ~60 Hz

void startTimer(Module* m) {
    if (m->timerRunning || !m->host) return;
    auto* ht = static_cast<const clap_host_timer_support_t*>(
        m->host->get_extension(m->host, CLAP_EXT_TIMER_SUPPORT));
    if (ht && ht->register_timer && ht->register_timer(m->host, kGuiTimerMs, &m->timerId))
        m->timerRunning = true;
}

void stopTimer(Module* m) {
    if (!m->timerRunning || !m->host) return;
    auto* ht = static_cast<const clap_host_timer_support_t*>(
        m->host->get_extension(m->host, CLAP_EXT_TIMER_SUPPORT));
    if (ht && ht->unregister_timer) ht->unregister_timer(m->host, m->timerId);
    m->timerRunning = false;
}

bool CLAP_ABI guiIsApiSupported(const clap_plugin_t*, const char* api, bool isFloating) {
    return !isFloating && api && !std::strcmp(api, kNativeApi);
}

bool CLAP_ABI guiGetPreferredApi(const clap_plugin_t*, const char** api, bool* isFloating) {
    *api = kNativeApi;
    *isFloating = false;
    return true;
}

bool CLAP_ABI guiCreate(const clap_plugin_t* p, const char* api, bool isFloating) {
    if (!guiIsApiSupported(p, api, isFloating)) return false;
    Module* m = M(p);
    if (!m->gui) m->gui = new orrery::gui::Gui(*m->bridge);
    if (!m->gui->create()) {
        delete m->gui;
        m->gui = nullptr;
        m->bridge->setTelemetryEnabled(false);
        return false;
    }
    startTimer(m);
    return true;
}

void CLAP_ABI guiDestroy(const clap_plugin_t* p) {
    Module* m = M(p);
    stopTimer(m);
    delete m->gui;
    m->gui = nullptr;
    m->bridge->setTelemetryEnabled(false);
}

bool CLAP_ABI guiSetScale(const clap_plugin_t* p, double scale) {
#if defined(__APPLE__)
    // Cocoa is specified in logical pixels, so the host must not also scale us.
    (void)p;
    (void)scale;
    return false;
#else
    Module* m = M(p);
    if (!m->gui) return false;
    m->gui->setScale(scale);
    return true;
#endif
}

bool CLAP_ABI guiGetSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    Module* m = M(p);
    if (!m->gui) return false;
    m->gui->getSize(*w, *h);
    return true;
}

bool CLAP_ABI guiCanResize(const clap_plugin_t*) { return true; }

bool CLAP_ABI guiGetResizeHints(const clap_plugin_t*, clap_gui_resize_hints_t* hints) {
    bool h = false, v = false, keep = false;
    uint32_t aw = 0, ah = 0;
    orrery::gui::Gui::resizeHints(h, v, keep, aw, ah);
    hints->can_resize_horizontally = h;
    hints->can_resize_vertically = v;
    hints->preserve_aspect_ratio = keep;
    hints->aspect_ratio_width = aw;
    hints->aspect_ratio_height = ah;
    return true;
}

bool CLAP_ABI guiAdjustSize(const clap_plugin_t* p, uint32_t* w, uint32_t* h) {
    Module* m = M(p);
    return m->gui && m->gui->adjustSize(*w, *h);
}

bool CLAP_ABI guiSetSize(const clap_plugin_t* p, uint32_t w, uint32_t h) {
    Module* m = M(p);
    return m->gui && m->gui->setSize(w, h);
}

bool CLAP_ABI guiSetParent(const clap_plugin_t* p, const clap_window_t* window) {
    Module* m = M(p);
    if (!m->gui || !window) return false;
    const bool ok = m->gui->setParent(reinterpret_cast<uintptr_t>(window->ptr));
    // Not every wrapper implements clap.timer-support (clap-wrapper's AUv2
    // refuses): drive the editor ourselves then, or it never repaints.
    if (ok && !m->timerRunning)
        m->gui->startFallbackTimer(static_cast<double>(kGuiTimerMs) / 1000.0);
    return ok;
}

bool CLAP_ABI guiSetTransient(const clap_plugin_t*, const clap_window_t*) { return false; }
void CLAP_ABI guiSuggestTitle(const clap_plugin_t*, const char*) {}

bool CLAP_ABI guiShow(const clap_plugin_t* p) {
    Module* m = M(p);
    if (!m->gui) return false;
    const bool shown = m->gui->show();
    m->bridge->setTelemetryEnabled(shown);
    if (shown) {
        startTimer(m);
        if (!m->timerRunning) m->gui->startFallbackTimer(static_cast<double>(kGuiTimerMs) / 1000.0);
    }
    return shown;
}

bool CLAP_ABI guiHide(const clap_plugin_t* p) {
    Module* m = M(p);
    m->bridge->setTelemetryEnabled(false);
    stopTimer(m);
    if (m->gui) m->gui->stopFallbackTimer();
    return m->gui && m->gui->hide();
}

const clap_plugin_gui_t kGui = {
    guiIsApiSupported, guiGetPreferredApi, guiCreate,         guiDestroy,    guiSetScale,
    guiGetSize,        guiCanResize,       guiGetResizeHints, guiAdjustSize, guiSetSize,
    guiSetParent,      guiSetTransient,    guiSuggestTitle,   guiShow,       guiHide};

void CLAP_ABI onGuiTimer(const clap_plugin_t* p, clap_id timerId) {
    Module* m = M(p);
    if (m->gui && timerId == m->timerId) m->gui->tick();
}

const clap_plugin_timer_support_t kTimerSupport = {onGuiTimer};

const supple::orrery::ClapExtensions kExtensions = {
    [](const clap_plugin_t*, const clap_host_t* host, EditorBridge& bridge) -> void* {
        auto* m = new Module();
        m->host = host;
        m->bridge = &bridge;
        return m;
    },
    [](void* data) {
        // Hosts should destroy the editor first; not all do, and a timer left
        // pointing at freed memory is not survivable.
        auto* m = static_cast<Module*>(data);
        stopTimer(m);
        delete m->gui;
        delete m;
    },
    [](const char* id) -> const void* {
        if (!std::strcmp(id, CLAP_EXT_GUI)) return &kGui;
        if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kTimerSupport;
        return nullptr;
    },
    nullptr,
};
const supple::orrery::ClapExtensions* extensions() { return &kExtensions; }

#else
const supple::orrery::ClapExtensions* extensions() { return nullptr; }
#endif  // ORRERY_WITH_GUI

uint32_t CLAP_ABI factoryCount(const clap_plugin_factory_t*) { return 1; }

const clap_plugin_descriptor_t* CLAP_ABI factoryGetDesc(const clap_plugin_factory_t*,
                                                        uint32_t index) {
    return index == 0 ? supple::orrery::clapDescriptor() : nullptr;
}

const clap_plugin_t* CLAP_ABI factoryCreate(const clap_plugin_factory_t*, const clap_host_t* host,
                                            const char* id) {
    if (!id || std::strcmp(id, supple::orrery::clapDescriptor()->id)) return nullptr;
    return supple::orrery::createClapPlugin(host, extensions());
}

const clap_plugin_factory_t kFactory = {factoryCount, factoryGetDesc, factoryCreate};

}  // namespace

bool orrery_entry_init(const char*) { return true; }
void orrery_entry_deinit() {}

const void* orrery_entry_get_factory(const char* factory_id) {
    if (factory_id && !std::strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID)) return &kFactory;
    return nullptr;
}
