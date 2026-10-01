// <slug>: the DAW with a window (docs/specs/gui.md). The UI thread is the
// model thread: it pumps the command bus's loop (socket requests, the service
// tick) between SDL events, and draws only when something changed.
//
//   <slug> [PROJECT] [--offscreen] [--size WxH] [--frames N] [--snapshot PNG at exit]
//          [--bench] [--no-socket] [--instance NAME] [--socket PATH]
//          [--workdir DIR] [--plugin-path P]...
#include <SDL3/SDL.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "shell.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/product.h"
#include "supple/rpc/model_loop.h"
#include "supple/rpc/protocol.h"
#include "supple/ui/root.h"

namespace fs = std::filesystem;
using namespace supple;

namespace {

struct Options {
    fs::path project, workdir, socket, snapshot;
    std::vector<fs::path> pluginPath;
    std::string instance = "default";
    bool offscreen = false, bench = false, serve = true;
    int width = 1280, height = 800, frames = 0;  // frames 0 = run until closed
};

int usage() {
    std::fprintf(stderr,
                 "usage: %s [PROJECT] [--offscreen] [--size WxH] [--frames N] [--snapshot PNG]\n"
                 "       [--bench] [--no-socket] [--instance NAME] [--socket PATH]\n"
                 "       [--workdir DIR] [--plugin-path P]...\n",
                 std::string(product::kSlug).c_str());
    return 2;
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
        if (a == "--offscreen")
            o.offscreen = true;
        else if (a == "--bench")
            o.bench = true;
        else if (a == "--no-socket")
            o.serve = false;
        else if (a == "--size") {
            const char* v = value();
            if (!v || std::sscanf(v, "%dx%d", &o.width, &o.height) != 2) return false;
        } else if (a == "--frames") {
            const char* v = value();
            if (!v) return false;
            o.frames = std::max(1, std::atoi(v));
        } else if (a == "--snapshot") {
            const char* v = value();
            if (!v) return false;
            o.snapshot = v;
        } else if (a == "--instance") {
            const char* v = value();
            if (!v) return false;
            o.instance = v;
        } else if (a == "--socket") {
            const char* v = value();
            if (!v) return false;
            o.socket = v;
        } else if (a == "--workdir") {
            const char* v = value();
            if (!v) return false;
            o.workdir = v;
        } else if (a == "--plugin-path") {
            const char* v = value();
            if (!v) return false;
            o.pluginPath.emplace_back(v);
        } else if (!a.empty() && a[0] != '-' && o.project.empty())
            o.project = fs::absolute(a);
        else
            return false;
    }
    return o.width > 0 && o.height > 0;
}

// Milliseconds since the kernel started this process (exec and dynamic
// loading included), for the cold-start gate (ADR-0005 §5: < 300 ms).
double millisecondsSinceProcessStart() {
    std::ifstream stat("/proc/self/stat");
    std::string line;
    std::getline(stat, line);
    const auto close = line.rfind(')');
    if (close == std::string::npos) return -1;
    std::istringstream fields(line.substr(close + 2));
    std::string field;
    for (int i = 3; i <= 22 && fields >> field; ++i)
        if (i == 22) {
            timespec boot{};
            clock_gettime(CLOCK_BOOTTIME, &boot);
            const double nowS =
                static_cast<double>(boot.tv_sec) + 1e-9 * static_cast<double>(boot.tv_nsec);
            const double startS = std::stod(field) / static_cast<double>(sysconf(_SC_CLK_TCK));
            return 1000.0 * (nowS - startS);
        }
    return -1;
}

double threadCpuMs() {
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return 1000.0 * static_cast<double>(t.tv_sec) + 1e-6 * static_cast<double>(t.tv_nsec);
}

double percentile(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1) + 0.5)];
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse(argc, argv, o)) return usage();

    auto platform = gui::Platform::start(o.offscreen ? gui::Platform::Driver::Offscreen
                                                     : gui::Platform::Driver::X11);
    if (!platform) {
        std::fprintf(stderr, "%s\n", platform.error().toString().c_str());
        return 1;
    }
    auto window = gui::Window::open(**platform,
                                    {std::string(product::kName), o.width, o.height, o.offscreen});
    if (!window) {
        std::fprintf(stderr, "%s\n", window.error().toString().c_str());
        return 1;
    }
    auto fonts = gui::loadFonts((*window)->vg(), SUPPLE_GUI_FONTS_DIR);
    if (!fonts) {
        std::fprintf(stderr, "%s\n", fonts.error().toString().c_str());
        return 1;
    }

    engine::AppOptions options;
    options.identity.name = std::string(product::kName);
    options.identity.vendor = std::string(product::kVendor);
    options.identity.version = std::string(product::kVersion);
    options.pluginPath = o.pluginPath;
    options.workdir = !o.workdir.empty()   ? o.workdir
                      : !o.project.empty() ? o.project.parent_path()
                                           : fs::current_path();
    auto app = engine::EngineApp::create(options);
    if (!app) {
        std::fprintf(stderr, "%s\n", app.error().toString().c_str());
        return 1;
    }
    if (!o.project.empty() && fs::exists(o.project)) {
        auto opened = (*app)->open(o.project);
        if (!opened) {
            std::fprintf(stderr, "%s\n", opened.error().toString().c_str());
            return 1;
        }
    }
    cmd::Registry registry;
    engine::registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, **app);
    (*app)->attachCommands(dispatcher);

    // The model loop runs inside the SDL loop: its work wakes SDL_WaitEvent.
    rpc::ModelLoop loop;
    loop.setWaker([p = platform->get()] { p->wake(); });
    std::unique_ptr<rpc::Server> server;
    auto publish = [&] {
        for (auto& [topic, payload] : (*app)->takeEvents())
            if (server) server->publish(topic, payload);
    };
    std::string status = "offscreen";
    if (o.serve) {
        server = std::make_unique<rpc::Server>(registry, dispatcher, [&](auto task) {
            loop.post([&, task = std::move(task)] {
                task();
                publish();
            });
        });
        const fs::path path = o.socket.empty() ? rpc::socketPath(o.instance) : o.socket;
        auto started = server->start(path);
        if (!started) {
            std::fprintf(stderr, "%s\n", started.error().toString().c_str());
            return 1;
        }
        status = "agents: " + path.string();
    }
    auto tick = loop.every(std::chrono::milliseconds(100), [&] {
        (*app)->tick();
        publish();
    });

    gui::Shell shell(**app, status);
    ui::Root root(**window, *fonts, ui::Theme::dark());
    root.setContent(shell.build());
    bool quit = false;
    int drawn = 0;
    double coldStartMs = -1;
    std::vector<double> frameCpu;
    while (!quit && !loop.stopRequested()) {
        const int next = loop.pump();
        shell.sync();
        if (o.frames > 0) root.requestAnimationFrame();  // benchmark runs draw every pass
        if (root.wantsFrame()) {
            const double cpu0 = threadCpuMs();
            root.frame();
            frameCpu.push_back(threadCpuMs() - cpu0);
            if (drawn == 0) coldStartMs = millisecondsSinceProcessStart();
            if (o.frames > 0 && ++drawn >= o.frames) break;
            if (o.frames == 0) ++drawn;
        }
        SDL_Event e;
        const bool got = o.frames > 0 ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, next);
        for (bool have = got; have; have = SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
                quit = true;
            else
                root.handle(e);  // the wake event is ignored: it only interrupts the wait
        }
    }
    tick.cancel();
    if (server) server->stop();
    if (!o.snapshot.empty()) {  // the final state, for tests and bug reports
        shell.sync();
        root.damageAll();
        root.frame(false);
        auto written = gui::writePng(o.snapshot, root.readBack());
        if (!written) {
            std::fprintf(stderr, "%s\n", written.error().toString().c_str());
            return 1;
        }
    }
    if (o.bench)
        std::printf(
            "{\"schema\": \"supple.gui-bench/1.0\", \"cold_start_ms\": %.1f, \"frames\": %d, "
            "\"frame_cpu_ms\": {\"p50\": %.3f, \"p95\": %.3f, \"max\": %.3f}, \"driver\": "
            "\"%s\"}\n",
            coldStartMs, drawn, percentile(frameCpu, 0.5), percentile(frameCpu, 0.95),
            frameCpu.empty() ? 0.0 : *std::max_element(frameCpu.begin(), frameCpu.end()),
            o.offscreen ? "offscreen" : "x11");
    return 0;
}
