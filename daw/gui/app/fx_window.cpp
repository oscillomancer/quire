#include "fx_window.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cstdio>
#include <map>

#include "app_actions.h"
#include "supple/ui/controls.h"
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kUiOrigin{cmd::Origin::Type::Ui, "gui", {}};

// A chain row: paints its selection, selects the effect on a left press and
// reorders with Alt+Up / Alt+Down (fx.move).
class Row : public ui::Node {
  public:
    Row(std::string id, std::function<void()> onSelect, std::function<void(int)> onMove = {})
        : Node(std::move(id)), onSelect_(std::move(onSelect)), onMove_(std::move(onMove)) {
        interactive = true;
        focusable = true;
        layout.dir = ui::Layout::Dir::Column;
        layout.height = ui::SizeRule::content();
        layout.gap = 2;
        layout.padding = ui::Insets::xy(4, 2);
    }
    bool selected = false;
    void paint(ui::Canvas& c) override {
        if (selected)
            c.fillRound(bounds(), c.theme().radius("s"),
                        c.theme().color("selection").withAlpha(0.55f));
    }
    bool onPointer(const ui::PointerEvent& e) override {
        if (e.type == ui::PointerEvent::Type::Down && e.button == 1) {
            if (onSelect_) onSelect_();
            return true;
        }
        return e.type == ui::PointerEvent::Type::Up;
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (!onMove_ || !(e.mods & ui::kAlt)) return false;
        if (e.key == SDLK_UP) {
            onMove_(-1);
            return true;
        }
        if (e.key == SDLK_DOWN) {
            onMove_(1);
            return true;
        }
        return false;
    }

  private:
    std::function<void()> onSelect_;
    std::function<void(int)> onMove_;
};

ui::Node& line(ui::Node& row, const std::string& id) {
    auto& n = row.emplace<ui::Node>(id);
    n.layout.dir = ui::Layout::Dir::Row;
    n.layout.height = ui::SizeRule::fixed(26);
    n.layout.gap = 6;
    n.layout.align = ui::Layout::Align::Center;
    return n;
}

}  // namespace

FxWindow::FxWindow(engine::EngineApp& app, cmd::Dispatcher& dispatcher, ui::Root& root, Hooks hooks,
                   std::filesystem::path favouritesFile)
    : app_(app),
      dispatcher_(dispatcher),
      root_(root),
      hooks_(std::move(hooks)),
      browser_(std::make_unique<PluginBrowser>(app, dispatcher, root, std::move(favouritesFile))),
      params_(dispatcher, root) {
    params_.onError = [this](const std::string& message) { setStatus(message); };
}

FxWindow::~FxWindow() = default;

std::unique_ptr<ui::Node> FxWindow::buildPage() {
    auto page = std::make_unique<ui::Node>("fx");
    page_ = page.get();
    page->layout.dir = ui::Layout::Dir::Column;
    page->layout.width = ui::SizeRule::fixed(840);
    page->layout.height = ui::SizeRule::fixed(560);
    page->layout.padding = ui::Insets::all(12);
    page->layout.gap = 8;

    auto& header = page->emplace<ui::Node>("fx.header");
    header.layout.dir = ui::Layout::Dir::Row;
    header.layout.height = ui::SizeRule::fixed(30);
    header.layout.gap = 8;
    header.layout.align = ui::Layout::Align::Center;
    title_ = &header.emplace<ui::Label>("fx.title", "FX", ui::Font::UiBold, "title");
    title_->layout.width = ui::SizeRule::fraction();
    header.emplace<ui::Button>("fx.close", "Close", [this] { close(); });

    tabsRow_ = &page->emplace<ui::Node>("fx.tabs");
    tabsRow_->layout.dir = ui::Layout::Dir::Row;
    tabsRow_->layout.height = ui::SizeRule::fixed(28);
    tabsRow_->layout.gap = 6;
    tabsRow_->emplace<ui::Button>("fx.tab.chain", "Chain", [this] { showChainTab(); });
    tabsRow_->emplace<ui::Button>("fx.tab.params", "Parameters", [this] { showParamsTab(); });
    tabsRow_->emplace<ui::Button>("fx.add", "Add plugin…", [this] {
        pendingChain_ = "main";
        browser_->open([this](const std::string& pluginId) { addFx(pluginId); });
    });
    status_ = &tabsRow_->emplace<ui::Label>("fx.status", "", ui::Font::Ui, "caption", "ink-quiet");
    status_->layout.width = ui::SizeRule::fraction();

    auto& scroll = page->emplace<ui::ScrollView>("fx.body");
    scroll.layout.width = ui::SizeRule::fraction();
    scroll.layout.height = ui::SizeRule::fraction();
    body_ = &scroll.content();
    body_->layout.gap = 4;
    return page;
}

Status FxWindow::open(model::Id track) {
    if (!app_.head().tracks.find(track))
        return Error("track.unknown", "no track " + track.str(), "/track", "use track.list");
    track_ = track;
    selected_ = {};
    tab_ = Tab::Chain;
    latency_.clear();  // a fresh window measures each effect again
    if (page_) {       // already open: switch track in place
        refresh();
        return {};
    }
    auto page = buildPage();
    root_.openPopup(std::move(page), {}, ui::Placement::Center, [this] {
        page_ = nullptr;
        body_ = nullptr;
        tabsRow_ = nullptr;
        title_ = nullptr;
        status_ = nullptr;
    });
    refresh();
    return {};
}

Status FxWindow::openFor(const UiState& ui, const json::Value& prompt) {
    std::string text;
    if (prompt.isString()) text = prompt.asString();
    if (text.empty() && !ui.selectedTracks.empty()) text = ui.selectedTracks.front();
    if (text.empty())
        return Error("gui.track", "no track given and nothing is selected", "/track",
                     "select a track, or give a track id like tr_1");
    auto id = model::Id::parse(text);
    if (!id)
        return Error("gui.track", "not a track id: " + text, "/track", "use a track id like tr_1");
    return open(*id);
}

void FxWindow::close() {
    if (page_) root_.closePopup();
}

void FxWindow::setStatus(std::string message) {
    if (status_) status_->setText(std::move(message));
}

std::vector<model::Id> FxWindow::chain() const {
    const model::Track* t = app_.head().tracks.find(track_);
    return t ? t->chain : std::vector<model::Id>{};
}

std::vector<model::Id> FxWindow::inputChain() const {
    const model::Track* t = app_.head().tracks.find(track_);
    return t ? t->inputChain : std::vector<model::Id>{};
}

std::string FxWindow::nameOf(model::Id fx) const {
    const model::FxInstance* inst = app_.head().fx.find(fx);
    if (!inst) return fx.str();
    return inst->label.empty() ? (inst->plugin.name.empty() ? inst->plugin.id : inst->plugin.name)
                               : inst->label;
}

std::string FxWindow::latencyOf(model::Id fx) {
    // Measured once per effect and cached; the plugin reports it in samples.
    auto it = latency_.find(fx.str());
    if (it != latency_.end()) return it->second;
    const model::FxInstance* inst = app_.head().fx.find(fx);
    if (!inst) return "-";
    std::string text = "-";
    if (auto latency = app_.fxLatencySamples(fx, *inst)) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%u spl", *latency);
        text = buf;
    }
    latency_[fx.str()] = text;
    return text;
}

Status FxWindow::dispatch(const char* command, json::Value args) {
    // gesture_ carries an in-progress slider drag, so its edits coalesce into
    // one undo step (Origin::gesture); it is empty between gestures.
    const cmd::Origin origin{cmd::Origin::Type::Ui, "gui", {}, gesture_};
    auto result = dispatcher_.execute({command, std::move(args), origin});
    if (!result) {
        setStatus(std::string(command) + ": " + result.error().message);
        return std::move(result).error();
    }
    return {};
}

void FxWindow::tick() {
    browser_->tick();
    if (page_ && app_.head().version != shownVersion_) refresh();
}

void FxWindow::select(model::Id fx) {
    selected_ = fx;
    if (tab_ == Tab::Params)
        refresh();
    else
        root_.damageAll();
}

void FxWindow::showChainTab() {
    tab_ = Tab::Chain;
    refresh();
}

void FxWindow::showParamsTab() {
    tab_ = Tab::Params;
    refresh();
}

void FxWindow::refresh() {
    if (!page_) return;
    const std::string focusId = root_.focused() ? root_.focused()->id() : std::string();
    const model::Track* track = app_.head().tracks.find(track_);
    if (!track) {
        setStatus("the track is gone");
        return;
    }
    if (title_) title_->setText("FX: " + (track->name.empty() ? track_.str() : track->name));
    body_->clear();
    if (hasSelected() && !app_.head().fx.find(selected_)) selected_ = {};
    if (!hasSelected()) {
        if (!track->chain.empty())
            selected_ = track->chain.front();
        else if (!track->inputChain.empty())
            selected_ = track->inputChain.front();
    }
    if (tab_ == Tab::Chain)
        buildChain(*body_);
    else
        buildParams(*body_);
    shownVersion_ = app_.head().version;
    root_.relayout();
    root_.damageAll();
    if (!focusId.empty())
        if (ui::Node* node = root_.find(focusId)) root_.focus(node);
}

void FxWindow::buildChain(ui::Node& body) {
    const model::Track* track = app_.head().tracks.find(track_);
    if (!track) return;
    // The input chain (fx.add {chain:"input"}): always shown, so it can be
    // started from here.
    auto& inputHeader = body.emplace<ui::Node>("fx.input.header");
    inputHeader.layout.dir = ui::Layout::Dir::Row;
    inputHeader.layout.height = ui::SizeRule::fixed(22);
    inputHeader.layout.gap = 6;
    inputHeader.layout.align = ui::Layout::Align::Center;
    inputHeader.emplace<ui::Label>("fx.section.input", "Input chain (recorded input)",
                                   ui::Font::UiBold, "caption", "ink-quiet");
    inputHeader.emplace<ui::Button>("fx.add.input", "Add input FX", [this] {
        pendingChain_ = "input";
        browser_->open([this](const std::string& pluginId) { addFx(pluginId); });
    });
    for (std::size_t i = 0; i < track->inputChain.size(); ++i)
        buildRow(body, track->inputChain[i], i, true);
    body.emplace<ui::Label>("fx.section.chain", "Chain", ui::Font::UiBold, "caption", "ink-quiet");
    if (track->chain.empty())
        body.emplace<ui::Label>("fx.empty", "No effects yet — use “Add plugin…”.", ui::Font::Ui,
                                "small", "ink-quiet");
    for (std::size_t i = 0; i < track->chain.size(); ++i) buildRow(body, track->chain[i], i, false);
}

void FxWindow::buildRow(ui::Node& body, model::Id fx, std::size_t index, bool input) {
    const model::FxInstance* inst = app_.head().fx.find(fx);
    if (!inst) return;
    const std::string prefix = input ? "ifx." : "fx.";
    const std::string id = fx.str();
    auto& row = body.emplace<Row>(
        prefix + "row." + id, [this, fx] { select(fx); },
        input ? std::function<void(int)>{}
              : std::function<void(int)>{[this, fx](int delta) { moveFx(fx, delta); }});
    row.selected = selected_ == fx;

    {
        auto& l1 = line(row, prefix + "line1." + id);
        char number[8];
        std::snprintf(number, sizeof number, "%zu.", index + 1);
        l1.emplace<ui::Label>(prefix + "number." + id, number, ui::Font::Mono, "caption",
                              "ink-quiet");
        auto& name =
            l1.emplace<ui::Label>(prefix + "name." + id, nameOf(fx), ui::Font::Ui, "small");
        name.layout.width = ui::SizeRule::fixed(160);
        auto& latency = l1.emplace<ui::Label>(prefix + "latency." + id, latencyOf(fx),
                                              ui::Font::Mono, "caption", "ink-quiet");
        latency.layout.width = ui::SizeRule::fixed(80);
        l1.emplace<ui::Button>(prefix + "bypass." + id, inst->bypass ? "bypassed" : "bypass",
                               [this, fx, bypass = inst->bypass] {
                                   json::Value args = json::Value::object();
                                   args["fx"] = fx.str();
                                   args["bypass"] = !bypass;
                                   if (dispatch("fx.set", std::move(args))) refresh();
                               });
        l1.emplace<ui::Button>(prefix + "offline." + id, inst->offline ? "offlined" : "offline",
                               [this, fx, offline = inst->offline] {
                                   json::Value args = json::Value::object();
                                   args["fx"] = fx.str();
                                   args["offline"] = !offline;
                                   if (dispatch("fx.set", std::move(args))) refresh();
                               });
        l1.emplace<ui::Button>(prefix + "editor." + id, "UI", [this, fx] {
            if (hooks_.openEditor) {
                if (auto opened = hooks_.openEditor(fx); !opened)
                    setStatus("editor: " + opened.error().message);
            }
        });
        if (!input) {
            l1.emplace<ui::Button>(prefix + "up." + id, "^", [this, fx] { moveFx(fx, -1); });
            l1.emplace<ui::Button>(prefix + "down." + id, "v", [this, fx] { moveFx(fx, 1); });
        }
        l1.emplace<ui::Button>(prefix + "params." + id, "params", [this, fx] {
            selected_ = fx;
            showParamsTab();
        });
        l1.emplace<ui::Button>(prefix + "remove." + id, "Remove", [this, fx] {
            json::Value args = json::Value::object();
            args["fx"] = fx.str();
            if (dispatch("fx.remove", std::move(args))) {
                if (selected_ == fx) selected_ = {};
                refresh();
            }
        });
    }
    {
        auto& l2 = line(row, prefix + "line2." + id);
        auto* wetLabel = &l2.emplace<ui::Label>(prefix + "wetlabel." + id, "", ui::Font::Mono,
                                                "caption", "ink-quiet");
        wetLabel->layout.width = ui::SizeRule::fixed(76);
        auto* wet = &l2.emplace<ui::Slider>(prefix + "wet." + id);
        wet->onChange = [this, fx, wetLabel](double normalized, ui::Gesture g) {
            if (g == ui::Gesture::End) {
                gesture_.clear();
                return;
            }
            if (g == ui::Gesture::Begin) gesture_ = "fx.wet:" + fx.str();
            const double percent = 100.0 * normalized;
            char text[24];
            std::snprintf(text, sizeof text, "wet %3.0f%%", percent);
            wetLabel->setText(text);
            json::Value args = json::Value::object();
            args["fx"] = fx.str();
            args["wet"] = percent;
            (void)dispatch("fx.set", std::move(args));
        };
        wet->setValue(inst->wet / 100.0);
        wet->setDefault(1.0);  // a double click returns to fully wet
        wet->layout.width = ui::SizeRule::fixed(220);
        char wetText[24];
        std::snprintf(wetText, sizeof wetText, "wet %3.0f%%", inst->wet);
        wetLabel->setText(wetText);
    }
}

void FxWindow::buildParams(ui::Node& body) {
    auto& chooser = line(body, "fx.params.chooser");
    chooser.layout.height = ui::SizeRule::fixed(28);
    std::string label = "No effect selected";
    if (hasSelected()) label = "Effect: " + nameOf(selected_);
    auto& name = chooser.emplace<ui::Label>("fx.params.name", label, ui::Font::UiBold, "small");
    name.layout.width = ui::SizeRule::fraction();
    chooser.emplace<ui::Button>("fx.params.prev", "<", [this] {
        const std::vector<model::Id> c = chain();
        auto it = std::find(c.begin(), c.end(), selected_);
        if (it != c.end() && it != c.begin())
            select(*(it - 1));
        else if (!c.empty())
            select(c.back());
    });
    chooser.emplace<ui::Button>("fx.params.next", ">", [this] {
        const std::vector<model::Id> c = chain();
        auto it = std::find(c.begin(), c.end(), selected_);
        if (it != c.end() && it + 1 != c.end())
            select(*(it + 1));
        else if (!c.empty())
            select(c.front());
    });
    if (!hasSelected()) {
        body.emplace<ui::Label>("fx.params.note", "Add or select an effect first.", ui::Font::Ui,
                                "small", "ink-quiet");
        return;
    }
    if (auto built = params_.build(body, selected_); !built)
        body.emplace<ui::Label>("fx.params.error", built.error().message, ui::Font::Ui, "small",
                                "clip");
}

void FxWindow::moveFx(model::Id fx, int delta) {
    const std::vector<model::Id> c = chain();
    auto it = std::find(c.begin(), c.end(), fx);
    if (it == c.end()) return;
    const auto from = static_cast<std::ptrdiff_t>(it - c.begin());
    const auto to = from + delta;
    if (to < 0 || to >= static_cast<std::ptrdiff_t>(c.size())) return;
    json::Value args = json::Value::object();
    args["fx"] = fx.str();
    args["index"] = static_cast<std::int64_t>(to);
    if (dispatch("fx.move", std::move(args))) refresh();
}

void FxWindow::addFx(const std::string& pluginId) {
    json::Value args = json::Value::object();
    args["track"] = track_.str();
    args["plugin"] = pluginId;
    if (pendingChain_ == "input") args["chain"] = "input";
    auto result = dispatcher_.execute({"fx.add", std::move(args), kUiOrigin});
    if (!result) {
        setStatus("fx.add: " + result.error().message);
        return;
    }
    if (const json::Value* made = result->find("fx"); made && made->isString())
        if (auto id = model::Id::parse(made->asString())) selected_ = *id;
    setStatus("added " + pluginId);
    refresh();
}

}  // namespace supple::gui
