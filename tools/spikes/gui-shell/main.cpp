// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Temporary XWayland/SDL/CLAP integration measurement.
#include <GL/gl.h>
#include <SDL3/SDL.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <clap/clap.h>
#include <clap/ext/gui.h>
#include <clap/ext/params.h>
#include <clap/ext/timer-support.h>
#include <dlfcn.h>
#include <nanovg.h>
#define NANOVG_GL3 1
#include <nanovg_gl.h>
#include <supple/product.h>

#include <sys/resource.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace supple::spike {
namespace {
using Clock = std::chrono::steady_clock;
using Milli = std::chrono::duration<double, std::milli>;

struct Options {
    std::string mode;
    std::string plugin;
    int frames = 60;
    int seconds = 4;
    int width = 1920;
    int height = 1080;
};

bool parse(int argc, char** argv, Options& o) {
    if (argc < 2) return false;
    o.mode = argv[1];
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc)
            o.frames = std::max(1, std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--seconds") == 0 && i + 1 < argc)
            o.seconds = std::max(1, std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--size") == 0 && i + 1 < argc) {
            if (std::sscanf(argv[++i], "%dx%d", &o.width, &o.height) != 2) return false;
        } else if (o.mode == "--embed" && o.plugin.empty())
            o.plugin = argv[i];
        else
            return false;
    }
    return (o.mode == "--draw" || o.mode == "--idle" || o.mode == "--text" ||
            (o.mode == "--embed" && !o.plugin.empty())) &&
           o.width > 0 && o.height > 0;
}

struct WindowState {
    SDL_Window* window = nullptr;
    SDL_GLContext context = nullptr;
    NVGcontext* vg = nullptr;
    int font = -1;
    Display* xdisplay = nullptr;
    Window xid = 0;

    bool open(const Options& o) {
        SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "x11");
        if (!SDL_Init(SDL_INIT_VIDEO)) return fail("SDL_Init");
        if (!SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3) ||
            !SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3) ||
            !SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE))
            return fail("SDL_GL_SetAttribute");
        window = SDL_CreateWindow(product::kName.data(), o.width, o.height,
                                  SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
        if (!window) return fail("SDL_CreateWindow");
        context = SDL_GL_CreateContext(window);
        if (!context) return fail("SDL_GL_CreateContext");
        SDL_GL_SetSwapInterval(0);
        vg = nvgCreateGL3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
        if (!vg) return fail("nvgCreateGL3");
        font = nvgCreateFont(vg, "ui", "/usr/share/fonts/noto/NotoSans-Regular.ttf");
        if (font < 0) std::fprintf(stderr, "NotoSans font unavailable; text may not draw\n");
        SDL_PropertiesID props = SDL_GetWindowProperties(window);
        xid =
            static_cast<Window>(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
        xdisplay = XOpenDisplay(nullptr);
        if (!xid || !xdisplay) return fail("X11 display or SDL XID");
        std::printf(
            "video_driver=%s XID=0x%lx display_scale=%.3f window_scale=%.3f\n",
            SDL_GetCurrentVideoDriver(), xid,
            static_cast<double>(SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window))),
            static_cast<double>(SDL_GetWindowDisplayScale(window)));
        std::printf("GL_VENDOR=%s\nGL_RENDERER=%s\nGL_VERSION=%s\n",
                    reinterpret_cast<const char*>(glGetString(GL_VENDOR)),
                    reinterpret_cast<const char*>(glGetString(GL_RENDERER)),
                    reinterpret_cast<const char*>(glGetString(GL_VERSION)));
        return true;
    }
    static bool fail(const char* step) {
        std::fprintf(stderr, "%s failed: %s\n", step, SDL_GetError());
        return false;
    }
    ~WindowState() {
        if (xdisplay) XCloseDisplay(xdisplay);
        if (vg) nvgDeleteGL3(vg);
        if (context) SDL_GL_DestroyContext(context);
        if (window) SDL_DestroyWindow(window);
        SDL_Quit();
    }
    void begin() {
        int pw = 0, ph = 0, w = 0, h = 0;
        SDL_GetWindowSizeInPixels(window, &pw, &ph);
        SDL_GetWindowSize(window, &w, &h);
        glViewport(0, 0, pw, ph);
        glClearColor(0.055f, 0.065f, 0.09f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        nvgBeginFrame(vg, static_cast<float>(w), static_cast<float>(h),
                      w ? static_cast<float>(pw) / static_cast<float>(w) : 1.0f);
    }
    void end() {
        nvgEndFrame(vg);
        SDL_GL_SwapWindow(window);
    }
};

void label(WindowState& ws, float x, float y, float size, const char* str) {
    nvgFontFaceId(ws.vg, ws.font);
    nvgFontSize(ws.vg, size);
    nvgFillColor(ws.vg, nvgRGB(225, 234, 244));
    nvgText(ws.vg, x, y, str, nullptr);
}

std::vector<float> waveData() {
    std::vector<float> data(64 * 8 * 1500);
    std::uint32_t seed = 0x234abcdu;
    for (float& v : data) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        v = static_cast<float>(seed & 65535u) / 32767.5f - 1.0f;
    }
    return data;
}

void drawArrange(WindowState& ws, const std::vector<float>& data) {
    ws.begin();
    int width = 0, height = 0;
    SDL_GetWindowSize(ws.window, &width, &height);
    const float fw = static_cast<float>(width);
    nvgBeginPath(ws.vg);
    nvgRect(ws.vg, 0, 0, fw, 65);
    nvgFillColor(ws.vg, nvgRGB(31, 43, 61));
    nvgFill(ws.vg);
    label(ws, 22, 29, 17, "Arrange view / 64 tracks / 8 items per track");
    for (int i = 0; i < 20; ++i) {
        char tick[24];
        std::snprintf(tick, sizeof(tick), "%d", i * 4 + 1);
        label(ws, 215 + i * 80.0f, 55, 10, tick);
    }
    nvgSave(ws.vg);
    nvgScissor(ws.vg, 0, 65, fw, static_cast<float>(height - 65));
    for (int row = 0; row < 64; ++row) {
        const float y = 65 + row * 38.0f;
        nvgBeginPath(ws.vg);
        nvgRect(ws.vg, 0, y, fw, 37);
        nvgFillColor(ws.vg, row % 2 ? nvgRGB(19, 28, 43) : nvgRGB(23, 33, 48));
        nvgFill(ws.vg);
        char name[32];
        std::snprintf(name, sizeof(name), "Track %02d", row + 1);
        label(ws, 12, y + 23, 12, name);
        for (int b = 0; b < 3; ++b) {
            nvgBeginPath(ws.vg);
            nvgRoundedRect(ws.vg, 112 + b * 28.0f, y + 9, 22, 18, 3);
            nvgFillColor(ws.vg, nvgRGB(71, 92, 117));
            nvgFill(ws.vg);
        }
        for (int item = 0; item < 8; ++item) {
            const float x = 215 + item * 197.0f;
            nvgBeginPath(ws.vg);
            nvgRoundedRect(ws.vg, x, y + 4, 184, 30, 3);
            nvgFillColor(ws.vg, nvgRGB(38, 67, 88));
            nvgFill(ws.vg);
            nvgBeginPath(ws.vg);
            const float* samples = data.data() + (row * 8 + item) * 1500;
            for (int point = 0; point < 1500; ++point) {
                const float sx = x + 2 + 180.0f * static_cast<float>(point) / 1499.0f;
                const float sy = y + 19 + samples[point] * 10.0f;
                if (point == 0)
                    nvgMoveTo(ws.vg, sx, sy);
                else
                    nvgLineTo(ws.vg, sx, sy);
            }
            nvgStrokeColor(ws.vg, nvgRGBA(115, 212, 227, 210));
            nvgStrokeWidth(ws.vg, 0.65f);
            nvgStroke(ws.vg);
        }
    }
    nvgRestore(ws.vg);
    ws.end();
}

void drawFx(WindowState& ws) {
    ws.begin();
    int width = 0;
    SDL_GetWindowSize(ws.window, &width, nullptr);
    nvgBeginPath(ws.vg);
    nvgRect(ws.vg, 0, 0, static_cast<float>(width), 90);
    nvgFillColor(ws.vg, nvgRGB(31, 43, 61));
    nvgFill(ws.vg);
    label(ws, 25, 39, 23, "FX chain / embedded CLAP editor");
    label(ws, 25, 69, 12, "SDL3 X11 parent + X11 child container");
    ws.end();
}

double cpuSeconds() {
    rusage usage{};
    getrusage(RUSAGE_SELF, &usage);
    return usage.ru_utime.tv_sec + usage.ru_utime.tv_usec * 1e-6 + usage.ru_stime.tv_sec +
           usage.ru_stime.tv_usec * 1e-6;
}

void printStats(const char* labelText, std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto p95 = static_cast<std::size_t>(std::ceil(0.95 * values.size())) - 1;
    std::printf("%s n=%zu median=%.3fms p95=%.3fms max=%.3fms\n", labelText, values.size(),
                values[values.size() / 2], values[p95], values.back());
}

int runDraw(WindowState& ws, const Options& o) {
    const auto data = waveData();
    std::vector<double> cpu, complete;
    cpu.reserve(o.frames);
    complete.reserve(o.frames);
    for (int i = 0; i < o.frames; ++i) {
        SDL_Event event{};
        while (SDL_PollEvent(&event))
            if (event.type == SDL_EVENT_QUIT) return 0;
        const auto start = Clock::now();
        const double cpuStart = cpuSeconds();
        drawArrange(ws, data);
        const double cpuEnd = cpuSeconds();
        glFinish();
        const auto finished = Clock::now();
        cpu.push_back((cpuEnd - cpuStart) * 1000.0);
        complete.push_back(Milli(finished - start).count());
    }
    printStats("frame_process_cpu", cpu);
    printStats("frame_glfinish_wall", complete);
    return 0;
}

int runIdle(WindowState& ws, const Options& o) {
    drawFx(ws);
    const double before = cpuSeconds();
    const auto until = Clock::now() + std::chrono::seconds(o.seconds);
    int events = 0, redraws = 0;
    while (Clock::now() < until) {
        SDL_Event event{};
        if (!SDL_WaitEventTimeout(&event, 250)) continue;
        if (event.type == SDL_EVENT_QUIT) break;
        ++events;
        if (event.type == SDL_EVENT_WINDOW_EXPOSED || event.type == SDL_EVENT_WINDOW_RESIZED) {
            drawFx(ws);
            ++redraws;
        }
    }
    std::printf("idle_seconds=%d process_cpu_seconds=%.6f events=%d redraws=%d\n", o.seconds,
                cpuSeconds() - before, events, redraws);
    return 0;
}

int runText(WindowState& ws, const Options& o) {
    const bool started = SDL_StartTextInput(ws.window);
    std::printf("SDL_StartTextInput=%s\n", started ? "true" : "false");
    if (!started) return 1;
    int textEvents = 0;
    const auto until = Clock::now() + std::chrono::seconds(o.seconds);
    while (Clock::now() < until) {
        SDL_Event event{};
        if (SDL_WaitEventTimeout(&event, 100) && event.type == SDL_EVENT_TEXT_INPUT) {
            ++textEvents;
            std::printf("text_input=%s\n", event.text.text);
        }
    }
    SDL_StopTextInput(ws.window);
    std::printf("text_events=%d (no synthetic input is generated by this mode)\n", textEvents);
    return 0;
}
}  // namespace
}  // namespace supple::spike

namespace supple::spike {
namespace {
struct PluginHost {
    clap_host_t host{};
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_gui_t* gui = nullptr;
    const clap_plugin_timer_support_t* timer = nullptr;
    std::atomic<bool> callbackRequested{false};
    std::atomic<bool> flushRequested{false};
    clap_id timerId = CLAP_INVALID_ID;
    std::uint32_t timerMs = 16;
    int ticks = 0;
    std::atomic<bool> audioRunning{false};
    std::atomic<bool> audioOk{true};
    std::thread audioThread;
    bool activated = false;
    bool guiCreated = false;
    void* library = nullptr;
    const clap_plugin_entry_t* entry = nullptr;

    PluginHost() {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "SDL GUI shell spike";
        host.vendor = "Supple Audio";
        host.url = "";
        host.version = "0";
        host.get_extension = getExtension;
        host.request_restart = [](const clap_host_t*) {};
        host.request_process = [](const clap_host_t*) {};
        host.request_callback = [](const clap_host_t* h) {
            self(h).callbackRequested.store(true, std::memory_order_release);
        };
    }
    static PluginHost& self(const clap_host_t* h) {
        return *static_cast<PluginHost*>(h->host_data);
    }
    static const void* CLAP_ABI getExtension(const clap_host_t*, const char* id) {
        static const clap_host_timer_support_t timers{
            [](const clap_host_t* h, std::uint32_t ms, clap_id* idOut) {
                auto& s = self(h);
                s.timerMs = std::max(1u, ms);
                *idOut = s.timerId = 1;
                return true;
            },
            [](const clap_host_t* h, clap_id id) {
                auto& s = self(h);
                if (id != s.timerId) return false;
                s.timerId = CLAP_INVALID_ID;
                return true;
            }};
        static const clap_host_params_t params{
            [](const clap_host_t*, clap_param_rescan_flags) {},
            [](const clap_host_t*, clap_id, clap_param_clear_flags) {},
            [](const clap_host_t* h) {
                self(h).flushRequested.store(true, std::memory_order_release);
            }};
        static const clap_host_gui_t guiHost{
            [](const clap_host_t*) {},
            [](const clap_host_t*, std::uint32_t, std::uint32_t) { return true; },
            [](const clap_host_t*) { return true; }, [](const clap_host_t*) { return true; },
            [](const clap_host_t*, bool) {}};
        if (std::strcmp(id, CLAP_EXT_TIMER_SUPPORT) == 0) return &timers;
        if (std::strcmp(id, CLAP_EXT_PARAMS) == 0) return &params;
        if (std::strcmp(id, CLAP_EXT_GUI) == 0) return &guiHost;
        return nullptr;
    }
    static std::uint32_t CLAP_ABI noEvents(const clap_input_events_t*) { return 0; }
    static const clap_event_header_t* CLAP_ABI noEventAt(const clap_input_events_t*,
                                                         std::uint32_t) {
        return nullptr;
    }
    static bool CLAP_ABI acceptEvent(const clap_output_events_t*, const clap_event_header_t*) {
        return true;
    }
    void tick() {
        if (callbackRequested.exchange(false, std::memory_order_acq_rel) && plugin)
            plugin->on_main_thread(plugin);
        // A stopped transport uses params.flush. Here process is running, so
        // process() drains the editor-to-host queue; do not call flush concurrently.
        flushRequested.store(false, std::memory_order_relaxed);
        if (timer && timerId != CLAP_INVALID_ID && plugin) {
            timer->on_timer(plugin, timerId);
            ++ticks;
        }
    }
    bool open(const std::string& path) {
        library = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!library) {
            std::fprintf(stderr, "dlopen: %s\n", dlerror());
            return false;
        }
        entry = static_cast<const clap_plugin_entry_t*>(dlsym(library, "clap_entry"));
        if (!entry || !entry->init(path.c_str())) return false;
        const auto* factory =
            static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory || factory->get_plugin_count(factory) == 0) return false;
        const auto* descriptor = factory->get_plugin_descriptor(factory, 0);
        std::printf("plugin=%s id=%s\n", descriptor->name, descriptor->id);
        plugin = factory->create_plugin(factory, &host, descriptor->id);
        if (!plugin || !plugin->init(plugin)) return false;
        gui = static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
        timer = static_cast<const clap_plugin_timer_support_t*>(
            plugin->get_extension(plugin, CLAP_EXT_TIMER_SUPPORT));
        if (!gui || !gui->is_api_supported(plugin, CLAP_WINDOW_API_X11, false)) return false;
        activated = plugin->activate(plugin, 48000.0, 1, 512);
        return activated;
    }
    bool startAudio() {
        if (!plugin->start_processing(plugin)) return false;
        audioRunning.store(true, std::memory_order_release);
        audioThread = std::thread([this] {
            float left[512]{};
            float right[512]{};
            float* channels[]{left, right};
            clap_audio_buffer_t buffer{};
            buffer.data32 = channels;
            buffer.channel_count = 2;
            const clap_input_events_t input{nullptr, noEvents, noEventAt};
            const clap_output_events_t output{nullptr, acceptEvent};
            clap_process_t p{};
            p.frames_count = 512;
            p.audio_inputs = &buffer;
            p.audio_outputs = &buffer;
            p.audio_inputs_count = 1;
            p.audio_outputs_count = 1;
            p.in_events = &input;
            p.out_events = &output;
            while (audioRunning.load(std::memory_order_acquire)) {
                std::fill(std::begin(left), std::end(left), 0.0f);
                std::fill(std::begin(right), std::end(right), 0.0f);
                if (plugin->process(plugin, &p) == CLAP_PROCESS_ERROR)
                    audioOk.store(false, std::memory_order_relaxed);
                p.steady_time += 512;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });
        return true;
    }
    void closeGui() {
        if (guiCreated) {
            gui->hide(plugin);
            gui->destroy(plugin);
            guiCreated = false;
        }
    }
    ~PluginHost() {
        audioRunning.store(false, std::memory_order_release);
        if (audioThread.joinable()) audioThread.join();
        if (plugin && activated) plugin->stop_processing(plugin);
        closeGui();
        if (plugin && activated) plugin->deactivate(plugin);
        if (plugin) plugin->destroy(plugin);
        if (entry) entry->deinit();
        if (library) dlclose(library);
    }
};

bool mappedChild(Display* display, Window parent, unsigned& count) {
    Window root = 0, outer = 0, *children = nullptr;
    count = 0;
    if (!XQueryTree(display, parent, &root, &outer, &children, &count)) return false;
    bool mapped = false;
    for (unsigned i = 0; i < count; ++i) {
        XWindowAttributes attributes{};
        if (XGetWindowAttributes(display, children[i], &attributes) &&
            attributes.map_state == IsViewable)
            mapped = true;
    }
    if (children) XFree(children);
    return mapped;
}

bool capture(Display* display, Window xid, const char* path) {
    XWindowAttributes attr{};
    if (!XGetWindowAttributes(display, xid, &attr)) return false;
    XImage* image = XGetImage(display, xid, 0, 0, static_cast<unsigned>(attr.width),
                              static_cast<unsigned>(attr.height), AllPlanes, ZPixmap);
    if (!image) return false;
    FILE* file = std::fopen(path, "wb");
    if (!file) {
        XDestroyImage(image);
        return false;
    }
    std::fprintf(file, "P6\n%d %d\n255\n", attr.width, attr.height);
    for (int y = 0; y < attr.height; ++y) {
        for (int x = 0; x < attr.width; ++x) {
            const unsigned long p = XGetPixel(image, x, y);
            const unsigned char rgb[3] = {static_cast<unsigned char>((p & image->red_mask) >> 16),
                                          static_cast<unsigned char>((p & image->green_mask) >> 8),
                                          static_cast<unsigned char>(p & image->blue_mask)};
            std::fwrite(rgb, 1, 3, file);
        }
    }
    std::fclose(file);
    XDestroyImage(image);
    return true;
}

int runEmbed(WindowState& ws, const Options& o) {
    PluginHost host;
    if (!host.open(o.plugin) || !host.startAudio()) return 1;
    const int initialW = 1100, initialH = 760;
    SDL_SetWindowSize(ws.window, initialW, initialH);
    drawFx(ws);
    {
        int actualW = 0, actualH = 0;
        SDL_GetWindowSize(ws.window, &actualW, &actualH);
        XWindowAttributes attr{};
        XGetWindowAttributes(ws.xdisplay, ws.xid, &attr);
        std::printf("initial_window requested=%dx%d SDL=%dx%d X11=%dx%d\n", initialW, initialH,
                    actualW, actualH, attr.width, attr.height);
    }
    Window container =
        XCreateSimpleWindow(ws.xdisplay, ws.xid, 20, 105, static_cast<unsigned>(initialW - 40),
                            static_cast<unsigned>(initialH - 125), 0, 0, 0);
    XMapWindow(ws.xdisplay, container);
    XSync(ws.xdisplay, False);
    bool okay = true;
    int mappedCycles = 0;
    int resizeSuccess = 0;
    for (int cycle = 0; cycle < 3; ++cycle) {
        if (!host.gui->create(host.plugin, CLAP_WINDOW_API_X11, false)) {
            okay = false;
            break;
        }
        host.guiCreated = true;
        clap_window_t parent{};
        parent.api = CLAP_WINDOW_API_X11;
        parent.x11 = container;
        if (!host.gui->set_parent(host.plugin, &parent) || !host.gui->show(host.plugin)) {
            okay = false;
            break;
        }
        const auto until = Clock::now() + std::chrono::milliseconds(600);
        while (Clock::now() < until) {
            SDL_Event event{};
            SDL_WaitEventTimeout(&event, 16);
            host.tick();
        }
        XSync(ws.xdisplay, False);
        unsigned count = 0;
        const bool mapped = mappedChild(ws.xdisplay, container, count);
        mappedCycles += mapped ? 1 : 0;
        std::printf("embed_cycle=%d child_count=%u mapped=%s\n", cycle + 1, count,
                    mapped ? "true" : "false");
        if (!mapped) okay = false;
        if (cycle == 0) {
            for (const auto [w, h] : {std::pair{1360, 900}, std::pair{1160, 780}}) {
                SDL_SetWindowSize(ws.window, w, h);
                XMoveResizeWindow(ws.xdisplay, container, 20, 105, static_cast<unsigned>(w - 40),
                                  static_cast<unsigned>(h - 125));
                std::uint32_t gw = static_cast<std::uint32_t>(w - 40);
                std::uint32_t gh = static_cast<std::uint32_t>(h - 125);
                const bool resized = host.gui->can_resize(host.plugin) &&
                                     host.gui->adjust_size(host.plugin, &gw, &gh) &&
                                     host.gui->set_size(host.plugin, gw, gh);
                resizeSuccess += resized ? 1 : 0;
                std::printf("resize=%dx%d plugin=%ux%u result=%s\n", w, h, gw, gh,
                            resized ? "true" : "false");
                if (!resized) okay = false;
                drawFx(ws);
                for (int j = 0; j < 18; ++j) {
                    SDL_Event event{};
                    SDL_WaitEventTimeout(&event, 16);
                    host.tick();
                }
                // A tiling compositor can override the requested top-level size.
                // Reconcile the child against the size X11 actually reports.
                XWindowAttributes measured{};
                XGetWindowAttributes(ws.xdisplay, ws.xid, &measured);
                const int childW = std::max(1, measured.width - 40);
                const int childH = std::max(1, measured.height - 125);
                XMoveResizeWindow(ws.xdisplay, container, 20, 105, static_cast<unsigned>(childW),
                                  static_cast<unsigned>(childH));
                std::uint32_t fittedW = static_cast<std::uint32_t>(childW);
                std::uint32_t fittedH = static_cast<std::uint32_t>(childH);
                const bool fitted = host.gui->adjust_size(host.plugin, &fittedW, &fittedH) &&
                                    host.gui->set_size(host.plugin, fittedW, fittedH);
                std::printf("actual_after_request=%dx%d plugin_fit=%ux%u result=%s\n",
                            measured.width, measured.height, fittedW, fittedH,
                            fitted ? "true" : "false");
                if (!fitted) okay = false;
                XSync(ws.xdisplay, False);
                drawFx(ws);
            }
            XSync(ws.xdisplay, False);
            int actualW = 0, actualH = 0;
            SDL_GetWindowSize(ws.window, &actualW, &actualH);
            XWindowAttributes attr{};
            XGetWindowAttributes(ws.xdisplay, ws.xid, &attr);
            std::printf("final_window SDL=%dx%d X11=%dx%d\n", actualW, actualH, attr.width,
                        attr.height);
            drawFx(ws);
            glFinish();
            SDL_Delay(100);
            const char* shot = "docs/research/gui-spike/embedded.ppm";
            const bool saved = capture(ws.xdisplay, ws.xid, shot);
            std::printf("screenshot=%s result=%s\n", shot, saved ? "true" : "false");
            if (!saved) okay = false;
        }
        host.closeGui();
        std::printf("timer_after_close=%u\n", host.timerId);
        if (host.timerId != CLAP_INVALID_ID) okay = false;
    }
    const auto until = Clock::now() + std::chrono::seconds(o.seconds);
    while (Clock::now() < until) {
        SDL_Event event{};
        SDL_WaitEventTimeout(&event, 100);
    }
    XDestroyWindow(ws.xdisplay, container);
    XSync(ws.xdisplay, False);
    std::printf("mapped_cycles=%d/3 resize_success=%d/2 timer_ticks=%d audio_ok=%s\n", mappedCycles,
                resizeSuccess, host.ticks,
                host.audioOk.load(std::memory_order_relaxed) ? "true" : "false");
    return okay && mappedCycles == 3 && resizeSuccess == 2 && host.ticks > 0 &&
                   host.audioOk.load(std::memory_order_relaxed)
               ? 0
               : 1;
}
}  // namespace
}  // namespace supple::spike

int main(int argc, char** argv) {
    using namespace supple::spike;
    Options options;
    if (!parse(argc, argv, options)) {
        std::fprintf(stderr,
                     "usage: gui-shell-spike --draw [--frames N] [--size WxH] | "
                     "--idle [--seconds S] | --text [--seconds S] | "
                     "--embed path.clap [--seconds S]\n");
        return 2;
    }
    WindowState window;
    if (!window.open(options)) return 1;
    if (options.mode == "--draw") return runDraw(window, options);
    if (options.mode == "--idle") return runIdle(window, options);
    if (options.mode == "--text") return runText(window, options);
    return runEmbed(window, options);
}
