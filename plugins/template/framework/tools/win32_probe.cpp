// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A real Win32 CLAP host: embedding in a parent HWND, pixels, resize (before
// and after parenting), HiDPI scale, hide/show, reopen and teardown, all while
// audio runs on its own thread. The Windows counterpart of x11-probe, with the
// same checks; product-independent.
//
//   win32-probe plugin.clap [capture.ppm] [--no-host-timer]
//
// Last, it unloads the module, reloads it and opens the editor once more.
//
// The editor renders through OpenGL 2. A machine whose only OpenGL is
// Windows' built-in GDI 1.1 renderer (a server or VM without a GPU driver)
// cannot draw it, and set_parent fails here as it would in a host; CI puts
// Mesa's software opengl32.dll beside this executable for that reason.
#ifndef NOMINMAX
#  define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <clap/clap.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "ProbeAudio.h"

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    std::fflush(stdout);
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
    int ticks = 0;
    Host() {
        host.clap_version = CLAP_VERSION_INIT;
        host.host_data = this;
        host.name = "Win32 embedding probe";
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
            [](const clap_host_t* h, clap_id) {
                self(h).timerId = CLAP_INVALID_ID;
                return true;
            }};
        static const clap_host_params_t params{[](const clap_host_t*, clap_param_rescan_flags) {},
                                               [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
                                               [](const clap_host_t* h) { self(h).flushNeeded = true; }};
        static const clap_host_gui_t gui{[](const clap_host_t*) {},
                                         [](const clap_host_t*, uint32_t, uint32_t) { return true; },
                                         [](const clap_host_t*) { return true; },
                                         [](const clap_host_t*) { return true; },
                                         [](const clap_host_t*, bool) {}};
        if (!std::strcmp(id, CLAP_EXT_TIMER_SUPPORT)) return &timers;
        if (!std::strcmp(id, CLAP_EXT_PARAMS)) return &params;
        if (!std::strcmp(id, CLAP_EXT_GUI)) return &gui;
        return nullptr;
    }
    void tick() {
        if (callback.exchange(false)) plugin->on_main_thread(plugin);
        if (!processing && flushNeeded.exchange(false)) {
            auto* params = static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
            clap_input_events_t in{nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; },
                                   [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
                                       return nullptr;
                                   }};
            clap_output_events_t out{nullptr,
                                     [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
            if (params) params->flush(plugin, &in, &out);
        }
        if (timer && timerId != CLAP_INVALID_ID) {
            timer->on_timer(plugin, timerId);
            ++ticks;
        }
    }
};

// Bounded, as a host's loop is: with the editor's own WM_TIMER driving it
// (no host timer) and a software renderer slower than the timer's period,
// a WM_TIMER is always pending again by the time the queue empties, and
// "until the queue is empty" never comes.
void drain() {
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
    MSG msg;
    while (std::chrono::steady_clock::now() < until && PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

// A host's message loop: the plugin's window procedure, and pugl's
// WM_TIMER fallback, run from here.
void pump(Host& host, int count) {
    for (int i = 0; i < count; ++i) {
        drain();
        host.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// The same loop with no plugin left to tick.
void pumpMessages(int count) {
    for (int i = 0; i < count; ++i) {
        drain();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

const wchar_t* kParentClass = L"SuppleWin32ProbeParent";

HWND makeParent(uint32_t w, uint32_t h) {
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DefWindowProcW;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        wc.lpszClassName = kParentClass;
        registered = RegisterClassExW(&wc) != 0;
    }
    // WS_CLIPCHILDREN, as hosts do: the parent must not paint over the
    // plugin's GL surface.
    const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
    RECT r{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(0, kParentClass, L"CLAP Win32 host verification", style, 40, 40, r.right - r.left,
                                r.bottom - r.top, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd) {
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
    }
    return hwnd;
}

void resizeParent(HWND hwnd, uint32_t w, uint32_t h) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    RECT r{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)};
    AdjustWindowRect(&r, style, FALSE);
    SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

HWND editorWindow(HWND parent) { return GetWindow(parent, GW_CHILD); }

bool embedded(HWND parent) {
    HWND child = editorWindow(parent);
    return child && IsWindowVisible(child) && GetParent(child) == parent;
}

bool childSize(HWND parent, uint32_t& w, uint32_t& h) {
    HWND child = editorWindow(parent);
    RECT r{};
    if (!child || !GetClientRect(child, &r)) return false;
    w = static_cast<uint32_t>(r.right - r.left);
    h = static_cast<uint32_t>(r.bottom - r.top);
    return true;
}

// The parent's client area as the user would see it. PrintWindow with
// PW_RENDERFULLCONTENT asks DWM for the composed content, which includes the
// child's GL surface; where that comes back blank (no compositor), the screen
// itself is read instead, which needs the window in front.
bool capture(HWND parent, const char* path, std::vector<uint32_t>* pixels = nullptr) {
    RECT rc{};
    GetClientRect(parent, &rc);
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return false;
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);

    auto varied = [&] {
        const auto* px = static_cast<const uint32_t*>(bits);
        size_t n = 0;
        for (int i = 0; i < w * h; ++i) n += (px[i] & 0xFFFFFF) != (px[0] & 0xFFFFFF);
        return n;
    };
    constexpr UINT kClientOnly = 0x1, kRenderFullContent = 0x2;
    PrintWindow(parent, mem, kClientOnly | kRenderFullContent);
    if (varied() <= 100) {
        SetForegroundWindow(parent);
        POINT origin{0, 0};
        ClientToScreen(parent, &origin);
        BitBlt(mem, 0, 0, w, h, screen, origin.x, origin.y, SRCCOPY);
    }
    const size_t distinct = varied();
    if (pixels) {
        const auto* px = static_cast<const uint32_t*>(bits);
        pixels->assign(px, px + static_cast<size_t>(w) * static_cast<size_t>(h));
        for (auto& pixel : *pixels) pixel &= 0xFFFFFF;
    }

    bool ok = false;
    if (FILE* f = std::fopen(path, "wb")) {
        std::fprintf(f, "P6\n%d %d\n255\n", w, h);
        const auto* px = static_cast<const uint32_t*>(bits);
        std::vector<unsigned char> row(static_cast<size_t>(w) * 3);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                const uint32_t p = px[y * w + x];
                row[static_cast<size_t>(x) * 3 + 0] = static_cast<unsigned char>((p >> 16) & 0xFF);
                row[static_cast<size_t>(x) * 3 + 1] = static_cast<unsigned char>((p >> 8) & 0xFF);
                row[static_cast<size_t>(x) * 3 + 2] = static_cast<unsigned char>(p & 0xFF);
            }
            std::fwrite(row.data(), 1, row.size(), f);
        }
        ok = std::ferror(f) == 0 && distinct > 100;
        std::fclose(f);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return ok;
}

// Count actual window messages before taking another capture: PrintWindow
// can itself provoke a paint, which must not masquerade as a timer repaint.
struct ObservePaint {
    HWND window;
    WNDPROC original;
    int timers = 0, paints = 0;
    static ObservePaint* current;
    explicit ObservePaint(HWND child) : window(child), original(nullptr) {
        current = this;
        original = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(window, GWLP_WNDPROC,
                                                               reinterpret_cast<LONG_PTR>(procedure)));
    }
    ~ObservePaint() {
        SetWindowLongPtrW(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(original));
        current = nullptr;
    }
    static LRESULT CALLBACK procedure(HWND hwnd, UINT message, WPARAM w, LPARAM l) {
        if (message == WM_TIMER) ++current->timers;
        if (message == WM_PAINT) ++current->paints;
        return CallWindowProcW(current->original, hwnd, message, w, l);
    }
};
ObservePaint* ObservePaint::current = nullptr;

void verifyTimerRepaint(Host& host, HWND parent, const char* output) {
    const auto* params = static_cast<const clap_plugin_params_t*>(
        host.plugin->get_extension(host.plugin, CLAP_EXT_PARAMS));
    clap_param_info_t info{};
    bool found = false;
    for (uint32_t i = 0; params && i < params->count(host.plugin); ++i) {
        if (params->get_info(host.plugin, i, &info) && !std::strcmp(info.name, PROBE_PLAIN_PARAM) &&
            !(info.flags & (CLAP_PARAM_IS_READONLY | CLAP_PARAM_IS_HIDDEN)) && info.min_value < info.max_value) {
            found = true;
            break;
        }
    }
    check(found, "a visible writable parameter is available for the repaint check");
    if (!found) return;
    std::vector<uint32_t> before, after;
    const std::string prefix = std::string(output) + ".timer";
    check(capture(parent, (prefix + "-before.ppm").c_str(), &before), "initial timer-check pixels");
    pump(host, 5);
    double value = 0;
    check(params->get_value(host.plugin, info.id, &value), "parameter readable before automation");
    clap_event_param_value_t event{};
    event.header.size = sizeof(event);
    event.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    event.header.type = CLAP_EVENT_PARAM_VALUE;
    event.param_id = info.id;
    event.cookie = info.cookie;
    event.note_id = event.port_index = event.channel = event.key = -1;
    event.value = value == info.max_value ? info.min_value : info.max_value;
    clap_input_events_t in{
        &event, [](const clap_input_events_t*) -> uint32_t { return 1; },
        [](const clap_input_events_t* list, uint32_t) -> const clap_event_header_t* {
            return &static_cast<const clap_event_param_value_t*>(list->ctx)->header;
        }};
    clap_output_events_t out{nullptr,
                            [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
    {
        ObservePaint observe(editorWindow(parent));
        // This reloaded instance has not been activated: flush belongs on
        // the main thread. No audio, meter movement, resize or user input
        // can make the new picture pass on behalf of parameter automation.
        params->flush(host.plugin, &in, &out);
        double updated = 0;
        check(params->get_value(host.plugin, info.id, &updated) && updated == event.value,
              "automation reaches the parameter before the repaint check");
        pump(host, 40);
        check(observe.paints > 0, "parameter automation repaints without a forced expose");
        if (host.refuseTimer) check(observe.timers > 0, "fallback WM_TIMER reaches the editor");
    }
    check(capture(parent, (prefix + "-after.ppm").c_str(), &after), "updated timer-check pixels");
    size_t changed = 0;
    if (before.size() == after.size())
        for (size_t i = 0; i < before.size(); ++i) changed += before[i] != after[i];
    check(changed > 100, "parameter automation changes the displayed editor");
}

// Hosts are per-monitor DPI aware; an unaware process would have Windows
// bitmap-scale the editor behind both our backs.
void becomeDpiAware() {
    using Fn = BOOL(WINAPI*)(HANDLE);
    if (HMODULE user = GetModuleHandleW(L"user32.dll")) {
        if (auto fn = reinterpret_cast<Fn>(
                reinterpret_cast<void (*)(void)>(GetProcAddress(user, "SetProcessDpiAwarenessContext"))))
            fn(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));  // PER_MONITOR_AWARE_V2
    }
}
}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: win32-probe plugin.clap [capture.ppm] [--no-host-timer]\n");
        return 2;
    }
    becomeDpiAware();
    HMODULE lib = LoadLibraryA(argv[1]);
    if (!lib) {
        std::fprintf(stderr, "cannot load %s (error %lu)\n", argv[1], GetLastError());
        return 2;
    }
    auto* entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(lib, "clap_entry"));
    if (!entry || !entry->init(argv[1])) return 2;
    auto* factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory || !factory->get_plugin_count(factory)) return 2;
    const auto* desc = factory->get_plugin_descriptor(factory, 0);
    std::printf("Testing %s %s (%s)\n", desc->name, desc->version, desc->id);

    Host host;
    const char* output = argc > 2 && argv[2][0] != '-' ? argv[2] : "win32-probe.ppm";
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "--no-host-timer")) host.refuseTimer = true;
    host.plugin = factory->create_plugin(factory, &host.host, desc->id);
    if (!host.plugin || !host.plugin->init(host.plugin)) return 2;
    host.gui = static_cast<const clap_plugin_gui_t*>(host.plugin->get_extension(host.plugin, CLAP_EXT_GUI));
    host.timer = static_cast<const clap_plugin_timer_support_t*>(
        host.plugin->get_extension(host.plugin, CLAP_EXT_TIMER_SUPPORT));
    if (!host.gui) return 2;
    supple::probe::Audio buffers;
    if (!buffers.configure(host.plugin, 512)) {
        std::fprintf(stderr, "cannot query the plugin's audio ports\n");
        return 2;
    }
    check(host.gui->is_api_supported(host.plugin, CLAP_WINDOW_API_WIN32, false), "Win32 embedded API");
    check(host.plugin->activate(host.plugin, 48000, 1, 512), "activation");
    host.processing = true;

    std::atomic<bool> run{true};
    std::atomic<bool> audioGood{true};
    std::thread audio([&] {
        if (!host.plugin->start_processing(host.plugin)) {
            audioGood = false;
            return;
        }
        clap_input_events_t in{nullptr, [](const clap_input_events_t*) -> uint32_t { return 0; },
                               [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
                                   return nullptr;
                               }};
        clap_output_events_t out{nullptr,
                                 [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
        clap_process_t p{};
        buffers.bind(p);
        p.in_events = &in;
        p.out_events = &out;
        double phase = 0;
        while (run) {
            buffers.fillInputs(phase);
            if (host.plugin->process(host.plugin, &p) == CLAP_PROCESS_ERROR) audioGood = false;
            if (!buffers.finiteOutputs()) audioGood = false;
            p.steady_time += 512;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        host.plugin->stop_processing(host.plugin);
    });

    for (int cycle = 0; cycle < 3; ++cycle) {
        check(host.gui->create(host.plugin, CLAP_WINDOW_API_WIN32, false), "create/reopen editor");
        uint32_t w = 0, h = 0;
        check(host.gui->get_size(host.plugin, &w, &h) && w && h, "editor dimensions");
        // Sized between create and set_parent, as hosts may (see x11-probe).
        uint32_t preW = 0, preH = 0;
        if (cycle == 1) {
            preW = 1320;
            preH = 840;
            check(host.gui->adjust_size(host.plugin, &preW, &preH) && host.gui->set_size(host.plugin, preW, preH),
                  "set_size before set_parent is honoured");
            check(host.gui->get_size(host.plugin, &w, &h) && w == preW && h == preH, "pre-parent size reported");
        }
        HWND parent = makeParent(w, h);
        check(parent != nullptr, "host parent window");
        clap_window_t window{};
        window.api = CLAP_WINDOW_API_WIN32;
        window.win32 = parent;
        check(host.gui->set_parent(host.plugin, &window), "set actual Win32 parent");
        check(host.gui->show(host.plugin), "show editor");
        pump(host, 25);
        check(embedded(parent), "visible child belongs to host parent");
        uint32_t cw = 0, ch = 0;
        check(childSize(parent, cw, ch) && cw == w && ch == h, "editor window matches its reported size");
        if (cycle == 1) {
            uint32_t realW = 0, realH = 0;
            check(host.gui->get_size(host.plugin, &realW, &realH) && realW == preW && realH == preH,
                  "pre-parent size survives realizing");
        }
        if (cycle == 0) check(capture(parent, output), "embedded editor draws nonblank pixels");

        w = 1320;
        h = 840;
        check(host.gui->adjust_size(host.plugin, &w, &h) && host.gui->set_size(host.plugin, w, h), "resize editor");
        resizeParent(parent, w, h);
        pump(host, 15);
        uint32_t actualW = 0, actualH = 0;
        check(host.gui->get_size(host.plugin, &actualW, &actualH) && actualW == w && actualH == h,
              "resize dimensions match");
        check(childSize(parent, cw, ch) && cw == w && ch == h, "editor window follows the resize");
        if (cycle == 0) {
            const std::string resized = std::string(output) + ".resized.ppm";
            check(capture(parent, resized.c_str()), "resized editor draws nonblank pixels");
        }
        if (cycle == 1) {
            check(host.gui->hide(host.plugin), "hide editor");
            check(host.gui->show(host.plugin), "show after hide");
            pump(host, 5);
            check(embedded(parent), "editor visible again after show");
        }
        if (cycle == 2) {
            // A host on a 200% display: sizes are physical pixels, so the
            // editor must double, and come back when the scale does.
            uint32_t w1 = 0, h1 = 0, w2 = 0, h2 = 0, w3 = 0, h3 = 0;
            host.gui->get_size(host.plugin, &w1, &h1);
            check(host.gui->set_scale(host.plugin, 2.0), "set_scale(2) is honoured");
            host.gui->get_size(host.plugin, &w2, &h2);
            check(w2 + 1 >= 2 * w1 && w2 <= 2 * w1 + 1 && h2 + 1 >= 2 * h1 && h2 <= 2 * h1 + 1,
                  "set_scale(2) doubles the editor");
            resizeParent(parent, w2, h2);
            pump(host, 5);
            check(childSize(parent, cw, ch) && cw == w2 && ch == h2, "editor window doubles with the scale");
            const std::string scaled = std::string(output) + ".scale2.ppm";
            capture(parent, scaled.c_str());
            host.gui->set_scale(host.plugin, 1.0);
            host.gui->get_size(host.plugin, &w3, &h3);
            check(w3 == w1 && h3 == h1, "set_scale(1) restores it");
            resizeParent(parent, w3, h3);
            pump(host, 3);
        }
        host.tick();
        host.gui->destroy(host.plugin);
        pump(host, 5);
        check(editorWindow(parent) == nullptr, "editor window destroyed with the editor");
        DestroyWindow(parent);
        pump(host, 2);
        check(host.timerId == CLAP_INVALID_ID, "GUI teardown unregisters timer");
    }
    run = false;
    audio.join();
    host.processing = false;
    check(audioGood, "concurrent audio survives resize/reopen/teardown");
    if (!host.refuseTimer) check(host.ticks > 0, "host timer drives GUI");
    host.plugin->deactivate(host.plugin);
    host.plugin->destroy(host.plugin);
    entry->deinit();
    FreeLibrary(lib);

    // Hosts unload plugins (a rescan, closing a project) and load them
    // again. Anything of ours left behind -- a window class whose procedure
    // was in the unloaded image, a timer still posting to it -- crashes the
    // message pump below rather than failing quietly.
    pumpMessages(10);
    lib = LoadLibraryA(argv[1]);
    check(lib != nullptr, "reloads after FreeLibrary");
    if (lib) {
        entry = reinterpret_cast<const clap_plugin_entry_t*>(GetProcAddress(lib, "clap_entry"));
        check(entry && entry->init(argv[1]), "entry initialises again");
        factory = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        // The first module's descriptor went with it: ask the new one.
        const auto* desc2 = factory->get_plugin_descriptor(factory, 0);
        Host again;
        again.refuseTimer = host.refuseTimer;
        again.plugin = factory->create_plugin(factory, &again.host, desc2->id);
        check(again.plugin && again.plugin->init(again.plugin), "plugin initialises again");
        again.gui = static_cast<const clap_plugin_gui_t*>(again.plugin->get_extension(again.plugin, CLAP_EXT_GUI));
        again.timer = static_cast<const clap_plugin_timer_support_t*>(
            again.plugin->get_extension(again.plugin, CLAP_EXT_TIMER_SUPPORT));
        uint32_t w = 0, h = 0;
        check(again.gui->create(again.plugin, CLAP_WINDOW_API_WIN32, false) && again.gui->get_size(again.plugin, &w, &h),
              "editor opens after a reload");
        HWND parent = makeParent(w, h);
        clap_window_t window{};
        window.api = CLAP_WINDOW_API_WIN32;
        window.win32 = parent;
        check(again.gui->set_parent(again.plugin, &window) && again.gui->show(again.plugin), "editor embeds after a reload");
        pump(again, 15);
        check(embedded(parent), "reloaded editor is visible");
        verifyTimerRepaint(again, parent, output);
        again.gui->destroy(again.plugin);
        DestroyWindow(parent);
        again.plugin->destroy(again.plugin);
        entry->deinit();
        FreeLibrary(lib);
        pumpMessages(10);
        check(true, "message loop survives the second unload");
    }
    std::printf("%d failures\n", failures);
    return failures ? 1 : 0;
}
