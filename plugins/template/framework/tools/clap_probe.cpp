// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A minimal CLAP host, for finding out what actually happens.
//
// The GUI preview tool renders the editor directly, which proves the drawing
// works and proves nothing about the plugin. This loads the built .clap the way
// a DAW does -- dlopen, factory, init, activate, gui->create/set_parent/show,
// a host timer, and real process() calls -- and reports what came back. It
// exists because "loads in the host but nothing moves" is not diagnosable by
// reading code.
//
//   clap-probe <path-to-binary-inside-.clap> [blocks] [--mono] [--resize] [--reopen]
//              [--drop-parent] [--destroy-hot] [--no-host-timer] [--presize]
//
// Its window work is Cocoa-only; elsewhere it exercises everything but the
// window (x11-probe covers X11 embedding). Audio goes to every port the
// plugin declares; a plugin that takes notes is sent one held note, and one
// without audio (a note effect) has to send notes back.
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <clap/clap.h>

#include "ProbeAudio.h"

// Loading a shared library is the one thing these hosts do that has no
// portable spelling. dlopen is POSIX; MSVC ships no <dlfcn.h> at all, so the
// three calls below are the whole of the difference.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
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
    int notesSeen = 0;
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

// Middle C, held from the first block, when the plugin takes notes.
clap_event_note_t gNoteOn{{sizeof(clap_event_note_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0},
                          -1, 0, 0, 60, 0.8};
bool gSendNote = false;

uint32_t CLAP_ABI inSize(const clap_input_events_t*) { return gSendNote ? 1 : 0; }
const clap_event_header_t* CLAP_ABI inGet(const clap_input_events_t*, uint32_t index) {
    return gSendNote && index == 0 ? &gNoteOn.header : nullptr;
}

bool CLAP_ABI outPush(const clap_output_events_t*, const clap_event_header_t* event) {
    ++g.outEventsSeen;
    if (event->space_id == CLAP_CORE_EVENT_SPACE_ID && event->type == CLAP_EVENT_NOTE_ON) ++g.notesSeen;
    return true;
}

#if defined(__APPLE__)
// Query after parenting and pumping the host loop. A cached pre-parent size
// cannot prove what Pugl realized; the native child must agree in points too.
bool checkEditorSize(const clap_plugin_gui_t* gui, NSWindow* win,
                     uint32_t expectedW, uint32_t expectedH, const char* label) {
    uint32_t w = 0, h = 0;
    const bool reported = gui->get_size(g.plugin, &w, &h);
    NSArray<NSView*>* children = [[win contentView] subviews];
    const NSRect frame = [children count] == 1 ? [[children objectAtIndex:0] frame] : NSZeroRect;
    const bool matches = reported && w == expectedW && h == expectedH && [children count] == 1 &&
                         std::fabs(frame.size.width - expectedW) < 0.5 &&
                         std::fabs(frame.size.height - expectedH) < 0.5;
    std::printf("%s: get_size=%d size=%ux%u native=%.1fx%.1f expected=%ux%u matches=%d\n",
                label, (int)reported, w, h, (double)frame.size.width, (double)frame.size.height,
                expectedW, expectedH, (int)matches);
    return matches;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: clap-probe <binary-inside-.clap> [blocks] [options]\n");
        return 2;
    }
    int blocks = 400;
    // --mono selects the plugin's declared mono port configuration before
    // activation, the same contract a real host follows; otherwise the
    // plugin keeps its default.
    bool mono = false;
    // AU hosts open and close the editor repeatedly, and the AU wrapper owns
    // the Cocoa view rather than the plugin. --reopen runs the editor through a
    // second create/set_parent/show against a fresh parent view; --drop-parent
    // releases the parent out from under a live editor, which is what a host
    // does when it tears its window down without telling the plugin first.
    bool reopen = false, dropParent = false, destroyHot = false, resize = false;
    // Sizes the editor between create() and set_parent(), as hosts may.
    bool presize = false;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--no-host-timer")) gRefuseTimers = true;
        else if (!std::strcmp(argv[i], "--mono")) mono = true;
        else if (!std::strcmp(argv[i], "--reopen")) reopen = true;
        else if (!std::strcmp(argv[i], "--drop-parent")) dropParent = true;
        else if (!std::strcmp(argv[i], "--destroy-hot")) destroyHot = true;
        else if (!std::strcmp(argv[i], "--resize")) resize = true;
        else if (!std::strcmp(argv[i], "--presize")) presize = true;
        else blocks = std::atoi(argv[i]);
    }
#if !defined(__APPLE__)
    (void)reopen, (void)dropParent, (void)destroyHot, (void)resize;  // window tests are Cocoa-only
#endif
    if (gRefuseTimers) std::printf("(refusing to register host timers)\n");

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
    g.host.name = "clap-probe";
    g.host.vendor = "Supple Audio";
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
    if (mono) {
        const bool selected = portConfigs && portConfigs->select(g.plugin, 1);
        std::printf("%s\n", selected ? "(mono configuration selected)"
                                      : "(no mono configuration: the default layout stays)");
    }
    supple::probe::Audio audio;
    if (!audio.configure(g.plugin, kBlock)) {
        std::fprintf(stderr, "cannot query the plugin's audio ports\n");
        return 1;
    }
    auto* notePorts = (const clap_plugin_note_ports_t*)g.plugin->get_extension(g.plugin, CLAP_EXT_NOTE_PORTS);
    const bool takesNotes = notePorts && notePorts->count(g.plugin, true) > 0;
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
    // A host asks the editor its size after create() and sizes the parent to
    // it, so the window does too: a fixed guess would crop or letterbox the
    // panel and the capture would show the probe's mistake, not the editor's.
#if defined(__APPLE__)
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSWindow* win = nil;
    NSView* parent = nil;
    auto openWindow = [&](uint32_t cw, uint32_t ch) {
        win = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, (CGFloat)cw, (CGFloat)ch)
                                          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                            backing:NSBackingStoreBuffered
                                              defer:NO];
        [win setTitle:@"clap-probe"];
        parent = [win contentView];
    };
    // The editor's size in host units (points), as last reported.
    uint32_t editorW = 900, editorH = 560;
    bool sizesMatch = true;
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
        if (presize) {
            uint32_t pw = 1125, ph = 700;
            gui->adjust_size(g.plugin, &pw, &ph);
            const bool ok = gui->set_size(g.plugin, pw, ph);
            std::printf("presize: adjust->%ux%u set_size=%d\n", pw, ph, (int)ok);
#if defined(__APPLE__)
            editorW = pw;
            editorH = ph;
#endif
        }
        uint32_t gw = 0, gh = 0;
        gui->get_size(g.plugin, &gw, &gh);
#if defined(__APPLE__)
        if (!presize && gw > 0 && gh > 0) {
            editorW = gw;
            editorH = gh;
        }
        openWindow(editorW, editorH);
#endif

        clap_window_t w{};
        w.api = api;
        w.ptr = (void*)parent;
        g.guiParented = gui->set_parent(g.plugin, &w);
        g.guiShown = gui->show(g.plugin);
        std::printf("gui: create=%d parent=%d show=%d size=%ux%u\n", (int)g.guiCreated,
                    (int)g.guiParented, (int)g.guiShown, gw, gh);
    }
#if defined(__APPLE__)
    if (!win) openWindow(editorW, editorH);
    [win makeKeyAndOrderFront:nil];
    std::printf("backing scale     : %.1f\n", (double)[win backingScaleFactor]);

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

    if (gui && g.guiShown) {
        // The timer dispatches pending Pugl configure events before asking
        // for the CLAP size; native Cocoa dimensions are read independently.
        if (timer && g.timerId != CLAP_INVALID_ID) timer->on_timer(g.plugin, g.timerId);
        sizesMatch = checkEditorSize(gui, win, editorW, editorH, "realized") && sizesMatch;
    }

    if (gui && reopen) {
        std::printf("reopening the editor...\n");
        gui->destroy(g.plugin);

        NSView* fresh = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, (CGFloat)editorW, (CGFloat)editorH)];
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
        if (timer && g.timerId != CLAP_INVALID_ID) timer->on_timer(g.plugin, g.timerId);
        sizesMatch = checkEditorSize(gui, win, rw, rh, "resized") && sizesMatch;
    }

    if (gui && dropParent) {
        // Swap the parent view out without telling the plugin. The editor's
        // views go with it; the plugin still holds its GUI.
        std::printf("dropping the parent view out from under the editor...\n");
        [win setContentView:[[NSView alloc]
                                initWithFrame:NSMakeRect(0, 0, (CGFloat)editorW, (CGFloat)editorH)]];
    }
#endif

    // --- run: audio blocks plus host timer callbacks ------------------------
    clap_input_events_t inEv{nullptr, inSize, inGet};
    clap_output_events_t outEv{nullptr, outPush};

    clap_process_t proc{};
    audio.bind(proc);
    proc.in_events = &inEv;
    proc.out_events = &outEv;
    proc.steady_time = 0;

    double phase = 0.0, outputPeak = 0.0;
    for (int b = 0; b < blocks; ++b) {
        // A sine on every input, and the note, so the processor and live
        // meters have something to do.
        audio.fillInputs(phase);
        gSendNote = takesNotes && b == 0;
        g.plugin->process(g.plugin, &proc);
        gSendNote = false;
        outputPeak = std::max(outputPeak, audio.outputPeak());
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

    const double outputLevel = outputPeak > 0.0 ? 20.0 * std::log10(outputPeak) : -120.0;

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
    if (audio.hasOutput())
        std::printf("output peak       : %.3f dBFS  %s\n", outputLevel,
                    outputLevel > -60.0 ? "(audio path is live)" : "(silent: unexpected)");
    else
        std::printf("notes out         : %d  %s\n", g.notesSeen,
                    g.notesSeen > 0 ? "(note path is live)" : "(silent: unexpected)");
    std::printf("flush requests    : %d\n", g.flushRequests);
    std::printf("out events        : %d\n", g.outEventsSeen);
    std::printf("main callbacks    : %d\n", g.callbacksRequested);

    if (gui) gui->destroy(g.plugin);
    g.plugin->stop_processing(g.plugin);
    g.plugin->deactivate(g.plugin);
    g.plugin->destroy(g.plugin);
    entry->deinit();
#if defined(__APPLE__)
    return sizesMatch ? 0 : 1;
#else
    return 0;
#endif
}
