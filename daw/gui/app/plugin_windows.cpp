#include "plugin_windows.h"

#include <SDL3/SDL.h>
#include <clap/clap.h>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>

#include "supple/model/session.h"

namespace supple::gui {

void applyEditorRequests(Window& window, const host::Instance::EditorRequests& requests) {
    if (requests.closed) return;
    if (requests.resize)
        window.setSize(static_cast<int>(requests.width), static_cast<int>(requests.height));
    if (requests.show) window.show();
    // Both requests can be coalesced by the host; hide wins in that case.
    if (requests.hide) window.hide();
}

// --- FdWatcher

Result<std::unique_ptr<FdWatcher>> FdWatcher::create(std::function<void()> wake, Pipe pipe,
                                                     Poll poll) {
    auto watcher = std::unique_ptr<FdWatcher>(new FdWatcher(std::move(wake)));
    const int result =
        pipe ? pipe(watcher->pipe_) : ::pipe2(watcher->pipe_, O_CLOEXEC | O_NONBLOCK);
    if (result != 0)
        return Error("gui.editor.fds",
                     std::string("cannot start editor FD service: ") + std::strerror(errno), {},
                     "release file descriptors and retry editor activation");
    watcher->poll_ = std::move(poll);
    try {
        watcher->thread_ =
            std::jthread([owner = watcher.get()](std::stop_token stop) { owner->run(stop); });
    } catch (const std::system_error& e) {
        return Error("gui.editor.fds", std::string("cannot start editor FD worker: ") + e.what());
    }
    return watcher;
}

std::optional<Error> FdWatcher::error() {
    std::lock_guard lock(mutex_);
    return error_;
}

FdWatcher::~FdWatcher() {
    thread_.request_stop();
    notify();
    if (thread_.joinable()) thread_.join();
    for (int fd : pipe_)
        if (fd >= 0) ::close(fd);
}

void FdWatcher::notify() {
    if (pipe_[1] < 0) return;
    while (::write(pipe_[1], "x", 1) < 0 && errno == EINTR) {
    }
    // EAGAIN means a previous wake is already pending.
}

void FdWatcher::watch(std::vector<std::pair<int, std::uint32_t>> fds) {
    {
        std::lock_guard lock(mutex_);
        // Ready descriptors stay out until dispatched.
        std::erase_if(fds, [&](const auto& f) {
            return std::any_of(ready_.begin(), ready_.end(),
                               [&](const auto& r) { return r.first == f.first; });
        });
        if (fds == fds_) return;
        fds_ = std::move(fds);
    }
    notify();
}

std::vector<std::pair<int, std::uint32_t>> FdWatcher::takeReady() {
    std::lock_guard lock(mutex_);
    return std::exchange(ready_, {});
}

void FdWatcher::run(std::stop_token stop) {
    while (!stop.stop_requested()) {
        std::vector<pollfd> polled;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [fd, flags] : fds_) {
                short events = 0;
                if (flags & CLAP_POSIX_FD_READ) events |= POLLIN;
                if (flags & CLAP_POSIX_FD_WRITE) events |= POLLOUT;
                polled.push_back({fd, events, 0});
            }
        }
        polled.push_back({pipe_[0], POLLIN, 0});
        if ((poll_ ? poll_(polled) : ::poll(polled.data(), polled.size(), -1)) < 0) {
            if (errno == EINTR) continue;
            {
                std::lock_guard lock(mutex_);
                error_ = Error("gui.editor.fds",
                               std::string("editor FD poll failed: ") + std::strerror(errno), {},
                               "close and reopen the editor after restarting FD service");
            }
            if (wake_) wake_();
            return;
        }
        if (polled.back().revents & POLLIN) {
            char buf[64];
            while (::read(pipe_[0], buf, sizeof buf) > 0) {
            }
        }
        bool any = false;
        {
            std::lock_guard lock(mutex_);
            for (std::size_t i = 0; i + 1 < polled.size(); ++i) {
                const short r = polled[i].revents;
                if (!r) continue;
                std::uint32_t flags = 0;
                if (r & POLLIN) flags |= CLAP_POSIX_FD_READ;
                if (r & POLLOUT) flags |= CLAP_POSIX_FD_WRITE;
                if (r & (POLLERR | POLLHUP | POLLNVAL)) flags |= CLAP_POSIX_FD_ERROR;
                ready_.emplace_back(polled[i].fd, flags);
                std::erase_if(fds_, [&](const auto& f) { return f.first == polled[i].fd; });
                any = true;
            }
        }
        if (any && wake_) wake_();
    }
}

// --- PluginWindows

PluginWindows::PluginWindows(const Platform& platform, engine::EngineApp& app,
                             std::function<void()> wake)
    : platform_(platform), app_(app) {
    auto watcher = FdWatcher::create(std::move(wake));
    if (watcher)
        watcher_ = std::move(*watcher);
    else
        watcherError_ = watcher.error();
}

PluginWindows::~PluginWindows() { closeAll(); }

bool PluginWindows::isOpen(model::Id fx) const {
    return std::any_of(editors_.begin(), editors_.end(),
                       [&](const Editor& e) { return e.fx == fx; });
}

Status PluginWindows::attach(Editor editor, const std::string& title) {
    if (watcher_)
        if (auto error = watcher_->error()) watcherError_ = std::move(error);
    if (!editor.instance->hasEditor())
        return Error("gui.editor", title + " has no editor this host can embed", "",
                     "the plugin offers no X11 editor (or its bridge process failed: restart it)");
    WindowOptions options;
    options.title = title;
    options.width = 640;
    options.height = 400;
    options.gl = false;
    options.resizable = false;
    // Floating above the app from the first map: hidden until the editor is in
    // and sized, owned by the app's window, a utility window.
    options.hidden = true;
    options.utility = true;
    options.parent = owner_;
    auto window = Window::open(platform_, options);
    if (!window) return std::move(window).error();
    const unsigned long parent = (*window)->x11Window();
    if (!parent) return Error("gui.editor", "plugin editors need the X11 video driver");
    auto size = editor.instance->openEditor(parent, static_cast<double>((*window)->pixelRatio()));
    restoreContext();
    if (!size) return std::move(size).error();
    if (watcherError_ && !editor.instance->watchedFds().empty()) {
        editor.instance->closeEditor();
        return *watcherError_;
    }
    if (size->width > 0 && size->height > 0)
        (*window)->setSize(static_cast<int>(size->width), static_cast<int>(size->height));
    (*window)->setResizable(size->resizable);
    (*window)->show();
    editor.window = std::move(*window);
    editor.resizable = size->resizable;
    editors_.push_back(std::move(editor));
    return {};
}

Status PluginWindows::open(model::Id fx) {
    for (const Editor& e : editors_)
        if (e.fx == fx) {
            e.window->show();
            SDL_RaiseWindow(e.window->sdl());
            return {};
        }
    const model::Session& s = app_.head();
    const model::FxInstance* info = s.fx.find(fx);
    if (!info) return Error("gui.editor", "no effect " + fx.str());
    host::Instance* instance = app_.instanceOf(fx);
    if (!instance)
        return Error("gui.editor", "the effect is not running", "",
                     "start the engine (engine.start) to open its editor");
    std::string title = info->plugin.name.empty() ? info->plugin.id : info->plugin.name;
    if (auto track = model::trackOfFx(s, fx))
        if (const model::Track* t = s.tracks.find(*track))
            title += " — " + (t->name.empty() ? track->str() : t->name);
    Editor editor;
    editor.fx = fx;
    editor.instance = instance;
    return attach(std::move(editor), title);
}

Status PluginWindows::open(host::Instance& instance, const std::string& title) {
    Editor editor;
    editor.instance = &instance;
    return attach(std::move(editor), title);
}

bool PluginWindows::reattach(Editor& editor) {
    host::Instance* instance = app_.instanceOf(*editor.fx);
    if (!instance || !instance->hasEditor()) return false;
    auto size = instance->openEditor(editor.window->x11Window(),
                                     static_cast<double>(editor.window->pixelRatio()));
    restoreContext();
    if (!size) return false;
    if (watcherError_ && !instance->watchedFds().empty()) {
        instance->closeEditor();
        return false;
    }
    editor.instance = instance;
    editor.resizable = size->resizable;
    if (size->width > 0 && size->height > 0)
        editor.window->setSize(static_cast<int>(size->width), static_cast<int>(size->height));
    editor.window->setResizable(size->resizable);
    return true;
}

void PluginWindows::drop(std::size_t index) {
    Editor& e = editors_[index];
    // The instance may already be gone (the engine replaced it): only close the
    // editor of the instance that is still the effect's.
    if (!e.fx || app_.instanceOf(*e.fx) == e.instance) e.instance->closeEditor();
    editors_.erase(editors_.begin() + static_cast<std::ptrdiff_t>(index));
    restoreContext();
}

Status PluginWindows::toggle(model::Id fx) {
    if (isOpen(fx)) {
        close(fx);
        return {};
    }
    return open(fx);
}

void PluginWindows::close(model::Id fx) {
    for (std::size_t i = 0; i < editors_.size(); ++i)
        if (editors_[i].fx == fx) return drop(i);
}

void PluginWindows::closeAll() {
    while (!editors_.empty()) drop(editors_.size() - 1);
}

bool PluginWindows::handle(const SDL_Event& e) {
    const SDL_WindowID id =
        e.window.windowID;  // the window events' field, shared layout for the ones we use
    if (e.type < SDL_EVENT_WINDOW_FIRST || e.type > SDL_EVENT_WINDOW_LAST) return false;
    for (std::size_t i = 0; i < editors_.size(); ++i) {
        Editor& ed = editors_[i];
        if (ed.window->id() != id) continue;
        if (ed.fx && app_.instanceOf(*ed.fx) != ed.instance) {
            if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) drop(i);  // else service() reopens
            return true;
        }
        if (e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            drop(i);
        } else if (e.type == SDL_EVENT_WINDOW_RESIZED && ed.resizable &&
                   ed.instance->editorOpen()) {
            auto accepted = ed.instance->resizeEditor(static_cast<std::uint32_t>(e.window.data1),
                                                      static_cast<std::uint32_t>(e.window.data2));
            if (accepted && (accepted->width != static_cast<std::uint32_t>(e.window.data1) ||
                             accepted->height != static_cast<std::uint32_t>(e.window.data2)))
                ed.window->setSize(static_cast<int>(accepted->width),
                                   static_cast<int>(accepted->height));
            restoreContext();
        }
        return true;
    }
    return false;
}

int PluginWindows::service() {
    if (watcher_)
        if (auto error = watcher_->error()) watcherError_ = std::move(error);
    if (watcherError_) {
        if (!watcherErrorReported_ && onError) {
            onError(*watcherError_);
            watcherErrorReported_ = true;
        }
        for (std::size_t i = editors_.size(); i-- > 0;)
            if ((!editors_[i].fx || app_.instanceOf(*editors_[i].fx) == editors_[i].instance) &&
                !editors_[i].instance->watchedFds().empty())
                drop(i);
    }
    // The engine closed the editor of an instance it replaced (fx.restart, a
    // VST3 key change): it reopens in the same window on the new instance.
    // An effect that is gone, or whose new instance cannot, loses its window.
    for (std::size_t i = editors_.size(); i-- > 0;) {
        Editor& e = editors_[i];
        if (e.fx && app_.instanceOf(*e.fx) != e.instance && !reattach(e)) {
            editors_.erase(editors_.begin() + static_cast<std::ptrdiff_t>(i));
            restoreContext();
            continue;
        }
        const auto requests = e.instance->takeEditorRequests();
        if (requests.closed) {
            editors_.erase(editors_.begin() + static_cast<std::ptrdiff_t>(i));
            continue;
        }
        applyEditorRequests(*e.window, requests);
    }
    // Ready descriptors go to their plugin, then are watched again.
    for (const auto& [fd, flags] : watcher_ && !watcherError_
                                       ? watcher_->takeReady()
                                       : std::vector<std::pair<int, std::uint32_t>>{})
        for (Editor& e : editors_)
            for (const auto& [wfd, wflags] : e.instance->watchedFds())
                if (wfd == fd) e.instance->onFd(fd, flags);
    std::vector<std::pair<int, std::uint32_t>> fds;
    int next = -1;
    for (Editor& e : editors_) {
        for (const auto& f : e.instance->watchedFds()) fds.push_back(f);
        int due = e.instance->nextTimerMs();
        if (due == 0) {
            e.instance->serviceMainThread();  // runs the due timers
            due = e.instance->nextTimerMs();
        }
        if (due >= 0) next = next < 0 ? due : std::min(next, due);
    }
    if (watcher_ && !watcherError_) watcher_->watch(std::move(fds));
    if (!editors_.empty()) restoreContext();
    return next;
}

}  // namespace supple::gui
