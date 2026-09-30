// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Delay — a minimal CLAP host, for finding out what actually happens.
//
// The GUI preview tool renders the editor directly, which proves the drawing
// works and proves nothing about the plugin. This loads the built .clap the way
// a DAW does -- dlopen, factory, init, activate, gui->create/set_parent/show,
// a host timer, and real process() calls -- and reports what came back. It
// exists because "loads in the host but nothing moves" is not diagnosable by
// reading code.
//
//   delay-clap-probe <path-to-binary-inside-.clap> [blocks]
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <clap/clap.h>

// Loading a shared library is the one thing these hosts do that has no
// portable spelling. dlopen is POSIX; MSVC ships no <dlfcn.h> at all, so the
// three calls below are the whole of the difference.
#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
namespace {
using LibHandle = HMODULE;
LibHandle libOpen(const char* p) { return LoadLibraryA(p); }
void* libSym(LibHandle h, const char* n) { return (void*)GetProcAddress(h, n); }
std::string libError() { return "error " + std::to_string((unsigned long)GetLastError()); }
}  // namespace
#else
#  include <dlfcn.h>
namespace {
using LibHandle = void*;
LibHandle libOpen(const char* p) { return dlopen(p, RTLD_LOCAL | RTLD_NOW); }
void* libSym(LibHandle h, const char* n) { return dlsym(h, n); }
std::string libError() { const char* e = dlerror(); return e ? e : "unknown error"; }
}  // namespace
#endif

#if defined(__APPLE__)
#import <Cocoa/Cocoa.h>
#endif

#include "DelayParams.h"

// AppKit creates and tears down views inside an autorelease pool that drains
// every run-loop iteration. A host harness that never drains one is a weaker
// test than the real thing: anything the plugin leaves autoreleased survives
// here and dies there. Editor setup below is wrapped accordingly.
#if defined(__APPLE__)
#  define PROBE_POOL @autoreleasepool
#else
#  define PROBE_POOL
#endif

namespace {

struct Probe {
    clap_host_t host{};
    const clap_plugin_t* plugin = nullptr;

    // What we are here to measure.
    int timersRegistered = 0;
    clap_id timerId = CLAP_INVALID_ID;
    int onTimerCalls = 0;
    int flushRequests = 0;
    int outEventsSeen = 0;
    int callbacksRequested = 0;
    bool callbackPending = false;
    bool guiCreated = false;
    bool guiParented = false;
    bool guiShown = false;
};

Probe g;

// --- host extensions -------------------------------------------------------

// Set by --no-host-timer, to reproduce the wrappers that refuse timers.
bool gRefuseTimers = false;

bool CLAP_ABI registerTimer(const clap_host_t*, uint32_t periodMs, clap_id* id) {
    (void)periodMs;
    if (gRefuseTimers) return false;
    *id = 1;
    g.timerId = 1;
    ++g.timersRegistered;
    return true;
}
bool CLAP_ABI unregisterTimer(const clap_host_t*, clap_id) { return true; }
const clap_host_timer_support_t kHostTimer = {registerTimer, unregisterTimer};

void CLAP_ABI rescan(const clap_host_t*, clap_param_rescan_flags) {}
void CLAP_ABI clearParam(const clap_host_t*, clap_id, clap_param_clear_flags) {}
void CLAP_ABI requestFlush(const clap_host_t*) { ++g.flushRequests; }
const clap_host_params_t kHostParams = {rescan, clearParam, requestFlush};

void CLAP_ABI latencyChanged(const clap_host_t*) {}
const clap_host_latency_t kHostLatency = {latencyChanged};

void CLAP_ABI resizeHintsChanged(const clap_host_t*) {}
bool CLAP_ABI requestResize(const clap_host_t*, uint32_t, uint32_t) { return true; }
bool CLAP_ABI requestShow(const clap_host_t*) { return true; }
bool CLAP_ABI requestHide(const clap_host_t*) { return true; }
void CLAP_ABI guiClosed(const clap_host_t*, bool) {}
const clap_host_gui_t kHostGui = {resizeHintsChanged, requestResize, requestShow, requestHide,
                                  guiClosed};

const void* CLAP_ABI hostGetExtension(const clap_host_t*, const char* id) {
    if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &kHostTimer;
    if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &kHostParams;
    if (!std::strcmp(id, CLAP_EXT_LATENCY)) return &kHostLatency;
    if (!std::strcmp(id, CLAP_EXT_GUI)) return &kHostGui;
    return nullptr;
}
void CLAP_ABI hostRequestRestart(const clap_host_t*) {}
void CLAP_ABI hostRequestProcess(const clap_host_t*) {}
void CLAP_ABI hostRequestCallback(const clap_host_t*) {
    ++g.callbacksRequested;
    g.callbackPending = true;
}

// --- event plumbing --------------------------------------------------------

uint32_t CLAP_ABI inSize(const clap_input_events_t*) { return 0; }
const clap_event_header_t* CLAP_ABI inGet(const clap_input_events_t*, uint32_t) { return nullptr; }

bool CLAP_ABI outPush(const clap_output_events_t*, const clap_event_header_t*) {
    ++g.outEventsSeen;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: delay-clap-probe <binary-inside-.clap> [blocks]\n");
        return 2;
    }
    int blocks = 400;
    // --mono selects the plugin's declared mono port configuration before
    // activation, the same contract a real host follows.
    uint32_t channels = 2;
    // AU hosts open and close the editor repeatedly, and the AU wrapper owns
    // the Cocoa view rather than the plugin. --reopen runs the editor through a
    // second create/set_parent/show against a fresh parent view; --drop-parent
    // releases the parent out from under a live editor, which is what a host
    // does when it tears its window down without telling the plugin first.
    bool reopen = false, dropParent = false, destroyHot = false, resize = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-host-timer")) gRefuseTimers = true;
        else if (!std::strcmp(argv[i], "--mono")) channels = 1;
        else if (!std::strcmp(argv[i], "--reopen")) reopen = true;
        else if (!std::strcmp(argv[i], "--drop-parent")) dropParent = true;
        else if (!std::strcmp(argv[i], "--destroy-hot")) destroyHot = true;
        else if (!std::strcmp(argv[i], "--resize")) resize = true;
        else blocks = std::atoi(argv[i]);
    }
    if (gRefuseTimers) std::printf("(refusing to register host timers)\n");
    std::printf("(bus channels: %u)\n", channels);

    LibHandle lib = libOpen(argv[1]);
    if (!lib) {
        std::fprintf(stderr, "load failed: %s\n", libError().c_str());
        return 1;
    }
    auto* entry = (const clap_plugin_entry_t*)libSym(lib, "clap_entry");
    if (!entry) {
        std::fprintf(stderr, "no clap_entry\n");
        return 1;
    }
    entry->init(argv[1]);

    auto* factory = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
    std::printf("plugin: %s %s\n", desc->name, desc->version);

    g.host.clap_version = CLAP_VERSION_INIT;
    g.host.name = "delay-clap-probe";
    g.host.vendor = "Delay";
    g.host.url = "";
    g.host.version = "1.0";
    g.host.get_extension = hostGetExtension;
    g.host.request_restart = hostRequestRestart;
    g.host.request_process = hostRequestProcess;
    g.host.request_callback = hostRequestCallback;

    g.plugin = factory->create_plugin(factory, &g.host, desc->id);
    if (!g.plugin || !g.plugin->init(g.plugin)) {
        std::fprintf(stderr, "create/init failed\n");
        return 1;
    }
    constexpr uint32_t kBlock = 512;
    auto* portConfigs = (const clap_plugin_audio_ports_config_t*)g.plugin->get_extension(
        g.plugin, CLAP_EXT_AUDIO_PORTS_CONFIG);
    if (!portConfigs || !portConfigs->select(g.plugin, channels == 1 ? 1 : 2)) {
        std::fprintf(stderr, "audio port configuration unavailable\n");
        return 1;
    }
    g.plugin->activate(g.plugin, 48000.0, kBlock, kBlock);
    g.plugin->start_processing(g.plugin);

    auto* params = (const clap_plugin_params_t*)g.plugin->get_extension(g.plugin, CLAP_EXT_PARAMS);
    auto* gui = (const clap_plugin_gui_t*)g.plugin->get_extension(g.plugin, CLAP_EXT_GUI);
    auto* timer =
        (const clap_plugin_timer_support_t*)g.plugin->get_extension(g.plugin,
                                                                    CLAP_EXT_TIMER_SUPPORT);
    std::printf("extensions: params=%s gui=%s timer=%s\n", params ? "yes" : "NO",
                gui ? "yes" : "NO", timer ? "yes" : "NO");

    // --- open the editor exactly as a host would ---------------------------
#if defined(__APPLE__)
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSWindow* win = [[NSWindow alloc]
        initWithContentRect:NSMakeRect(0, 0, 820, 480)
                  styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                    backing:NSBackingStoreBuffered
                      defer:NO];
    [win setTitle:@"delay-clap-probe"];
    NSView* parent = [win contentView];
#else
    void* parent = nullptr;
#endif

    if (gui) PROBE_POOL {
        const char* api = nullptr;
        bool floating = false;
        gui->get_preferred_api(g.plugin, &api, &floating);
        std::printf("gui preferred api: %s floating=%d supported=%d\n", api ? api : "(null)",
                    (int)floating, (int)gui->is_api_supported(g.plugin, api, false));

        g.guiCreated = gui->create(g.plugin, api, false);
        uint32_t gw = 0, gh = 0;
        gui->get_size(g.plugin, &gw, &gh);

        clap_window_t w{};
        w.api = api;
        w.ptr = (void*)parent;
        g.guiParented = gui->set_parent(g.plugin, &w);
        g.guiShown = gui->show(g.plugin);
        std::printf("gui: create=%d parent=%d show=%d size=%ux%u\n", (int)g.guiCreated,
                    (int)g.guiParented, (int)g.guiShown, gw, gh);
    }
#if defined(__APPLE__)
    [win makeKeyAndOrderFront:nil];

    // Pump once so the editor is fully realized and has drawn before any of
    // the teardown games below.
    @autoreleasepool {
        for (NSEvent* e = nil;
             (e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                     untilDate:[NSDate distantPast]
                                        inMode:NSDefaultRunLoopMode
                                       dequeue:YES]);)
            [NSApp sendEvent:e];
    }

    if (gui && reopen) {
        std::printf("reopening the editor...\n");
        gui->destroy(g.plugin);

        NSView* fresh = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 820, 480)];
        [win setContentView:fresh];

        const char* api2 = nullptr;
        bool floating2 = false;
        gui->get_preferred_api(g.plugin, &api2, &floating2);
        const bool c2 = gui->create(g.plugin, api2, false);
        clap_window_t w2{};
        w2.api = api2;
        w2.ptr = (void*)fresh;
        const bool p2 = gui->set_parent(g.plugin, &w2);
        const bool s2 = gui->show(g.plugin);
        std::printf("reopen: create=%d parent=%d show=%d\n", (int)c2, (int)p2, (int)s2);
    }

    // Tear the editor down with a repaint already queued in CoreAnimation --
    // which is the state the AU wrapper's fallback timer leaves it in every
    // frame. If anything of ours outlives the view and CA still messages it,
    // this is where it shows.
    if (gui && destroyHot) {
        std::printf("destroying the editor with a repaint queued...\n");
        if (timer && g.timerId != CLAP_INVALID_ID) timer->on_timer(g.plugin, g.timerId);
        gui->destroy(g.plugin);
        gui = nullptr;
        for (int i = 0; i < 40; ++i) {
            @autoreleasepool {
                for (NSEvent* e = nil;
                     (e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES]);)
                    [NSApp sendEvent:e];
            }
        }
        std::printf("survived teardown\n");
    }

    // The AU wrapper drives gui->set_size() from the Cocoa view's setFrame:,
    // so resizing is a routine path there and worth exercising. Under Metal it
    // also has to resize the layer's drawable, which is not automatic.
    if (gui && resize) {
        uint32_t rw = 1400, rh = 896;
        gui->adjust_size(g.plugin, &rw, &rh);
        const bool ok = gui->set_size(g.plugin, rw, rh);
        [win setContentSize:NSMakeSize(rw, rh)];
        std::printf("resize: adjust->%ux%u set_size=%d\n", rw, rh, (int)ok);
        for (int i = 0; i < 30; ++i) {
            @autoreleasepool {
                for (NSEvent* e = nil;
                     (e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES]);)
                    [NSApp sendEvent:e];
            }
        }
    }

    if (gui && dropParent) {
        // Swap the parent view out without telling the plugin. The editor's
        // views go with it; the plugin still holds its GUI.
        std::printf("dropping the parent view out from under the editor...\n");
        [win setContentView:[[NSView alloc]
                                initWithFrame:NSMakeRect(0, 0, 820, 480)]];
    }
#endif

    // --- run: audio blocks plus host timer callbacks ------------------------
    std::vector<float> l(kBlock), r(kBlock);
    float* chans[2] = {l.data(), r.data()};
    clap_audio_buffer_t inBuf{}, outBuf{};
    inBuf.data32 = chans;
    inBuf.channel_count = channels;
    outBuf.data32 = chans;
    outBuf.channel_count = channels;

    clap_input_events_t inEv{nullptr, inSize, inGet};
    clap_output_events_t outEv{nullptr, outPush};

    clap_process_t proc{};
    proc.frames_count = kBlock;
    proc.audio_inputs = &inBuf;
    proc.audio_inputs_count = 1;
    proc.audio_outputs = &outBuf;
    proc.audio_outputs_count = 1;
    proc.in_events = &inEv;
    proc.out_events = &outEv;
    proc.steady_time = 0;

    double phase = 0.0;
    for (int b = 0; b < blocks; ++b) {
        // A healthy sine, so the processor and live meters have something to do.
        for (uint32_t i = 0; i < kBlock; ++i) {
            const float s = (float)(0.9 * std::sin(phase));
            phase += 2.0 * M_PI * 220.0 / 48000.0;
            l[i] = s;
            r[i] = s;
        }
        g.plugin->process(g.plugin, &proc);
        if (g.callbackPending) {
            g.callbackPending = false;
            g.plugin->on_main_thread(g.plugin);
        }
        proc.steady_time += kBlock;

        if (timer && g.timerId != CLAP_INVALID_ID) {
            timer->on_timer(g.plugin, g.timerId);
            ++g.onTimerCalls;
        }
#if defined(__APPLE__)
        // A host pumps its own run loop; the plugin's PUGL_MODULE world does not.
        @autoreleasepool {
            NSDate* deadline = gRefuseTimers
                                   ? [NSDate dateWithTimeIntervalSinceNow:0.002]
                                   : [NSDate distantPast];
            for (NSEvent* e = nil;
                 (e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                         untilDate:deadline
                                            inMode:NSDefaultRunLoopMode
                                           dequeue:YES]);)
                [NSApp sendEvent:e];
        }
#endif
    }

#if defined(__APPLE__)
    // Give a native fallback timer and CoreAnimation a real-time window to
    // publish the final frame. A tight offline audio loop otherwise completes
    // before one 16 ms timer interval has elapsed and does not test fallback.
    if (gRefuseTimers) {
        for (int i = 0; i < 8; ++i) {
            @autoreleasepool {
                for (NSEvent* e = nil;
                     (e = [NSApp nextEventMatchingMask:NSEventMaskAny
                                             untilDate:[NSDate dateWithTimeIntervalSinceNow:0.005]
                                                inMode:NSDefaultRunLoopMode
                                               dequeue:YES]);)
                    [NSApp sendEvent:e];
            }
        }
    }
#endif

    // This effect sends its wet level through GUI telemetry only.

#if defined(__APPLE__)
    // Grab what the editor actually put on screen inside the host's window.
    // Reading numbers proves the plumbing; this proves the panel.
    {
        char cmd[256];
        std::snprintf(cmd, sizeof(cmd), "screencapture -x -o -l%ld probe-window.png",
                      (long)[win windowNumber]);
        if (system(cmd) == 0) std::printf("wrote probe-window.png\n");
    }
#endif

    std::printf("\n--- results ---\n");
    std::printf("timers registered : %d\n", g.timersRegistered);
    std::printf("on_timer calls    : %d\n", g.onTimerCalls);
    std::printf("output meter      : GUI telemetry only\n");
    std::printf("flush requests    : %d\n", g.flushRequests);
    std::printf("out events        : %d\n", g.outEventsSeen);
    std::printf("main callbacks    : %d\n", g.callbacksRequested);

    if (gui) gui->destroy(g.plugin);
    g.plugin->stop_processing(g.plugin);
    g.plugin->deactivate(g.plugin);
    g.plugin->destroy(g.plugin);
    entry->deinit();
    return 0;
}
