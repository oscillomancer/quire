// The File actions (new, open, save, save as, import audio) with the system's
// file dialogs (SDL3: the desktop portal or a native dialog, asynchronous —
// the UI thread never waits on them) and a confirmation before unsaved
// changes are discarded. Every change goes through the bus.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "app_actions.h"
#include "supple/ui/root.h"

struct SDL_Window;

namespace supple::gui {

// Native callbacks may race shutdown; posted completions run on the UI thread.
class DialogLifetime {
  public:
    using Paths = std::vector<std::filesystem::path>;
    using Post = std::function<void(std::function<void()>)>;
    DialogLifetime() = default;
    DialogLifetime(const DialogLifetime&) = delete;
    DialogLifetime& operator=(const DialogLifetime&) = delete;
    ~DialogLifetime();
    std::function<void(Paths)> guard(Post post, std::function<void(Paths)> done) const;

  private:
    struct State {
        std::mutex mutex;
        bool active = true;
    };
    std::shared_ptr<State> state_ = std::make_shared<State>();
};

class FileActions {
  public:
    using Post = std::function<void(std::function<void()>)>;
    FileActions(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
                engine::EngineApp& app, UiState& ui, SDL_Window* window, Post post);

    bool dirty() const;  // changes since the last save/open/new
    // Runs `then` now if nothing would be lost; otherwise asks Save / Don't
    // save / Cancel first.
    void confirmDiscard(const std::string& what, std::function<void()> then);
    // For tests: what a dialog would have returned.
    void openPath(const std::filesystem::path& path);
    void saveAs(const std::filesystem::path& path);
    void import(const std::vector<std::filesystem::path>& files);

  private:
    Result<json::Value> exec(const std::string& command, json::Value args);
    void markSaved();
    void showError(const std::string& message);
    ui::Root& root_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    UiState& ui_;
    SDL_Window* window_;
    Post post_;
    DialogLifetime dialogs_;
    std::uint64_t savedVersion_ = 0;
    std::string savedBranch_;
};

}  // namespace supple::gui
