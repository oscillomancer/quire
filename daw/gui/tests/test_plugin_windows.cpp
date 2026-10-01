// Plugin editors embedded in X11 windows. Needs a display: runs only with
// SUPPLE_GUI_DISPLAY_TEST=1 (a window appears for about a second).
#include <SDL3/SDL.h>
#include <doctest/doctest.h>

#include <unistd.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <thread>

#include "plugin_windows.h"
#include "supple/base/environment.h"
#include "supple/host/clap_host.h"

using namespace supple;

TEST_CASE("the stock plugins offer embeddable X11 editors (no display needed)") {
    const std::filesystem::path dir = SUPPLE_CLAP_DIR;
    auto plugins = host::scan(std::span<const std::filesystem::path>(&dir, 1));
    REQUIRE(plugins);
    int withEditor = 0;
    for (const host::PluginInfo& info : *plugins) {
        auto instance =
            host::Instance::create(info, host::HostIdentity{"Quire test", "Supple Audio", "", "0"});
        REQUIRE(instance);
        withEditor += (*instance)->hasEditor();
        CHECK_FALSE((*instance)->editorOpen());
        CHECK((*instance)->nextTimerMs() >= -1);
    }
    MESSAGE(withEditor << " of " << plugins->size() << " plugins have editors");
    CHECK(withEditor >= 8);  // the stock effects and Orrery ship editors
}

TEST_CASE("an editor opens in its window, services its timers and closes") {
    if (!environmentFlagEnabled("SUPPLE_GUI_DISPLAY_TEST") || !std::getenv("DISPLAY")) {
        MESSAGE("skipped: set SUPPLE_GUI_DISPLAY_TEST=1 with an X11 DISPLAY");
        return;
    }
    auto platform = gui::Platform::start(gui::Platform::Driver::X11);
    REQUIRE(platform);
    engine::AppOptions options;
    options.workdir = std::filesystem::temp_directory_path() /
                      ("supple-editor-test-" + std::to_string(::getpid()));
    auto app = engine::EngineApp::create(options);
    REQUIRE(app);
    const std::filesystem::path dir = SUPPLE_CLAP_DIR;
    auto plugins = host::scan(std::span<const std::filesystem::path>(&dir, 1));
    REQUIRE(plugins);
    gui::PluginWindows windows(**platform, **app, [] {});
    for (const host::PluginInfo& info : *plugins) {
        auto instance =
            host::Instance::create(info, host::HostIdentity{"Quire test", "Supple Audio", "", "0"});
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
