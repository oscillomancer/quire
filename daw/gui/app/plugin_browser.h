// The plugin browser (docs/specs/gui.md §4.5, §1.2): a searchable list over the
// scan cache with stock plugins and favourites first; Enter adds the selected
// plugin to the chain. Favourites are user config, never project state.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "supple/engine/app.h"
#include "supple/ui/node.h"

namespace supple::ui {
class Root;
class List;
class TextField;
class Label;
}  // namespace supple::ui

namespace supple::gui {

// $XDG_CONFIG_HOME/<slug>/favourites.json — a supple.favourites/1.0 document
// {"favourites": ["com.suppleaudio.eq", …]}. Order is the user's, not sorted.
struct Favourites {
    std::vector<std::string> ids;  // plugin ids
    static std::filesystem::path defaultFile(std::string_view slug);
    static Result<Favourites> load(const std::filesystem::path& file);  // absent: empty
    Status save(const std::filesystem::path& file) const;
    bool has(std::string_view id) const;
    bool toggle(std::string_view id);  // true when it is now a favourite
};

class PluginBrowser {
  public:
    PluginBrowser(engine::EngineApp&, cmd::Dispatcher& dispatcher, ui::Root& root,
                  std::filesystem::path favouritesFile);
    // Opens the browser; `onAdd` runs with a plugin id on Enter or a double click.
    void open(std::function<void(const std::string&)> onAdd);
    void close();
    bool isOpen() const { return page_ != nullptr; }

    // Frame pass: runs a rescan that was requested last frame (never inside the
    // click that asked for it, so "Scanning…" paints first).
    void tick();
    void refresh();  // re-read the scan cache and rebuild the rows
    void setStatus(std::string message);

    // State the panels and tests read back.
    const std::vector<std::string>& shown() const { return shown_; }
    const Favourites& favourites() const { return favourites_; }
    const std::filesystem::path& favouritesFile() const { return favouritesFile_; }
    // The action the browser adds through (set by open()).
    const std::function<void(const std::string&)>& onAdd() const { return onAdd_; }

  private:
    struct Entry {
        std::string id, name, vendor, format;
        bool instrument = false, semantic = false;
    };
    struct Row {
        const Entry* entry;
        bool favourite;
    };
    std::unique_ptr<ui::Node> buildPage();
    void sortAndFilter();
    void activate(std::size_t row);
    void runScan();

    cmd::Dispatcher& dispatcher_;
    ui::Root& root_;
    std::filesystem::path favouritesFile_;
    Favourites favourites_;
    std::vector<Entry> all_;
    std::vector<Row> rows_;           // display order, painted
    std::vector<std::string> shown_;  // ids, in display order (tests)
    std::function<void(const std::string&)> onAdd_;

    ui::Node* page_ = nullptr;
    ui::TextField* search_ = nullptr;
    ui::List* list_ = nullptr;
    ui::Label* status_ = nullptr;
    bool scanPending_ = false;
};

}  // namespace supple::gui
