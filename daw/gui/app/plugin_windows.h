// Plugin editors in their own windows (docs/specs/gui.md §4.5): an X11
// window per open editor, the plugin's GUI embedded in it (CLAP gui), its
// timers and file descriptors serviced from the UI thread (= the plugin's main
// thread), its resize requests applied. Editors follow their instance: if the
// engine replaces or drops it, the window closes.
#pragma once

#include <poll.h>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "supple/engine/app.h"
#include "supple/gui/window.h"

union SDL_Event;

namespace supple::gui {

// Applies coalesced host requests without destroying a hidden editor. Closed
// editors are removed by the owner; close takes precedence over other flags.
void applyEditorRequests(Window& window, const host::Instance::EditorRequests& requests);

// Polls plugin file descriptors on a thread and wakes the UI thread when one
// is ready; a ready descriptor is not polled again until the UI thread has
// dispatched it (so a level-triggered fd cannot spin).
class FdWatcher {
  public:
    using Pipe = std::function<int(int*)>;
    using Poll = std::function<int(std::span<pollfd>)>;
    static Result<std::unique_ptr<FdWatcher>> create(std::function<void()> wake, Pipe pipe = {},
                                                     Poll poll = {});
    ~FdWatcher();
    std::optional<Error> error();
    void watch(std::vector<std::pair<int, std::uint32_t>> fds);  // UI thread: the set to poll
    std::vector<std::pair<int, std::uint32_t>> takeReady();      // UI thread: (fd, CLAP flags)

  private:
    explicit FdWatcher(std::function<void()> wake) : wake_(std::move(wake)) {}
    void run(std::stop_token stop);
    void notify();
    Poll poll_;
    std::function<void()> wake_;
    std::mutex mutex_;
    std::vector<std::pair<int, std::uint32_t>> fds_, ready_;
    int pipe_[2] = {-1, -1};  // wakes poll() when the set changes or on shutdown
    std::jthread thread_;
    std::optional<Error> error_;
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
    // Opens the effect's editor, or closes it when it is open (the FX chain's
    // "UI" and the mixer's slot buttons).
    Status toggle(model::Id fx);
    void closeAll();
    // The app's window: editors float above it, and its GL context is made
    // current again after plugin code ran (editors draw with their own).
    void setOwner(const Window* owner) { owner_ = owner; }
    std::size_t count() const { return editors_.size(); }
    bool isOpen(model::Id fx) const;
    std::function<void(const Error&)> onError;

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
    void restoreContext() const {
        if (owner_) owner_->forceCurrent();
    }
    const Platform& platform_;
    const Window* owner_ = nullptr;
    engine::EngineApp& app_;
    std::vector<Editor> editors_;
    std::unique_ptr<FdWatcher> watcher_;
    std::optional<Error> watcherError_;
    bool watcherErrorReported_ = false;
};

}  // namespace supple::gui
