// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A real X11 CLAP host: embedding, resize, reopen, hot teardown and pixels.
// Product-independent. Build with CLAP headers, -lX11 -ldl -pthread.
#include <clap/clap.h>
#include <clap/ext/track-info.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <dlfcn.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) ++failures;
}
struct Host {
    clap_host_t host{};
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_gui_t* gui = nullptr;
    const clap_plugin_timer_support_t* timer = nullptr;
    clap_id timerId = CLAP_INVALID_ID;
    std::atomic<bool> callback{false};
    std::atomic<bool> flushNeeded{false};
    bool processing = false;
    bool refuseTimer = false;
    const char* track = "Probe track";
    int ticks = 0;
    Host() {
        host.clap_version = CLAP_VERSION_INIT;
        host.host_data = this;
        host.name = "Linux embedding probe";
        host.vendor = "Plugin development";
        host.url = "";
        host.version = "1";
        host.get_extension = extension;
        host.request_restart = [](const clap_host_t*) {};
        host.request_process = [](const clap_host_t*) {};
        host.request_callback = [](const clap_host_t* h) { self(h).callback = true; };
    }
    static Host& self(const clap_host_t* h) { return *static_cast<Host*>(h->host_data); }
    static const void* CLAP_ABI extension(const clap_host_t*, const char* id) {
        static const clap_host_timer_support_t timers{
            [](const clap_host_t* h, uint32_t, clap_id* id) {
                if (self(h).refuseTimer) return false;
                *id = self(h).timerId = 1;
                return true;
            },
            [](const clap_host_t* h, clap_id) { self(h).timerId = CLAP_INVALID_ID; return true; }};
        static const clap_host_params_t params{
            [](const clap_host_t*, clap_param_rescan_flags) {},
            [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
            [](const clap_host_t* h) { self(h).flushNeeded = true; }};
        static const clap_host_gui_t gui{
            [](const clap_host_t*) {},
            [](const clap_host_t*, uint32_t, uint32_t) { return true; },
            [](const clap_host_t*) { return true; },
            [](const clap_host_t*) { return true; },
            [](const clap_host_t*, bool) {}};
        static const clap_host_track_info_t track{
            [](const clap_host_t* h, clap_track_info_t* info) {
                *info = {};
                info->flags = CLAP_TRACK_INFO_HAS_TRACK_NAME;
                std::snprintf(info->name, sizeof(info->name), "%s", self(h).track);
                return true;
            }};
        if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &timers;
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
        if (!std::strcmp(id, CLAP_EXT_GUI)) return &gui;
        if (!std::strcmp(id, CLAP_EXT_TRACK_INFO)) return &track;
        return nullptr;
    }
    void tick() {
        if (callback.exchange(false)) plugin->on_main_thread(plugin);
        if (!processing && flushNeeded.exchange(false)) {
            auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
            clap_input_events_t in{nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; },
                [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* { return nullptr; }};
            clap_output_events_t out{nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
            if (params) params->flush(plugin, &in, &out);
        }
        if (timer && timerId != CLAP_INVALID_ID) { timer->on_timer(plugin, timerId); ++ticks; }
    }
};
void pump(Display* d, Host& host, int count) {
    for (int i = 0; i < count; ++i) {
        while (XPending(d)) { XEvent ev; XNextEvent(d, &ev); }
        host.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}
bool embedded(Display* d, Window parent) {
    Window root = 0, up = 0, *children = nullptr;
    unsigned n = 0;
    if (!XQueryTree(d, parent, &root, &up, &children, &n)) return false;
    bool mapped = false;
    for (unsigned i = 0; i < n; ++i) {
        XWindowAttributes a{};
        if (XGetWindowAttributes(d, children[i], &a) && a.map_state == IsViewable) mapped = true;
    }
    if (children) XFree(children);
    return mapped;
}
bool capture(Display* d, Window parent, const char* path) {
    XWindowAttributes a{};
    if (!XGetWindowAttributes(d, parent, &a)) return false;
    XImage* image = XGetImage(d, parent, 0, 0, a.width, a.height, AllPlanes, ZPixmap);
    if (!image) return false;
    FILE* f = std::fopen(path, "wb");
    if (!f) { XDestroyImage(image); return false; }
    std::fprintf(f, "P6\n%d %d\n255\n", a.width, a.height);
    unsigned long first = XGetPixel(image, 0, 0);
    size_t varied = 0;
    auto channel = [](unsigned long pixel, unsigned long mask) {
        if (!mask) return static_cast<unsigned char>(0);
        while (!(mask & 1)) { mask >>= 1; pixel >>= 1; }
        return static_cast<unsigned char>((pixel & mask) * 255 / mask);
    };
    for (int y = 0; y < a.height; ++y) for (int x = 0; x < a.width; ++x) {
        const unsigned long p = XGetPixel(image, x, y);
        varied += p != first;
        unsigned char rgb[]{channel(p, image->red_mask), channel(p, image->green_mask), channel(p, image->blue_mask)};
        std::fwrite(rgb, 1, 3, f);
    }
    const bool ok = std::ferror(f) == 0 && varied > 100;
    std::fclose(f);
    XDestroyImage(image);
    return ok;
}
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: linux-host-probe plugin.clap [capture.ppm] [--no-host-timer] [--companion]\n"); return 2; }
    Display* display = XOpenDisplay(nullptr);
    if (!display) { std::fprintf(stderr, "Cannot open X11 display\n"); return 2; }
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { std::fprintf(stderr, "%s\n", dlerror()); return 2; }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    if (!entry || !entry->init(argv[1])) return 2;
    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory || !factory->get_plugin_count(factory)) return 2;
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    std::printf("Testing %s %s (%s)\n", desc->name, desc->version, desc->id);
    Host host;
    host.track = "Probe Drums";
    Host companion;
    companion.track = "Probe Vocal";
    bool withCompanion = false;
    const char* output = argc > 2 && argv[2][0] != '-' ? argv[2] : "/tmp/plugin-host.ppm";
    for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], "--no-host-timer")) host.refuseTimer = true;
    for (int i = 2; i < argc; ++i) if (!std::strcmp(argv[i], "--companion")) withCompanion = true;
    host.plugin = factory->create_plugin(factory, &host.host, desc->id);
    if (!host.plugin || !host.plugin->init(host.plugin)) return 2;
    if (withCompanion) {
        companion.plugin = factory->create_plugin(factory, &companion.host, desc->id);
        if (!companion.plugin || !companion.plugin->init(companion.plugin)) return 2;
        std::puts("Created companion instance on Probe Vocal without an editor");
    }
    host.gui = static_cast<const clap_plugin_gui_t*>(host.plugin->get_extension(host.plugin, CLAP_EXT_GUI));
    host.timer = static_cast<const clap_plugin_timer_support_t*>(host.plugin->get_extension(host.plugin, CLAP_EXT_TIMER_SUPPORT));
    if (!host.gui) return 2;
    check(host.gui->is_api_supported(host.plugin, CLAP_WINDOW_API_X11, false), "X11 embedded API");
    check(host.plugin->activate(host.plugin, 48000, 1, 512), "activation");
    host.processing = true;
    std::atomic<bool> run{true};
    std::atomic<bool> audioGood{true};
    std::thread audio([&] {
        if (!host.plugin->start_processing(host.plugin)) { audioGood = false; return; }
        float left[512]{}, right[512]{};
        float* data[]{left, right};
        clap_audio_buffer_t buffer{};
        buffer.data32 = data; buffer.channel_count = 2;
        clap_input_events_t in{nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; },
            [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* { return nullptr; }};
        clap_output_events_t out{nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
        clap_process_t p{};
        p.frames_count = 512; p.audio_inputs = &buffer; p.audio_outputs = &buffer;
        p.audio_inputs_count = p.audio_outputs_count = 1; p.in_events = &in; p.out_events = &out;
        double phase = 0;
        while (run) {
            for (int i = 0; i < 512; ++i) { left[i] = 0.2f * std::sin(phase); right[i] = 0.17f * std::cos(phase * 1.37); phase += 0.0576; }
            if (host.plugin->process(host.plugin, &p) == CLAP_PROCESS_ERROR) audioGood = false;
            for (int i = 0; i < 512; ++i) if (!std::isfinite(left[i]) || !std::isfinite(right[i])) audioGood = false;
            p.steady_time += 512;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        host.plugin->stop_processing(host.plugin);
    });
    for (int cycle = 0; cycle < 3; ++cycle) {
        Window parent = XCreateSimpleWindow(display, DefaultRootWindow(display), 40, 40, 1100, 720, 0, 0, 0);
        XStoreName(display, parent, "CLAP Linux host verification");
        XMapWindow(display, parent);
        XSync(display, False);
        check(host.gui->create(host.plugin, CLAP_WINDOW_API_X11, false), "create/reopen editor");
        uint32_t w = 0, h = 0;
        check(host.gui->get_size(host.plugin, &w, &h) && w && h, "editor dimensions");
        XResizeWindow(display, parent, w, h);
        clap_window_t window{}; window.api = CLAP_WINDOW_API_X11; window.x11 = parent;
        check(host.gui->set_parent(host.plugin, &window), "set actual X11 parent");
        check(host.gui->show(host.plugin), "show editor");
        pump(display, host, 25);
        if (companion.plugin) companion.tick();
        check(embedded(display, parent), "mapped child belongs to host parent");
        w = 1320; h = 840;
        check(host.gui->adjust_size(host.plugin, &w, &h) && host.gui->set_size(host.plugin, w, h), "resize editor");
        XResizeWindow(display, parent, w, h);
        pump(display, host, 15);
        uint32_t actualW = 0, actualH = 0;
        check(host.gui->get_size(host.plugin, &actualW, &actualH) && actualW == w && actualH == h, "resize dimensions match");
        if (cycle == 0) check(capture(display, parent, output), "embedded editor draws nonblank pixels");
        if (cycle == 1) { check(host.gui->hide(host.plugin), "hide editor"); check(host.gui->show(host.plugin), "show after hide"); pump(display, host, 5); }
        host.tick();
        host.gui->destroy(host.plugin);
        pump(display, host, 5);
        XDestroyWindow(display, parent);
        XSync(display, False);
        check(host.timerId == CLAP_INVALID_ID, "GUI teardown unregisters timer");
    }
    run = false;
    audio.join();
    host.processing = false;
    check(audioGood, "concurrent audio survives resize/reopen/teardown");
    if (!host.refuseTimer) check(host.ticks > 0, "host timer drives GUI");
    else std::puts("NOTE: no-host-timer proves lifecycle only; repaint liveness requires host integration.");
    host.plugin->deactivate(host.plugin);
    host.plugin->destroy(host.plugin);
    if (companion.plugin) companion.plugin->destroy(companion.plugin);
    entry->deinit();
    dlclose(lib);
    XCloseDisplay(display);
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
