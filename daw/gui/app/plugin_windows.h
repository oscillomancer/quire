// Plugin editors in their own windows (docs/specs/gui.md §4.5): an X11
// window per open editor, the plugin's GUI embedded in it (CLAP gui), its
// timers and file descriptors serviced from the UI thread (= the plugin's main
// thread), its resize requests applied. On instance replacement, the editor
// reopens in the same window; removing the effect closes its window.
#pragma once

#include <poll.h>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include "supple/engine/app.h"
#include "supple/gui/window.h"
#include "supple/json/value.h"

union SDL_Event;

namespace supple::gui {

// Applies coalesced host requests without destroying a hidden editor. Closed
// editors are removed by the owner; close takes precedence over other flags.
void applyEditorRequests(Window& window, const host::Instance::EditorRequests& requests);

// Polls app/model and plugin descriptors on one thread and wakes the UI when one
// is ready; a ready descriptor is not polled again until the UI thread has
// dispatched it (so a level-triggered fd cannot spin).
// One descriptor with its registration's serial (`owner`), so old readiness
// never reaches a later one that reuses the descriptor number.
struct FdWatch {
    int fd = -1;
    std::uint32_t flags = 0;  // CLAP_POSIX_FD_*; in takeReady(), what was ready
    std::uint64_t owner = 0;
    bool operator==(const FdWatch&) const = default;
};

class FdWatcher {
  public:
    using Pipe = std::function<int(int*)>;
    using Poll = std::function<int(std::span<pollfd>)>;
    static Result<std::unique_ptr<FdWatcher>> create(std::function<void()> wake, Pipe pipe = {},
                                                     Poll poll = {});
    ~FdWatcher();
    std::optional<Error> error();
    void watch(std::vector<FdWatch> fds);  // UI thread: the set to poll
    std::vector<FdWatch> takeReady();      // UI thread: what was ready, per registration

  private:
    explicit FdWatcher(std::function<void()> wake) : wake_(std::move(wake)) {}
    void run(std::stop_token stop);
    void notify();
    Poll poll_;
    std::function<void()> wake_;
    std::mutex mutex_;
    std::vector<FdWatch> fds_, ready_;
    std::uint64_t generation_ = 0;  // bumped when the set changes: older polls are stale
    int pipe_[2] = {-1, -1};        // wakes poll() when the set changes or on shutdown
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
    bool hasFocus(model::Id fx) const;  // its editor's window has the input focus
    std::function<void(const Error&)> onError;

    // Where each effect's editor was last: its position and, when the editor
    // is resizable, its size. An editor opens there again; the project's UI
    // file keeps them ({fx id: {x, y, width, height}}).
    json::Value saveLayout() const;
    void loadLayout(const json::Value& layout);

    // Events addressed to editor windows; true when consumed.
    bool handle(const SDL_Event& e);
    // Applies editor requests and rebuilds the shared watch set. App instance
    // timers run in EngineApp::tick; caller-owned local editors add their own
    // timer deadlines here. Returns their nearest delay (-1: none).
    int service();
    // Shared watcher: dispatch the previous pass's app/editor readiness, then
    // suspend before model commands can close/reuse descriptors. service()
    // rebuilds the set afterwards, even when no editor is open.
    bool dispatchFds();
    void suspendFdWatches();
    void setModelWakeFd(int fd) { modelWakeFd_ = fd; }
    std::optional<Error> fdError() { return watcher_ ? watcher_->error() : watcherError_; }

  private:
    struct Editor {
        std::optional<model::Id> fx;  // empty: an instance the caller owns
        host::Instance* instance = nullptr;
        std::uint64_t serial = 0;  // this registration of the instance's descriptors
        std::unique_ptr<Window> window;
        bool resizable = false;
    };
    struct Placement {
        int x = 0, y = 0;
        int width = 0, height = 0;  // 0: the editor's own size (not resizable)
    };
    static std::optional<Placement> placementOf(const Editor& editor);
    void remember(const Editor& editor);
    void place(Editor& editor);  // before the window is shown
    Status attach(Editor editor, const std::string& title);
    bool reattach(Editor& editor);  // the effect's current instance, in the same window
    void drop(std::size_t index);
    void restoreContext() const {
        if (owner_) owner_->forceCurrent();
    }
    const Platform& platform_;
    const Window* owner_ = nullptr;
    engine::EngineApp& app_;
    std::vector<Editor> editors_;
    std::uint64_t nextSerial_ = 1;
    std::uint64_t nextServiceSerial_ = 4;  // even; editors have odd serials
    struct ServiceWatch {
        engine::EngineApp::ServiceFd source;
        std::uint64_t serial;
    };
    std::vector<ServiceWatch> serviceWatches_;
    int modelWakeFd_ = -1;
    std::map<model::Id, Placement> placements_;
    std::unique_ptr<FdWatcher> watcher_;
    std::optional<Error> watcherError_;
    bool watcherErrorReported_ = false;
};

}  // namespace supple::gui
