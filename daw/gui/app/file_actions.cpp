#include "file_actions.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <memory>

#include "supple/ui/scroll_view.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kOrigin{cmd::Origin::Type::Ui, "gui", {}};

// A dialog's answer, delivered on the UI thread through `post`.
struct DialogRequest {
    FileActions::Post post;
    std::function<void(std::vector<std::filesystem::path>)> done;
};

void SDLCALL onDialog(void* userdata, const char* const* files, int) {
    std::unique_ptr<DialogRequest> req(static_cast<DialogRequest*>(userdata));
    std::vector<std::filesystem::path> paths;
    if (files)
        for (const char* const* f = files; *f; ++f) paths.emplace_back(*f);
    if (paths.empty()) return;  // cancelled (or an error SDL reported)
    req->post([done = std::move(req->done), paths = std::move(paths)] { done(paths); });
}

const SDL_DialogFileFilter kProjectFilter[] = {{"Projects", "json"}, {"All files", "*"}};
const SDL_DialogFileFilter kAudioFilter[] = {{"Audio", "wav;flac;mp3"}, {"All files", "*"}};

}  // namespace

FileActions::FileActions(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
                         engine::EngineApp& app, UiState& ui, SDL_Window* window, Post post)
    : root_(root),
      actions_(actions),
      dispatcher_(dispatcher),
      app_(app),
      ui_(ui),
      window_(window),
      post_(std::move(post)) {
    markSaved();
    actions_.addUi("ui.file.new", "File: new project", [this] {
        confirmDiscard("Start a new project?", [this] {
            if (exec("project.new", json::Value::object())) markSaved();
        });
    });
    actions_.addUi("ui.file.open", "File: open project…", [this] {
        confirmDiscard("Open another project?", [this] {
            auto* req = new DialogRequest{post_, [this](auto paths) { openPath(paths.front()); }};
            SDL_ShowOpenFileDialog(onDialog, req, window_, kProjectFilter, 2, nullptr, false);
        });
    });
    actions_.addUi("ui.file.save", "File: save", [this] {
        if (app_.projectPath().empty()) return (void)actions_.run("ui.file.save_as");
        if (exec("project.save", json::Value::object())) markSaved();
    });
    actions_.addUi("ui.file.save_as", "File: save as…", [this] {
        auto* req = new DialogRequest{post_, [this](auto paths) { saveAs(paths.front()); }};
        const std::string here = app_.projectPath().empty() ? "" : app_.projectPath().string();
        SDL_ShowSaveFileDialog(onDialog, req, window_, kProjectFilter, 2,
                               here.empty() ? nullptr : here.c_str());
    });
    actions_.addUi("ui.file.import", "File: import audio…", [this] {
        auto* req = new DialogRequest{post_, [this](auto paths) { import(paths); }};
        SDL_ShowOpenFileDialog(onDialog, req, window_, kAudioFilter, 2, nullptr, true);
    });
}

Result<json::Value> FileActions::exec(const std::string& command, json::Value args) {
    auto r = dispatcher_.execute({command, std::move(args), kOrigin});
    if (!r) showError(command + ": " + r.error().message);
    return r;
}

void FileActions::markSaved() {
    savedVersion_ = app_.head().version;
    savedBranch_ = app_.versions().branch();
}

bool FileActions::dirty() const {
    return app_.head().version != savedVersion_ || app_.versions().branch() != savedBranch_;
}

void FileActions::showError(const std::string& message) {
    if (actions_.onError) actions_.onError(message);
}

void FileActions::openPath(const std::filesystem::path& path) {
    json::Value args = json::Value::object();
    args["path"] = path.string();
    if (exec("project.open", std::move(args))) {
        ui_.selectedTracks.clear();
        ui_.selectedItems.clear();
        markSaved();
    }
}

void FileActions::saveAs(const std::filesystem::path& path) {
    std::filesystem::path p = path;
    if (p.extension().empty()) p += ".json";
    json::Value args = json::Value::object();
    args["path"] = p.string();
    if (exec("project.save", std::move(args))) markSaved();
}

void FileActions::import(const std::vector<std::filesystem::path>& files) {
    // One file: onto the selected track (or a new one) at the playhead. Several:
    // a new track each, all at the playhead — one undo step.
    const actions::Context ctx = actions_.context();
    auto imported = dispatcher_.transact("Import audio", kOrigin, [&]() -> Result<json::Value> {
        for (std::size_t i = 0; i < files.size(); ++i) {
            std::string track;
            if (files.size() == 1 && !ui_.selectedTracks.empty()) {
                track = ui_.selectedTracks.front();
            } else {
                json::Value add = json::Value::object();
                add["name"] = files[i].stem().string();
                auto t = dispatcher_.execute({"track.add", std::move(add), kOrigin});
                if (!t) return t;
                track = (*t)["track"].asString();
            }
            json::Value item = json::Value::object();
            item["track"] = track;
            item["position"] = ctx.playheadBeats;
            item["audio_file"] = files[i].string();
            if (auto r = dispatcher_.execute({"item.add", std::move(item), kOrigin}); !r) return r;
        }
        return json::Value::object();
    });
    if (!imported) showError("import: " + imported.error().message);
}

void FileActions::confirmDiscard(const std::string& what, std::function<void()> then) {
    if (!dirty()) return then();
    auto form = std::make_unique<ui::Node>("file.confirm");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(420);
    form->layout.height = ui::SizeRule::fixed(130);
    form->layout.padding = ui::Insets::all(14);
    form->layout.gap = 10;
    form->emplace<ui::Label>("file.confirm.title", what, ui::Font::UiBold, "body");
    form->emplace<ui::TextBlock>("file.confirm.text", "The project has unsaved changes.",
                                 ui::Font::Ui, "small", "ink-quiet");
    auto& row = form->emplace<ui::Node>("file.confirm.buttons");
    row.layout.dir = ui::Layout::Dir::Row;
    row.layout.height = ui::SizeRule::fixed(28);
    row.layout.gap = 8;
    auto shared = std::make_shared<std::function<void()>>(std::move(then));
    // Each button copies what it needs first: closePopup destroys the button.
    row.emplace<ui::Button>("file.confirm.save", "Save", [this, shared] {
        auto keep = shared;
        root_.closePopup();
        if (app_.projectPath().empty()) {
            // Save as first; continue after the dialog answers.
            auto* req = new DialogRequest{post_, [this, keep](auto paths) {
                                              saveAs(paths.front());
                                              if (!dirty()) (*keep)();
                                          }};
            SDL_ShowSaveFileDialog(onDialog, req, window_, kProjectFilter, 2, nullptr);
            return;
        }
        if (exec("project.save", json::Value::object())) {
            markSaved();
            (*keep)();
        }
    });
    row.emplace<ui::Button>("file.confirm.discard", "Don't save", [this, shared] {
        auto keep = shared;
        root_.closePopup();
        (*keep)();
    });
    row.emplace<ui::Button>("file.confirm.cancel", "Cancel", [this] { root_.closePopup(); });
    root_.openPopup(std::move(form), {}, ui::Placement::Center);
}

}  // namespace supple::gui
