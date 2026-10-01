// <slug>: the DAW with a window (docs/specs/gui.md). The UI thread is the
// model thread: it pumps the command bus's loop (socket requests, the service
// tick) between SDL events, and draws only when something changed.
//
//   <slug> [PROJECT] [--offscreen] [--size WxH] [--frames N] [--snapshot PNG at exit]
//          [--bench] [--no-socket] [--instance NAME] [--socket PATH]
//          [--workdir DIR] [--plugin-path P]... [--generate N] [--script]
//          [--idle SECONDS] [--view START_SECONDS,PIXELS_PER_SECOND]
//          [--audio jack|dummy|none]
#include <SDL3/SDL.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "actions_list.h"
#include "agent_panel.h"
#include "app_actions.h"
#include "chrome.h"
#include "file_actions.h"
#include "generate.h"
#include "plugin_windows.h"
#include "shell.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/gui/png.h"
#include "supple/gui/window.h"
#include "supple/peaks/peaks.h"
#include "supple/product.h"
#include "supple/rpc/model_loop.h"
#include "supple/rpc/protocol.h"
#include "supple/ui/root.h"
#include "ui_persistence.h"

namespace fs = std::filesystem;
using namespace supple;

namespace {

struct Options {
    fs::path project, workdir, socket, snapshot;
    std::vector<fs::path> pluginPath;
    std::string instance = "default";
    bool offscreen = false, bench = false, serve = true, script = false;
    int width = 1280, height = 800, frames = 0;  // frames 0 = run until closed
    int generate = 0;                            // tracks of a generated session
    double idleSeconds = 0;                      // measure idle CPU after the frames
    double viewStart = -1, viewPps = 0;          // initial lanes viewport
    std::string audio;  // jack | dummy | none; default: jack (none offscreen)
};

int usage() {
    std::fprintf(stderr,
                 "usage: %s [PROJECT] [--offscreen] [--size WxH] [--frames N] [--snapshot PNG]\n"
                 "       [--bench] [--no-socket] [--instance NAME] [--socket PATH]\n"
                 "       [--workdir DIR] [--plugin-path P]... [--generate N] [--script]\n"
                 "       [--idle SECONDS] [--view START_SECONDS,PIXELS_PER_SECOND]\n"
                 "       [--audio jack|dummy|none]\n",
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
        else if (a == "--script")
            o.script = true;
        else if (a == "--audio") {
            const char* v = value();
            if (!v ||
                (std::string(v) != "jack" && std::string(v) != "dummy" && std::string(v) != "none"))
                return false;
            o.audio = v;
        } else if (a == "--view") {
            const char* v = value();
            if (!v || std::sscanf(v, "%lf,%lf", &o.viewStart, &o.viewPps) != 2) return false;
        } else if (a == "--generate") {
            const char* v = value();
            if (!v) return false;
            o.generate = std::clamp(std::atoi(v), 1, 1024);
        } else if (a == "--idle") {
            const char* v = value();
            if (!v) return false;
            o.idleSeconds = std::max(0.0, std::atof(v));
        } else if (a == "--size") {
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

double processCpuMs() {
    timespec t{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return 1000.0 * static_cast<double>(t.tv_sec) + 1e-6 * static_cast<double>(t.tv_nsec);
}

double wallMs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return 1000.0 * static_cast<double>(t.tv_sec) + 1e-6 * static_cast<double>(t.tv_nsec);
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
    gui::UiState uiState;
    gui::registerGuiCommands(registry, uiState);
    cmd::Dispatcher dispatcher(registry, **app);
    (*app)->attachCommands(dispatcher);
    if (o.generate > 0) {
        auto generated = gui::generateSession(dispatcher, (*app)->mediaDir(), o.generate);
        if (!generated) {
            std::fprintf(stderr, "%s\n", generated.error().toString().c_str());
            return 1;
        }
    }

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

    peaks::PeakService peaks(peaks::defaultCacheDir(product::kSlug),
                             [&](std::function<void()> f) { loop.post(std::move(f)); });
    gui::Shell shell(**app, status, peaks);
    peaks.onReady = [&](const std::string&) { shell.lanes()->invalidate(); };
    ui::Root root(**window, *fonts, ui::Theme::dark());
    bool quit = false;
    gui::AppActions appActions(registry, dispatcher, **app, uiState,
                               actions::Keymap::defaultFile(product::kSlug));
    appActions.onError = [](const std::string& message) {
        std::fprintf(stderr, "%s\n", message.c_str());
    };
    appActions.addUi("ui.actions", "View: actions list",
                     [&] { gui::openActionsList(root, appActions); });
    appActions.addUi("ui.quit", "File: quit", [&] { quit = true; });
    gui::AgentPanel agentPanel(
        root, **app, registry, dispatcher,
        [&](std::function<void()> f) { loop.post(std::move(f)); },
        agent::Config::defaultFile(product::kSlug), SUPPLE_SKILL_DIR);
    appActions.addUi("ui.agent", "View: agent panel", [&] { agentPanel.toggle(); });
    gui::PluginWindows pluginWindows(**platform, **app, [p = platform->get()] { p->wake(); });
    gui::FileActions fileActions(root, appActions, dispatcher, **app, uiState, (*window)->sdl(),
                                 [&](std::function<void()> f) { loop.post(std::move(f)); });
    appActions.addUi("ui.fx.editor", "FX: open editor", "fx", [&](const json::Value& v) -> Status {
        const auto id = v.isString() ? model::Id::parse(v.asString()) : std::nullopt;
        if (!id) return Error("gui.editor", "an effect id like fx_3", "/fx");
        return pluginWindows.open(*id);
    });
    root.onUnhandledKey = [&](const ui::KeyEvent& e) {
        const bool typing = root.focused() && root.focused()->role() == ui::Role::TextField;
        return appActions.key(e.key, e.mods, typing);
    };
    auto chromeLayout = gui::ChromeLayout::load(gui::ChromeLayout::defaultFile(product::kSlug));
    if (!chromeLayout)
        std::fprintf(stderr, "menus: %s (using the defaults)\n",
                     chromeLayout.error().toString().c_str());
    {
        auto frame = std::make_unique<ui::Node>("window");
        frame->layout.dir = ui::Layout::Dir::Column;
        frame->add(gui::makeChrome(root, appActions,
                                   chromeLayout ? *chromeLayout : gui::ChromeLayout::defaults()));
        auto& body = frame->emplace<ui::Node>("main");
        body.layout.dir = ui::Layout::Dir::Row;
        body.add(shell.build());
        body.add(agentPanel.build());
        root.setContent(std::move(frame));
    }
    // The audio engine: the device for interactive use, nothing offscreen.
    const std::string audioMode =
        !o.audio.empty() ? o.audio : (o.offscreen || o.frames > 0 ? "none" : "jack");
    if (audioMode != "none") {
        json::Value args = json::Value::object();
        args["backend"] = audioMode;
        if (auto started = dispatcher.execute(
                {"engine.start", std::move(args), {cmd::Origin::Type::Ui, "gui", {}}});
            !started)
            std::fprintf(
                stderr,
                "audio: %s (run engine.start from the actions list when the device is ready)\n",
                started.error().toString().c_str());
    }

    // Per-project UI state (gui.md §1.2).
    gui::UiPersistence uiFile;
    uiFile.add(
        "selection",
        [&] {
            json::Value v = json::Value::object(), tracks = json::Value::array(),
                        items = json::Value::array();
            for (const auto& t : uiState.selectedTracks) tracks.push_back(t);
            for (const auto& i : uiState.selectedItems) items.push_back(i);
            v["tracks"] = std::move(tracks);
            v["items"] = std::move(items);
            return v;
        },
        [&](const json::Value& v) {
            auto ids = [](const json::Value* a) {
                std::vector<std::string> out;
                if (a && a->isArray())
                    for (const auto& x : a->asArray())
                        if (x.isString()) out.push_back(x.asString());
                return out;
            };
            uiState.selectedTracks = ids(v.find("tracks"));
            uiState.selectedItems = ids(v.find("items"));
        });
    uiFile.add(
        "panels",
        [&] {
            json::Value v = json::Value::object();
            v["agent"] = agentPanel.visible();
            return v;
        },
        [&](const json::Value& v) {
            if (const json::Value* a = v.find("agent"); a && a->isBool())
                agentPanel.setVisible(a->asBool());
        });
    uiFile.add(
        "window",
        [&] {
            json::Value v = json::Value::object();
            v["width"] = static_cast<int>((*window)->width());
            v["height"] = static_cast<int>((*window)->height());
            return v;
        },
        [&](const json::Value& v) {
            const json::Value* w = v.find("width");
            const json::Value* h = v.find("height");
            if (!o.offscreen && w && h && w->isNumber() && h->isNumber())
                (*window)->setSize(static_cast<int>(w->asInt()), static_cast<int>(h->asInt()));
        });
    uiFile.add(
        "lanes",
        [&] {
            const views::Viewport& vp = shell.lanes()->viewport();
            json::Value v = json::Value::object();
            v["start_seconds"] = vp.startSeconds;
            v["pixels_per_second"] = vp.pixelsPerSecond;
            v["scroll_y"] = static_cast<double>(vp.scrollY);
            v["row_height"] = static_cast<double>(vp.rowHeight);
            return v;
        },
        [&](const json::Value& v) {
            views::Viewport vp = shell.lanes()->viewport();
            auto num = [&](const char* k, auto& out) {
                if (const json::Value* x = v.find(k); x && x->isNumber())
                    out = static_cast<std::remove_reference_t<decltype(out)>>(x->asDouble());
            };
            num("start_seconds", vp.startSeconds);
            num("pixels_per_second", vp.pixelsPerSecond);
            num("scroll_y", vp.scrollY);
            num("row_height", vp.rowHeight);
            shell.lanes()->setViewport(vp);
        });
    if (o.viewStart >= 0) {
        views::Viewport v;
        v.startSeconds = o.viewStart;
        v.pixelsPerSecond = o.viewPps;
        shell.lanes()->setViewport(v);
    }
    int drawn = 0, longTasks = 0;
    double coldStartMs = -1, longestTaskMs = 0;
    std::vector<double> frameCpu;
    // Counted runs (--frames) first wait, uncounted, until every visible
    // pyramid is ready; --script then moves the viewport every frame.
    bool warm = o.frames == 0;
    const double warmUntil = wallMs() + 60000;
    int step = 0;
    while (!quit && !loop.stopRequested()) {
        const double t0 = wallMs();
        int next = loop.pump();
        if (const int plugin = pluginWindows.service(); plugin >= 0)
            next = next < 0 ? plugin : std::min(next, plugin);
        if (const double ms = wallMs() - t0; ms > 2.0) {
            ++longTasks;
            longestTaskMs = std::max(longestTaskMs, ms);
        }
        shell.sync();
        uiFile.tick((*app)->projectPath());
        if (o.script && warm) {
            views::Lanes& lanes = *shell.lanes();
            views::Viewport v = lanes.viewport();
            const double k = step++;
            v.pixelsPerSecond = 40.0 * std::pow(2.0, 3.0 * std::sin(k / 40.0));
            v.startSeconds = 2.0 + 20.0 * (1 + std::sin(k / 70.0));
            v.scrollY = static_cast<float>((1 + std::sin(k / 25.0)) / 2) *
                        std::max(0.0f, lanes.contentHeight() - lanes.bounds().h);
            lanes.setViewport(v);
        }
        if (o.frames > 0) root.requestAnimationFrame();  // benchmark runs draw every pass
        if (root.wantsFrame()) {
            const double cpu0 = threadCpuMs();
            root.frame();
            const double cpu = threadCpuMs() - cpu0;
            if (drawn == 0 && coldStartMs < 0) coldStartMs = millisecondsSinceProcessStart();
            if (!warm) {
                warm = shell.lanes()->stats().pending == 0;
                if (!warm && wallMs() > warmUntil) {
                    std::fprintf(stderr, "peaks did not become ready within 60 s\n");
                    return 1;
                }
            } else {
                frameCpu.push_back(cpu);
                if (o.frames > 0 && ++drawn >= o.frames) break;
                if (o.frames == 0) ++drawn;
            }
        }
        SDL_Event e;
        const bool got = o.frames > 0 ? SDL_PollEvent(&e) : SDL_WaitEventTimeout(&e, next);
        for (bool have = got; have; have = SDL_PollEvent(&e)) {
            if (pluginWindows.handle(e)) continue;  // an editor window's event
            if (e.type == SDL_EVENT_QUIT || e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (o.frames > 0 || !fileActions.dirty())
                    quit = true;
                else if (!root.popupOpen())
                    fileActions.confirmDiscard("Quit?", [&] { quit = true; });
            } else
                root.handle(e);  // the wake event is ignored: it only interrupts the wait
        }
    }
    double idleCpuPercent = -1;
    if (o.idleSeconds > 0) {  // no input, no model changes: the loop should sleep
        const double c0 = processCpuMs(), w0 = wallMs();
        while (wallMs() - w0 < o.idleSeconds * 1000) {
            const int next = loop.pump();
            shell.sync();
            if (root.wantsFrame()) root.frame();
            SDL_Event e;
            const double left = o.idleSeconds * 1000 - (wallMs() - w0);
            const int wait =
                next < 0 ? static_cast<int>(left) : std::min(next, static_cast<int>(left));
            if (SDL_WaitEventTimeout(&e, std::max(1, wait)))
                while (SDL_PollEvent(&e)) {
                }
        }
        idleCpuPercent = 100.0 * (processCpuMs() - c0) / (wallMs() - w0);
    }
    tick.cancel();
    if (server) server->stop();
    (void)uiFile.flush((*app)->projectPath());
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
    if (o.bench) {
        const auto& ls = shell.lanes()->stats();
        std::printf(
            "{\"schema\": \"supple.gui-bench/1.1\", \"driver\": \"%s\", \"cold_start_ms\": %.1f, "
            "\"frames\": %d, \"frame_cpu_ms\": {\"p50\": %.3f, \"p95\": %.3f, \"max\": %.3f}, "
            "\"ui_tasks_over_2ms\": %d, \"longest_ui_task_ms\": %.2f, \"idle_cpu_percent\": %.2f, "
            "\"tracks\": %d, \"last_frame\": {\"rows\": %zu, \"items\": %zu, \"columns\": %zu, "
            "\"notes\": %zu}, \"peaks\": {\"built\": %d, \"loaded\": %d}, \"gl\": \"%s\"}\n",
            o.offscreen ? "offscreen" : "x11", coldStartMs, drawn, percentile(frameCpu, 0.5),
            percentile(frameCpu, 0.95),
            frameCpu.empty() ? 0.0 : *std::max_element(frameCpu.begin(), frameCpu.end()), longTasks,
            longestTaskMs, idleCpuPercent, o.generate, ls.rows, ls.items, ls.columns, ls.notes,
            peaks.stats().built, peaks.stats().loaded, (*window)->renderer().c_str());
    }
    return 0;
}
