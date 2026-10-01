// Plugin editors in their own windows (docs/specs/gui.md §4.5): an X11
// window per open editor, the plugin's GUI embedded in it (CLAP gui), its
// timers and file descriptors serviced from the UI thread (= the plugin's main
// thread), its resize requests applied. Editors follow their instance: if the
// engine replaces or drops it, the window closes.
#pragma once

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "supple/engine/app.h"
#include "supple/gui/window.h"

union SDL_Event;

namespace supple::gui {

// Polls plugin file descriptors on a thread and wakes the UI thread when one
// is ready; a ready descriptor is not polled again until the UI thread has
// dispatched it (so a level-triggered fd cannot spin).
class FdWatcher {
  public:
    explicit FdWatcher(std::function<void()> wake);
    ~FdWatcher();
    void watch(std::vector<std::pair<int, std::uint32_t>> fds);  // UI thread: the set to poll
    std::vector<std::pair<int, std::uint32_t>> takeReady();      // UI thread: (fd, CLAP flags)

  private:
    void run(std::stop_token stop);
    std::function<void()> wake_;
    std::mutex mutex_;
    std::vector<std::pair<int, std::uint32_t>> fds_, ready_;
    int pipe_[2] = {-1, -1};  // wakes poll() when the set changes or on shutdown
    std::jthread thread_;
};

class PluginWindows {
  public:
    PluginWindows(const Platform& platform, engine::EngineApp& app, std::function<void()> wake);
    ~PluginWindows();

    // Opens (or raises) the editor of an effect in the session.
    Status open(model::Id fx);
    // Opens an editor for an instance the caller owns (tests, previews).
    Status open(host::Instance& instance, const std::string& title);
    void close(model::Id fx);
    void closeAll();
    std::size_t count() const { return editors_.size(); }
    bool isOpen(model::Id fx) const;

    // Events addressed to editor windows; true when consumed.
    bool handle(const SDL_Event& e);
    // Runs due plugin timers, ready fds and resize/close requests; returns the
    // milliseconds to the next plugin deadline (-1: none).
    int service();

  private:
    struct Editor {
        std::optional<model::Id> fx;  // empty: an instance the caller owns
        host::Instance* instance = nullptr;
        std::unique_ptr<Window> window;
        bool resizable = false;
    };
    Status attach(Editor editor, const std::string& title);
    void drop(std::size_t index);
    const Platform& platform_;
    engine::EngineApp& app_;
    std::vector<Editor> editors_;
    FdWatcher watcher_;
};

}  // namespace supple::gui
