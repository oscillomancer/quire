// The FX chain window (docs/specs/gui.md §4.5): a popup panel for one track's
// chain — per-effect name, bypass and offline (distinct, fx.set), wet/dry,
// latency, editor, reorder and remove — plus the input chain, the plugin
// browser and a semantic parameter tab. Every change is a command on the bus.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "fx_params.h"
#include "plugin_browser.h"
#include "supple/engine/app.h"
#include "supple/model/ids.h"
#include "supple/ui/node.h"

namespace supple::ui {
class Root;
class Label;
class Button;
class TextField;
}  // namespace supple::ui

namespace supple::gui {

struct UiState;

class FxWindow {
  public:
    struct Hooks {
        // Open (or close, when open) a plugin's own editor (PluginWindows in the app);
        // errors are shown.
        std::function<Status(model::Id)> openEditor;
    };

    FxWindow(engine::EngineApp& app, cmd::Dispatcher& dispatcher, ui::Root& root, Hooks hooks,
             std::filesystem::path favouritesFile);
    ~FxWindow();

    Status open(model::Id track);
    // A take's own chain (take FX, gap log #22): the item plays through it
    // before its track's input.
    Status openTake(model::Id take);
    std::optional<model::Id> take() const { return take_; }
    // The `ui.fx.chain` action: a track id in `prompt`, else the selected track.
    Status openFor(const UiState& ui, const json::Value& prompt);
    // The same, with the plugin browser open on top (ui.fx.add).
    Status openToAdd(const UiState& ui, const json::Value& prompt);
    void close();
    bool isOpen() const { return page_ != nullptr; }

    // Frame pass: refresh after changes made elsewhere (agents, undo).
    void tick();
    void refresh();
    void setStatus(std::string message);

    // State the tests read back.
    model::Id track() const { return track_; }
    model::Id selected() const { return selected_; }
    // The badge a row shows: how the effect runs (in-process / shared bridge /
    // isolated), offline, and failed. Pure, so the composition is testable
    // without an engine that can crash a bridge.
    static std::string badgeText(const model::FxInstance& fx, bool failed);
    // The pins button's text (ADR-0020): "pins 1/2", "3/4 → 1/2", "key 3/4".
    static std::string pinsText(const model::PinMap& pins);
    // The pins menu of a track-chain effect: input pair, output pair, key.
    void openPinsMenu(model::Id fx, const ui::Rect& anchor);
    // Engine-reported failures (engine.status failed_fx), by effect id.
    const std::set<std::string>& failed() const { return failed_; }
    void select(model::Id fx);
    void showParamsTab();
    void showChainTab();
    std::vector<model::Id> chain() const;
    std::vector<model::Id> inputChain() const;
    PluginBrowser& browser() { return *browser_; }

  private:
    enum class Tab { Chain, Params };
    bool hasSelected() const { return selected_.n != 0; }
    std::unique_ptr<ui::Node> buildPage();
    void buildChain(ui::Node& body);
    void buildParams(ui::Node& body);
    void buildRow(ui::Node& body, model::Id fx, std::size_t index, bool input);
    Status dispatch(const char* command, json::Value args);
    std::string nameOf(model::Id fx) const;
    std::string latencyOf(model::Id fx);
    void moveFx(model::Id fx, int delta);
    void addFx(const std::string& pluginId);
    void promptSaveChain();
    void saveChain();
    void loadChainMenu(bool replace);
    void choosePresetChain(bool input);
    void checkFailures();  // engine.status failed_fx, at most twice a second

    engine::EngineApp& app_;
    cmd::Dispatcher& dispatcher_;
    ui::Root& root_;
    Hooks hooks_;
    std::unique_ptr<PluginBrowser> browser_;
    ParamPanel params_;

    model::Id track_, selected_;
    std::optional<model::Id> take_;  // set: the window shows that take's chain
    Tab tab_ = Tab::Chain;
    std::string gesture_;  // the active slider drag
    std::uint64_t gestureSerial_ = 0;
    std::string pendingChain_ = "main";           // where Add plugin… inserts
    std::map<std::string, std::string> latency_;  // measured once per effect
    std::set<std::string> failed_;                // engine.status failed_fx
    double failureCheckMs_ = -1e9;
    std::string presetChain_ = "main";
    ui::Button* presetChainButton_ = nullptr;
    ui::Node* savePrompt_ = nullptr;
    ui::TextField* saveName_ = nullptr;
    ui::Node* page_ = nullptr;
    ui::Node* body_ = nullptr;
    ui::Node* tabsRow_ = nullptr;
    ui::Label* title_ = nullptr;
    ui::Label* status_ = nullptr;
    std::uint64_t shownVersion_ = ~std::uint64_t{0};
};

}  // namespace supple::gui
