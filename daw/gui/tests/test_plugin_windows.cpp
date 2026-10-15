// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Native editor integration. Real editor windows require
// SUPPLE_GUI_DISPLAY_TEST=1 (a window appears for about a second).
#include <SDL3/SDL.h>
#include <doctest/doctest.h>
#include "product_fixture.h"

#include <clap/clap.h>
#if defined(__linux__)
#include <unistd.h>
#endif
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <thread>

#include "plugin_windows.h"
#include "supple/base/environment.h"
#include "supple/cmd/bus.h"
#include "supple/engine/app.h"
#include "supple/host/clap_host.h"
#include "supple/rpc/model_loop.h"
#include "test_support.h"

using namespace supple;
namespace {
bool displayTestsEnabled() {
    if (!environmentFlagEnabled("SUPPLE_GUI_DISPLAY_TEST")) return false;
#if defined(__linux__)
    return std::getenv("DISPLAY") != nullptr;
#else
    return true;
#endif
}
}  // namespace

#if defined(__linux__)
TEST_CASE("editor FD watching reports startup failure and stops without a display") {
    auto failed = gui::FdWatcher::create([] {},
                                         [](int*) {
                                             errno = EMFILE;
                                             return -1;
                                         });
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "gui.editor.fds");
    int descriptors[2];
    REQUIRE(::pipe(descriptors) == 0);
    std::mutex mutex;
    std::condition_variable ready;
    bool notified = false;
    auto watcher = gui::FdWatcher::create([&] {
        std::lock_guard lock(mutex);
        notified = true;
        ready.notify_one();
    });
    REQUIRE(watcher);
    (*watcher)->watch({{descriptors[0], CLAP_POSIX_FD_READ, 1}});
    REQUIRE(::write(descriptors[1], "x", 1) == 1);
    {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return notified; });
    }
    const auto events = (*watcher)->takeReady();
    REQUIRE(events.size() == 1);
    CHECK(events.front().fd == descriptors[0]);
    CHECK(events.front().owner == 1);
    CHECK((events.front().flags & CLAP_POSIX_FD_READ) != 0);
    CHECK_FALSE((*watcher)->error());
    watcher->reset();  // must wake an otherwise idle poll before joining
    ::close(descriptors[0]);
    ::close(descriptors[1]);
    notified = false;
    int polls = 0;
    auto broken = gui::FdWatcher::create(
        [&] {
            std::lock_guard lock(mutex);
            notified = true;
            ready.notify_one();
        },
        {},
        [&](std::span<pollfd>) {
            ++polls;
            errno = EBADF;
            return -1;
        });
    REQUIRE(broken);
    {
        std::unique_lock lock(mutex);
        ready.wait(lock, [&] { return notified; });
    }
    REQUIRE((*broken)->error());
    CHECK((*broken)->error()->code == "gui.editor.fds");
    broken->reset();
    CHECK(polls == 1);  // a permanent failure terminates instead of spinning
}

#endif

TEST_CASE("the stock plugins offer embeddable native editors (no display needed)") {
    const std::filesystem::path dir = SUPPLE_CLAP_DIR;
    auto plugins = host::scan(std::span<const std::filesystem::path>(&dir, 1));
    REQUIRE(plugins);
    if (plugins->empty()) {  // a partial build (the plugins build with ALL)
        MESSAGE("skipped: no stock plugins in " << dir.string());
        return;
    }
    int withEditor = 0;
    for (const host::PluginInfo& info : *plugins) {
        auto instance = host::Instance::create(
            info, host::HostIdentity{gui::test::name(), "Supple Audio", "", "0"});
        REQUIRE(instance);
        withEditor += (*instance)->hasEditor();
        CHECK_FALSE((*instance)->editorOpen());
        CHECK((*instance)->nextTimerMs() >= -1);
    }
    MESSAGE(withEditor << " of " << plugins->size() << " plugins have editors");
    CHECK(withEditor >= 8);  // the stock effects and Orrery ship editors
}

TEST_CASE("an editor opens in its window, services its timers and closes") {
    if (!displayTestsEnabled()) {
        MESSAGE("skipped: set SUPPLE_GUI_DISPLAY_TEST=1 in a native desktop session");
        return;
    }
    auto platform = gui::Platform::start(gui::Platform::Driver::Native);
    REQUIRE(platform);
    engine::AppOptions options;
    options.workdir = std::filesystem::temp_directory_path() /
                      ("supple-editor-test-" + std::to_string(testProcessId()));
    auto app = engine::EngineApp::create(options);
    REQUIRE(app);
    const std::filesystem::path dir = SUPPLE_CLAP_DIR;
    auto plugins = host::scan(std::span<const std::filesystem::path>(&dir, 1));
    REQUIRE(plugins);
    gui::PluginWindows windows(**platform, **app, [] {});
    for (const host::PluginInfo& info : *plugins) {
        auto instance = host::Instance::create(
            info, host::HostIdentity{gui::test::name(), "Supple Audio", "", "0"});
        REQUIRE(instance);
        if (!(*instance)->hasEditor()) continue;
        // SUPPLE_EDITOR_ONLY=<name> and SUPPLE_EDITOR_HOLD_MS for a manual look.
        if (const char* only = std::getenv("SUPPLE_EDITOR_ONLY"); only && info.name != only)
            continue;
        REQUIRE((*instance)->activate(48000, 32, 512));
        auto opened = windows.open(**instance, info.name);
        REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
        CHECK((*instance)->editorOpen());
        CHECK(windows.count() == 1);
        // A short run of the UI loop: timers and fds are serviced.
        const char* hold = std::getenv("SUPPLE_EDITOR_HOLD_MS");
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(hold ? std::atoi(hold) : 400);
        while (std::chrono::steady_clock::now() < until) {
            const int next = windows.service();
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, next < 0 ? 20 : std::min(next, 20))) windows.handle(e);
        }
        windows.closeAll();
        CHECK_FALSE((*instance)->editorOpen());
        (*instance)->deactivate();
        MESSAGE("editor of " << info.name << " opened and closed");
    }
    std::filesystem::remove_all(options.workdir);
}

TEST_CASE("isolated plugins' editors embed from their bridge process (opt-in, needs a display)") {
    if (!displayTestsEnabled()) return;
    host::setBridgeExecutable(SUPPLE_BRIDGE_PATH);
    auto platform = gui::Platform::start(gui::Platform::Driver::Native);
    REQUIRE(platform);
    engine::AppOptions options;
    options.workdir = std::filesystem::temp_directory_path() /
                      ("supple-bridged-editor-" + std::to_string(testProcessId()));
    auto app = engine::EngineApp::create(options);
    REQUIRE(app);
    const std::filesystem::path dir = SUPPLE_CLAP_DIR;
    auto plugins = host::scan(std::span<const std::filesystem::path>(&dir, 1));
    REQUIRE(plugins);
    gui::PluginWindows windows(**platform, **app, [] {});
    int opened = 0;
    for (const host::PluginInfo& info : *plugins) {
        if (const char* only = std::getenv("SUPPLE_EDITOR_ONLY"); only && info.name != only)
            continue;
        auto instance = host::Instance::create(
            info, host::HostIdentity{gui::test::name(), "Supple Audio", "", "0"}, true);
        REQUIRE(instance);
        if (!(*instance)->hasEditor()) continue;
        REQUIRE((*instance)->activate(48000, 32, 512));
        auto open = windows.open(**instance, info.name);
        REQUIRE_MESSAGE(open, (open ? "" : open.error().toString()));
        const char* hold = std::getenv("SUPPLE_EDITOR_HOLD_MS");
        const auto until = std::chrono::steady_clock::now() +
                           std::chrono::milliseconds(hold ? std::atoi(hold) : 400);
        while (std::chrono::steady_clock::now() < until) {
            const int next = windows.service();
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, next < 0 ? 20 : std::min(next, 20))) windows.handle(e);
        }
        CHECK(windows.count() == 1);  // the bridge kept it open
        windows.closeAll();
        CHECK_FALSE((*instance)->editorOpen());
        (*instance)->deactivate();
        ++opened;
        MESSAGE("bridged editor of " << info.name << " (pid " << (*instance)->bridgePid()
                                     << ") opened and closed");
    }
    CHECK(opened >= 1);
    std::filesystem::remove_all(options.workdir);
}

TEST_CASE("VST3 editors embed too (installed plugins; opt-in, needs a display)") {
    if (!displayTestsEnabled()) return;
    auto directories = host::defaultSearchPath();
    if (const char* directory = std::getenv("SUPPLE_VST3_DIR"))
        directories = {std::filesystem::path(directory)};
    auto platform = gui::Platform::start(gui::Platform::Driver::Native);
    REQUIRE(platform);
    engine::AppOptions options;
    options.workdir = std::filesystem::temp_directory_path() /
                      ("supple-vst3-editor-" + std::to_string(testProcessId()));
    auto app = engine::EngineApp::create(options);
    REQUIRE(app);
    auto plugins = host::scan(directories);
    REQUIRE(plugins);
    gui::PluginWindows windows(**platform, **app, [] {});
    int opened = 0;
    for (const host::PluginInfo& info : *plugins) {
        if (info.format != "vst3") continue;
        if (const char* only = std::getenv("SUPPLE_EDITOR_ONLY"); only && info.name != only)
            continue;
        auto instance = host::Instance::create(
            info, host::HostIdentity{gui::test::name(), "Supple Audio", "", "0"});
        if (!instance) {
            MESSAGE(info.name << ": " << instance.error().message);
            continue;
        }
        if (!(*instance)->hasEditor()) {
            MESSAGE(info.name << ": no native editor");
            continue;
        }
        REQUIRE((*instance)->activate(48000, 32, 512));
        auto open = windows.open(**instance, info.name);
        REQUIRE_MESSAGE(open, (open ? "" : open.error().toString()));
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
        while (std::chrono::steady_clock::now() < until) {
            const int next = windows.service();
            SDL_Event e;
            if (SDL_WaitEventTimeout(&e, next < 0 ? 20 : std::min(next, 20))) windows.handle(e);
        }
        windows.closeAll();
        CHECK_FALSE((*instance)->editorOpen());
        (*instance)->deactivate();
        ++opened;
        MESSAGE("VST3 editor of " << info.name << " opened and closed");
    }
    MESSAGE(opened << " VST3 editors");
    std::filesystem::remove_all(options.workdir);
}

TEST_CASE("editor sizing preserves native pixels at fractional Windows DPI") {
    for (double scale : {1.25, 1.5, 1.75, 2.0}) {
        for (std::uint32_t pixels : {511u, 512u, 513u}) {
            const auto [w, h] =
                gui::editorWindowSize(pixels, pixels, NativeWindow::Kind::Win32, scale, scale);
            CAPTURE(scale);
            CAPTURE(pixels);
            CHECK(w == static_cast<int>(pixels));
            CHECK(h == static_cast<int>(pixels));
        }
    }
    // Cocoa sizes are points, despite its two backing pixels per point.
    CHECK(gui::editorWindowSize(513, 257, NativeWindow::Kind::Cocoa, 2, 1) ==
          std::pair<int, int>{513, 257});
    CHECK(gui::editorWindowSize(512, 256, NativeWindow::Kind::X11, 2, 1) ==
          std::pair<int, int>{256, 128});
}

TEST_CASE("an editor is told about display-scale changes, not about noise") {
    CHECK_FALSE(gui::editorScaleChanged(1.5, 1.5));
    CHECK_FALSE(gui::editorScaleChanged(1.5, 1.5 + 1e-9));
    CHECK(gui::editorScaleChanged(1.0, 1.25));  // 100% to 125%
    CHECK(gui::editorScaleChanged(2.0, 1.0));   // to a standard-density monitor
    CHECK(gui::editorScaleChanged(1.0, 1.001 * 1.01));
    // An unusable reading is never a change to report.
    CHECK_FALSE(gui::editorScaleChanged(1.0, 0.0));
    CHECK_FALSE(gui::editorScaleChanged(0.0, 1.0));
    CHECK_FALSE(gui::editorScaleChanged(1.0, std::numeric_limits<double>::quiet_NaN()));
}

TEST_CASE("a bridge-owned editor window reopens only where a display still shows it") {
    auto platform = gui::Platform::start(gui::Platform::Driver::Offscreen);
    REQUIRE(platform);
    int count = 0;
    SDL_DisplayID* displays = SDL_GetDisplays(&count);
    REQUIRE(displays);
    REQUIRE(count > 0);
    SDL_Rect bounds{};
    REQUIRE(SDL_GetDisplayBounds(displays[0], &bounds));
    SDL_free(displays);
    const int x = bounds.x + 40, y = bounds.y + 40;
    // A remembered position and size on a display is restored as it was.
    auto frame = gui::restoredEditorFrame(x, y, 320, 240);
    REQUIRE(frame);
    CHECK(frame->x == x);
    CHECK(frame->y == y);
    CHECK(frame->width == 320);
    CHECK(frame->height == 240);
    // A position alone (a fixed-size editor) carries no size.
    frame = gui::restoredEditorFrame(x, y, 0, 0);
    REQUIRE(frame);
    CHECK(frame->width == 0);
    CHECK(frame->height == 0);
    // A monitor that is gone: the bridge places the window itself.
    CHECK_FALSE(gui::restoredEditorFrame(bounds.x + bounds.w + 5000, y, 320, 240));
    CHECK_FALSE(gui::restoredEditorFrame(x, bounds.y - 9000, 0, 0));
}

TEST_CASE("an offscreen editor container exposes its available native parent") {
    auto platform = gui::Platform::start(gui::Platform::Driver::Offscreen);
    REQUIRE(platform);
    gui::WindowOptions options{"editor-parent", 64, 48, true};
    auto window = gui::Window::open(**platform, options);
    REQUIRE(window);
#if defined(__linux__)
    CHECK_FALSE((*window)->nativeParent().valid());
#else
    // The native GUI's offscreen mode uses hidden desktop windows.
    CHECK((*window)->nativeParent().valid());
    CHECK((*window)->nativeParent().kind == nativeWindowKind());
#endif
}

TEST_CASE("editor show and hide requests preserve the offscreen parent window") {
    auto platform = gui::Platform::start(gui::Platform::Driver::Offscreen);
    REQUIRE(platform);
    gui::WindowOptions options{"editor-requests", 64, 48, true};
    options.gl = false;
    auto opened = gui::Window::open(**platform, options);
    REQUIRE(opened);
    auto& window = **opened;
    const auto identity = window.id();
    host::Instance::EditorRequests requests;
    requests.show = true;
    gui::applyEditorRequests(window, requests);
    CHECK((SDL_GetWindowFlags(window.sdl()) & SDL_WINDOW_HIDDEN) == 0);
    requests = {};
    requests.hide = true;
    gui::applyEditorRequests(window, requests);
    CHECK((SDL_GetWindowFlags(window.sdl()) & SDL_WINDOW_HIDDEN) != 0);
    requests.show = true;
    requests.resize = true;
    requests.width = 80;
    requests.height = 60;
    gui::applyEditorRequests(window, requests);
    CHECK((SDL_GetWindowFlags(window.sdl()) & SDL_WINDOW_HIDDEN) != 0);
    CHECK(window.width() *
              (nativeWindowKind() == NativeWindow::Kind::Cocoa ? 1 : window.pixelRatio()) ==
          doctest::Approx(80));
    CHECK(window.height() *
              (nativeWindowKind() == NativeWindow::Kind::Cocoa ? 1 : window.pixelRatio()) ==
          doctest::Approx(60));
    requests.hide = false;
    gui::applyEditorRequests(window, requests);
    CHECK((SDL_GetWindowFlags(window.sdl()) & SDL_WINDOW_HIDDEN) == 0);
    requests.closed = true;
    requests.hide = true;
    requests.width = 99;
    gui::applyEditorRequests(window, requests);
    CHECK((SDL_GetWindowFlags(window.sdl()) & SDL_WINDOW_HIDDEN) == 0);
    CHECK(window.width() *
              (nativeWindowKind() == NativeWindow::Kind::Cocoa ? 1 : window.pixelRatio()) ==
          doctest::Approx(80));
    CHECK(window.id() == identity);
}

TEST_CASE("an editor follows its effect onto a replacement instance (needs a display)") {
    // fx.restart (or a VST3 key change) replaces the instance: the engine
    // closes the old editor while its window exists, and the window shows the
    // new instance's editor (it used to keep a dead editor parented to a
    // destroyed window, which crashed plug-ins and sometimes the app).
    if (!displayTestsEnabled()) {
        MESSAGE("skipped: set SUPPLE_GUI_DISPLAY_TEST=1 in a native desktop session");
        return;
    }
    auto platform = gui::Platform::start(gui::Platform::Driver::Native);
    REQUIRE(platform);
    engine::AppOptions options;
    options.workdir = std::filesystem::temp_directory_path() /
                      ("supple-editor-replace-" + std::to_string(testProcessId()));
    options.pluginPath = {std::filesystem::path(SUPPLE_CLAP_DIR)};
    options.identity = host::HostIdentity{gui::test::name(), "Supple Audio", "", "0"};
    auto app = engine::EngineApp::create(options);
    REQUIRE(app);
    cmd::Registry registry;
    engine::registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, **app);
    (*app)->attachCommands(dispatcher);
    const cmd::Origin origin{cmd::Origin::Type::Test, "editor-replace-test", {}};
    auto run = [&](const char* command, json::Value args) {
        auto r = dispatcher.execute({command, std::move(args), origin});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    json::Value start = json::Value::object();
    start["backend"] = "dummy";
    run("engine.start", std::move(start));
    std::string plugin;
    json::Value list = run("plugin.list", json::Value::object());
    for (const json::Value& p : list["plugins"].asArray())
        if (const json::Value* n = p.find("name"); n && n->isString() && n->asString() == "Plumb")
            plugin = p.find("id")->asString();
    if (plugin.empty()) {
        MESSAGE("skipped: the stock plugins are not built");
        return;
    }
    json::Value track = json::Value::object();
    track["type"] = "channel";
    const std::string trackId = run("track.add", std::move(track))["track"].asString();
    json::Value add = json::Value::object();
    add["track"] = trackId;
    add["plugin"] = plugin;
    const auto fx = *model::Id::parse(run("fx.add", std::move(add))["fx"].asString());
    {
        gui::PluginWindows windows(**platform, **app, [] {});
        auto pump = [&](int ms) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < until) {
                (*app)->tick();
                const int next = windows.service();
                SDL_Event e;
                if (SDL_WaitEventTimeout(&e, next < 0 ? 20 : std::min(next, 20))) windows.handle(e);
            }
        };
        REQUIRE(windows.open(fx));
        {  // the editor window's input is the plug-in's, never the app's
            int count = 0;
            SDL_Window** all = SDL_GetWindows(&count);
            REQUIRE(count == 1);
            SDL_Event e{};
            e.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            e.button.windowID = SDL_GetWindowID(all[0]);
            e.button.button = SDL_BUTTON_LEFT;
            SDL_free(all);
            CHECK(windows.handle(e));
            e.type = SDL_EVENT_MOUSE_BUTTON_UP;
            CHECK(windows.handle(e));
        }
        host::Instance* const before = (*app)->instanceOf(fx);
        REQUIRE(before);
        CHECK(before->editorOpen());
        pump(200);
        (*app)->restartFx(fx);
        host::Instance* const after = (*app)->instanceOf(fx);
        REQUIRE(after);
        CHECK(after != before);
        pump(300);
        CHECK(windows.isOpen(fx));
        CHECK(windows.count() == 1);
        CHECK(after->editorOpen());
        windows.closeAll();
        CHECK_FALSE(after->editorOpen());
    }
    run("engine.stop", json::Value::object());
    std::filesystem::remove_all(options.workdir);
}

TEST_CASE("editor placements round-trip through the UI file's section") {
    auto platform = gui::Platform::start(gui::Platform::Driver::Offscreen);
    REQUIRE(platform);
    engine::AppOptions options;
    options.workdir = std::filesystem::temp_directory_path() /
                      ("supple-editor-layout-" + std::to_string(testProcessId()));
    auto app = engine::EngineApp::create(options);
    REQUIRE(app);
    gui::PluginWindows windows(**platform, **app, [] {});
    auto layout = json::parse(R"({"fx_3": {"x": 120, "y": -40, "width": 800, "height": 500},
                                  "fx_7": {"x": 5, "y": 6},
                                  "fx_8": {"x": 1, "y": 2, "width": 0, "height": 300},
                                  "tr_1": {"x": 1, "y": 2}, "fx_9": {"y": 2}, "junk": 3})");
    REQUIRE(layout);
    windows.loadLayout(*layout);
    // Effects only, with a position; a size only when both sides are given.
    const auto saved = json::parse(R"({"fx_3": {"x": 120, "y": -40, "width": 800, "height": 500},
                                       "fx_7": {"x": 5, "y": 6}, "fx_8": {"x": 1, "y": 2}})");
    REQUIRE(saved);
    CHECK(windows.saveLayout() == *saved);
    windows.loadLayout(json::Value::object());  // another project's file
    CHECK(windows.saveLayout() == json::Value::object());
    std::filesystem::remove_all(options.workdir);
}

TEST_CASE("an effect's editor reopens where it was moved and at its size (needs a display)") {
    if (!displayTestsEnabled()) {
        MESSAGE("skipped: set SUPPLE_GUI_DISPLAY_TEST=1 in a native desktop session");
        return;
    }
    auto platform = gui::Platform::start(gui::Platform::Driver::Native);
    REQUIRE(platform);
    engine::AppOptions options;
    options.workdir = std::filesystem::temp_directory_path() /
                      ("supple-editor-place-" + std::to_string(testProcessId()));
    options.pluginPath = {std::filesystem::path(SUPPLE_CLAP_DIR)};
    options.identity = host::HostIdentity{gui::test::name(), "Supple Audio", "", "0"};
    auto app = engine::EngineApp::create(options);
    REQUIRE(app);
    cmd::Registry registry;
    engine::registerEngineCommands(registry);
    cmd::Dispatcher dispatcher(registry, **app);
    (*app)->attachCommands(dispatcher);
    const cmd::Origin origin{cmd::Origin::Type::Test, "editor-place-test", {}};
    auto run = [&](const char* command, json::Value args) {
        auto r = dispatcher.execute({command, std::move(args), origin});
        REQUIRE_MESSAGE(r, (r ? "" : r.error().toString()));
        return *r;
    };
    json::Value start = json::Value::object();
    start["backend"] = "dummy";
    run("engine.start", std::move(start));
    // SUPPLE_EDITOR_ONLY=<name> picks the plug-in (default: the first resizable one).
    const char* only = std::getenv("SUPPLE_EDITOR_ONLY");
    json::Value track = json::Value::object();
    track["type"] = "channel";
    const std::string trackId = run("track.add", std::move(track))["track"].asString();
    json::Value list = run("plugin.list", json::Value::object());
    {
        gui::PluginWindows windows(**platform, **app, [] {});
        auto pump = [&](int ms) {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
            while (std::chrono::steady_clock::now() < until) {
                (*app)->tick();
                const int next = windows.service();
                SDL_Event e;
                if (SDL_WaitEventTimeout(&e, next < 0 ? 20 : std::min(next, 20))) windows.handle(e);
            }
        };
        auto editorWindow = [] {
            int count = 0;
            SDL_Window** all = SDL_GetWindows(&count);
            SDL_Window* w = count == 1 ? all[0] : nullptr;
            SDL_free(all);
            return w;
        };
        std::optional<model::Id> fx;
        bool resizable = false;
        for (const json::Value& p : list["plugins"].asArray()) {
            const json::Value* n = p.find("name");
            if (!n || !n->isString() || (only && n->asString() != only)) continue;
            if (const json::Value* f = p.find("format");
                f && f->isString() && f->asString() != "clap")
                continue;
            json::Value add = json::Value::object();
            add["track"] = trackId;
            add["plugin"] = p.find("id")->asString();
            auto added = dispatcher.execute({"fx.add", std::move(add), origin});
            if (!added) continue;
            const auto id = *model::Id::parse((*added)["fx"].asString());
            if (!windows.open(id)) continue;
            pump(100);
            resizable = (SDL_GetWindowFlags(editorWindow()) & SDL_WINDOW_RESIZABLE) != 0;
            if (resizable || only) {
                fx = id;
                MESSAGE("editor of " << n->asString() << (resizable ? " (resizable)" : ""));
                break;
            }
            windows.close(id);
        }
        if (!fx) {
            MESSAGE("skipped: no plug-in with an editor");
        } else {
            SDL_Window* w = editorWindow();
            REQUIRE(w);
            int x0 = 0, y0 = 0, w0 = 0, h0 = 0;
            SDL_GetWindowPosition(w, &x0, &y0);
            SDL_GetWindowSize(w, &w0, &h0);
            // The user moves it (and makes it larger), as a window manager would.
            SDL_SetWindowPosition(w, x0 + 97, y0 + 61);
            if (resizable) SDL_SetWindowSize(w, w0 + 120, h0 + 80);
            SDL_SyncWindow(w);
            pump(300);
            int x1 = 0, y1 = 0, w1 = 0, h1 = 0;
            SDL_GetWindowPosition(w, &x1, &y1);
            SDL_GetWindowSize(w, &w1, &h1);
            MESSAGE("opened at " << x0 << "," << y0 << " " << w0 << "x" << h0 << "; moved to " << x1
                                 << "," << y1 << " " << w1 << "x" << h1);
            windows.close(*fx);
            pump(100);
            REQUIRE(windows.open(*fx));
            w = editorWindow();
            REQUIRE(w);
            SDL_SyncWindow(w);
            pump(300);
            int x2 = 0, y2 = 0, w2 = 0, h2 = 0;
            SDL_GetWindowPosition(w, &x2, &y2);
            SDL_GetWindowSize(w, &w2, &h2);
            MESSAGE("reopened at " << x2 << "," << y2 << " " << w2 << "x" << h2);
            CHECK(x2 == x1);
            CHECK(y2 == y1);
            CHECK(w2 == w1);
            CHECK(h2 == h1);
            // And it is in the UI file's section.
            const json::Value saved = windows.saveLayout();
            REQUIRE(saved.find(fx->str()));
            CHECK(saved.find(fx->str())->find("x")->asInt() == x2);
            windows.closeAll();
        }
    }
    run("engine.stop", json::Value::object());
    std::filesystem::remove_all(options.workdir);
}

#if defined(__linux__)
TEST_CASE("editor FD readiness never reaches a later registration of the same descriptor") {
    int descriptors[2];
    REQUIRE(::pipe(descriptors) == 0);
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false, polledAgain = false;
    // The poll that holds the first registration pauses until the editor has
    // closed and a new one has registered the same descriptor number; then
    // it reports the descriptor readable.
    auto watcher = gui::FdWatcher::create(
        [] {}, {},
        [&](std::span<pollfd> fds) {
            const bool watching = std::any_of(
                fds.begin(), fds.end(), [&](const pollfd& p) { return p.fd == descriptors[0]; });
            std::unique_lock lock(mutex);
            if (watching && !entered) {
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&] { return release; });
                for (pollfd& p : fds)
                    if (p.fd == descriptors[0]) p.revents = POLLIN;
                return 1;
            }
            if (entered && release) {
                polledAgain = true;
                changed.notify_all();
            }
            lock.unlock();
            return ::poll(fds.data(), fds.size(), -1);
        });
    REQUIRE(watcher);
    (*watcher)->watch({{descriptors[0], CLAP_POSIX_FD_READ, 1}});
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }
    (*watcher)->watch({});                                         // the editor closes
    (*watcher)->watch({{descriptors[0], CLAP_POSIX_FD_READ, 2}});  // another reuses the fd
    {
        std::unique_lock lock(mutex);
        release = true;
        changed.notify_all();
        changed.wait(lock, [&] { return polledAgain; });
    }
    CHECK((*watcher)->takeReady().empty());  // the old poll's result was not delivered
    watcher->reset();
    ::close(descriptors[0]);
    ::close(descriptors[1]);
}

#endif

TEST_CASE("the shared GUI watcher wakes for app and model work without an editor") {
    auto platform = gui::Platform::start(gui::Platform::Driver::Offscreen);
    REQUIRE(platform);
    engine::AppOptions options;
    options.workdir = std::filesystem::temp_directory_path() /
                      ("supple-gui-wake-" + std::to_string(testProcessId()));
    auto app = engine::EngineApp::create(options);
    REQUIRE(app);
    rpc::ModelLoop loop;
    std::mutex mutex;
    std::condition_variable changed;
    bool notified = false;
    {
        gui::PluginWindows windows(**platform, **app, [&] {
            std::lock_guard lock(mutex);
            notified = true;
            changed.notify_one();
        });
        windows.setModelWake(loop.waitable());
        CHECK(windows.count() == 0);
        CHECK(windows.service() == -1);
        (*app)->workQueued();
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return notified; });
            notified = false;
        }
        CHECK(windows.dispatchFds());
        (*app)->tick();
        CHECK(windows.service() == -1);
        int calls = 0;
        loop.post([&] { ++calls; });
        {
            std::unique_lock lock(mutex);
            changed.wait(lock, [&] { return notified; });
        }
        CHECK(windows.dispatchFds());
        CHECK(loop.pump() == -1);
        CHECK(calls == 1);
        windows.suspendFdWatches();
    }
    std::filesystem::remove_all(options.workdir);
}
