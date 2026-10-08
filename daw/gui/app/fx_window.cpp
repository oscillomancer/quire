#include "fx_window.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <optional>
#include <utility>

#include "app_actions.h"
#include "supple/ui/controls.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/root.h"
#include "supple/ui/scroll_view.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

const cmd::Origin kUiOrigin{cmd::Origin::Type::Ui, "gui", {}};

double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

bool draggingSlider(const ui::Node& node) {
    if (const auto* slider = dynamic_cast<const ui::Slider*>(&node); slider && slider->dragging())
        return true;
    for (const auto& child : node.children())
        if (draggingSlider(*child)) return true;
    return false;
}

// A chain row: paints its selection, selects the effect on a left press
// (with the press's modifiers and click count), and reorders by dragging
// (onDrag) or with Alt+Up / Alt+Down (fx.move).
class Row : public ui::Node {
  public:
    enum class Phase { Move, Drop, Cancel };
    Row(std::string id, std::function<void(unsigned mods, int clicks)> onSelect,
        std::function<void(int)> onMove = {})
        : Node(std::move(id)), onSelect_(std::move(onSelect)), onMove_(std::move(onMove)) {
        interactive = true;
        focusable = true;
        layout.dir = ui::Layout::Dir::Column;
        layout.height = ui::SizeRule::content();
        layout.gap = 2;
        layout.padding = ui::Insets::xy(4, 2);
    }
    bool selected = false;
    bool dragged = false;                          // being dragged: dimmed
    bool markerTop = false, markerBottom = false;  // where a dragged row would land
    std::function<void(Phase, ui::Point)> onDrag;  // unset: the row stays put
    void paint(ui::Canvas& c) override {
        const ui::Theme& t = c.theme();
        if (selected) c.fillRound(bounds(), t.radius("s"), t.color("selection").withAlpha(0.55f));
        if (dragged) c.fillRound(bounds(), t.radius("s"), t.color("ground").withAlpha(0.5f));
        const ui::Rect b = bounds();
        if (markerTop) c.fillRect({b.x, b.y - 3, b.w, 3}, t.color("focus"));
        if (markerBottom) c.fillRect({b.x, b.bottom(), b.w, 3}, t.color("focus"));
    }
    bool onPointer(const ui::PointerEvent& e) override {
        using T = ui::PointerEvent::Type;
        if (e.type == T::Down && e.button == 1) {
            pressAt_ = e.pos;
            moving_ = false;
            if (auto callback = onSelect_)
                callback(e.mods, e.clicks);  // may rebuild the rows: nothing after
            return true;
        }
        if (e.type == T::Move && onDrag && pressAt_) {
            if (!moving_ && std::hypot(e.pos.x - pressAt_->x, e.pos.y - pressAt_->y) < 4)
                return true;
            if (!moving_ && root()) root()->focus(this);  // Escape cancels
            moving_ = true;
            auto callback = onDrag;
            callback(Phase::Move, e.pos);
            return true;
        }
        if (e.type == T::Up && e.button == 1) {
            const bool was = std::exchange(moving_, false);
            pressAt_.reset();
            if (was && onDrag) {
                auto callback = onDrag;
                callback(Phase::Drop, e.pos);  // rebuilds the rows: nothing after
            }
            return true;
        }
        return false;
    }
    bool onKey(const ui::KeyEvent& e) override {
        if (e.key == SDLK_ESCAPE && moving_) {
            moving_ = false;
            pressAt_.reset();
            if (root()) root()->capture(nullptr);
            if (auto callback = onDrag) callback(Phase::Cancel, {});
            return true;
        }
        if (!onMove_ || !(e.mods & ui::kAlt)) return false;
        if (e.key == SDLK_UP) {
            auto callback = onMove_;
            callback(-1);
            return true;
        }
        if (e.key == SDLK_DOWN) {
            auto callback = onMove_;
            callback(1);
            return true;
        }
        return false;
    }

  private:
    std::function<void(unsigned, int)> onSelect_;
    std::function<void(int)> onMove_;
    std::optional<ui::Point> pressAt_;
    bool moving_ = false;
};

// Modifier-aware buttons and menu activation keep scripted input and SDL
// input on the same path (Shift chooses replacement at either click).
class LoadChainButton : public ui::Button {
  public:
    explicit LoadChainButton(std::function<void(bool)> open)
        : Button("fx.chain.load", "Load chain…") {
        onClick = [this, open = std::move(open)] { open(shift_); };
    }
    bool onPointer(const ui::PointerEvent& e) override {
        shift_ = (e.mods & ui::kShift) != 0;
        return Button::onPointer(e);
    }
    bool onKey(const ui::KeyEvent& e) override {
        shift_ = (e.mods & ui::kShift) != 0;
        return Button::onKey(e);
    }

  private:
    bool shift_ = false;
};

// The saved chains: a click loads one (Shift replaces), a right click offers
// to delete it (`remove` gets the preset's file name, true when it is gone).
class ChainMenu : public ui::List {
  public:
    ChainMenu(ui::Root& root, json::Value chains, bool replace,
              std::function<void(const std::string&, bool)> load,
              std::function<bool(const std::string&)> remove)
        : List("fx.chain.menu"),
          root_(root),
          chains_(std::move(chains)),
          replace_(replace),
          load_(std::move(load)),
          remove_(std::move(remove)) {
        rowHeight = 28;
        layout.width = ui::SizeRule::fixed(360);
        setCount(chains_.size());
        paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool) {
            const auto& entry = chains_.asArray()[i];
            c.text({r.x + 8, r.y, r.w - 16, r.h}, entry.find("name")->asString(), ui::Font::Ui,
                   c.theme().type("small"), c.theme().color("ink"));
            c.text({r.x, r.y, r.w - 10, r.h},
                   std::to_string(entry.find("effects")->asInt()) + " FX", ui::Font::Mono,
                   c.theme().type("caption"), c.theme().color("ink-quiet"),
                   ui::kRight | ui::kMiddle);
        };
        onActivate = [this](std::size_t i) {
            const std::string path = chains_.asArray()[i]["path"].asString();
            auto callback = load_;
            const bool replacing = replace_ || shift_;
            root_.closePopup();
            callback(path, replacing);
        };
        onContext = [this](std::size_t i, ui::Point at) {
            const auto& entry = chains_.asArray()[i];
            ui::MenuItem item;
            item.label = "Delete \"" + entry.find("name")->asString() + "\"";
            item.run = [this, i,
                        file = std::filesystem::path(entry.find("path")->asString()).stem()] {
                if (remove_(file.string())) removeRow(i);
            };
            root_.openPopup(ui::makeMenu("fx.chain.menu.context", {std::move(item)},
                                         [this] { root_.closePopup(); }),
                            ui::Rect{at.x, at.y, 0, 0}, ui::Placement::Below);
        };
    }
    bool onPointer(const ui::PointerEvent& e) override {
        shift_ = (e.mods & ui::kShift) != 0;
        if (e.type == ui::PointerEvent::Type::Up && e.button == 1 && selected()) {
            if (bounds().contains(e.pos)) {
                const auto row = *selected();
                auto activate = onActivate;
                activate(row);
            }
            return true;
        }
        return List::onPointer(e);
    }
    bool onKey(const ui::KeyEvent& e) override {
        shift_ = (e.mods & ui::kShift) != 0;
        if ((e.key == SDLK_RETURN || e.key == SDLK_KP_ENTER) && selected()) {
            const auto row = *selected();
            auto activate = onActivate;
            activate(row);  // closes this menu; do not read members afterwards
            return true;
        }
        return List::onKey(e);
    }

  private:
    void removeRow(std::size_t i) {
        auto& rows = chains_.asArray();
        rows.erase(rows.begin() + static_cast<std::ptrdiff_t>(i));
        select(std::nullopt);
        setCount(rows.size());
        root_.relayout();
        root_.damageAll();
        if (rows.empty()) root_.closePopup();  // destroys this menu: nothing after
    }
    ui::Root& root_;
    json::Value chains_;
    bool replace_, shift_ = false;
    std::function<void(const std::string&, bool)> load_;
    std::function<bool(const std::string&)> remove_;
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
    params_.openLearn = hooks_.openLearn;
}

bool FxWindow::shows(model::Id fx) const {
    if (!page_) return false;
    for (const auto& list : {chain(), take_ ? std::vector<model::Id>{} : inputChain()})
        if (std::find(list.begin(), list.end(), fx) != list.end()) return true;
    return false;
}

bool FxWindow::hasFocus() const {
    for (const ui::Node* n = root_.focused(); n; n = n->parent())
        if (n == page_) return true;
    return false;
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
    presetChainButton_ = &header.emplace<ui::Button>(
        "fx.chain.target", "Main chain", [this] { choosePresetChain(presetChain_ != "input"); });
    header.emplace<ui::Button>("fx.chain.save", "Save chain…", [this] {
        if (take_) return setStatus("Chain presets are for track chains");
        if (savePrompt_ && savePrompt_->visible) return hideSavePrompt();
        promptSaveChain();
    });
    header.emplace<LoadChainButton>([this](bool replace) {
        if (take_) return setStatus("Chain presets are for track chains");
        loadChainMenu(replace);
    });
    header.emplace<ui::Button>("fx.close", "Close", [this] { close(); });

    savePrompt_ = &page->emplace<ui::Node>("fx.chain.prompt");
    savePrompt_->layout.dir = ui::Layout::Dir::Row;
    savePrompt_->layout.height = ui::SizeRule::fixed(28);
    savePrompt_->layout.gap = 8;
    savePrompt_->visible = false;
    saveName_ = &savePrompt_->emplace<ui::TextField>("fx.chain.name", "", "Chain preset name");
    saveName_->layout.width = ui::SizeRule::fraction();
    saveName_->onSubmit = [this](const std::string&) { saveChain(); };
    saveName_->onCancel = [this] { hideSavePrompt(); };
    savePrompt_->emplace<ui::Button>("fx.chain.commit", "Save", [this] { saveChain(); });

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
    // What "FX: last touched parameter (MIDI CC)" moves (ADR-0027).
    touched_ =
        &tabsRow_->emplace<ui::Label>("fx.touched", "", ui::Font::Ui, "caption", "ink-quiet");
    touched_->layout.width = ui::SizeRule::content();
    touchedSerial_ = ~std::uint64_t{0};

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
    pendingChain_ = "main";
    take_.reset();
    selected_ = {};
    picked_.clear();
    tab_ = Tab::Chain;
    presetChain_ = "main";
    if (presetChainButton_) presetChainButton_->setText("Main chain");
    if (savePrompt_) {
        savePrompt_->visible = false;
        root_.releaseWithin(savePrompt_);
    }
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
        presetChainButton_ = nullptr;
        savePrompt_ = nullptr;
        saveName_ = nullptr;
    });
    refresh();
    return {};
}

Status FxWindow::openTake(model::Id take) {
    const model::Session& s = app_.head();
    if (!s.takes.find(take))
        return Error("take.unknown", "no take " + take.str(), "/take", "use take.list");
    for (const auto& e : s.items)
        if (std::find(e.value->takes.begin(), e.value->takes.end(), take) != e.value->takes.end()) {
            SUPPLE_TRY(open(e.value->track));
            take_ = take;
            refresh();
            return {};
        }
    return Error("take.orphan", take.str() + " belongs to no item", "/take");
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

// The plugin browser on its own (the strips' "+"): what it adds goes last in
// the track's chain; a chain window open on that track shows it.
Status FxWindow::openToAdd(const UiState& ui, const json::Value& prompt) {
    std::string text;
    if (prompt.isString()) text = prompt.asString();
    if (const json::Value* t = prompt.isObject() ? prompt.find("track") : nullptr;
        t && t->isString())
        text = t->asString();
    if (text.empty() && !ui.selectedTracks.empty()) text = ui.selectedTracks.front();
    const auto id = model::Id::parse(text);
    if (!id || !app_.head().tracks.find(*id))
        return Error("gui.track",
                     text.empty() ? "no track given and nothing is selected" : "no track " + text,
                     "/track", "select a track, or give a track id like tr_1");
    const model::Id track = *id;
    browser_->open([this, track](const std::string& pluginId) { addFxTo(track, pluginId); },
                   "Add plugin to " + model::trackLabel(app_.head(), track));
    return {};
}

void FxWindow::addFxTo(model::Id track, const std::string& pluginId) {
    json::Value args = json::Value::object();
    args["track"] = track.str();
    args["plugin"] = pluginId;
    auto result = dispatcher_.execute({"fx.add", std::move(args), kUiOrigin});
    if (!result) {
        const std::string message = "fx.add: " + result.error().message;
        if (page_) setStatus(message);
        if (hooks_.onError) hooks_.onError(message);
        return;
    }
    if (!page_ || track_ != track || take_) return;
    if (const json::Value* made = result->find("fx"); made && made->isString())
        if (auto made_id = model::Id::parse(made->asString())) {
            selected_ = *made_id;
            picked_.clear();
        }
    setStatus("added " + pluginId);
    refresh();
}

void FxWindow::close() {
    if (page_) root_.closePopup();
}

void FxWindow::setStatus(std::string message) {
    if (status_) status_->setText(std::move(message));
}

std::vector<model::Id> FxWindow::chain() const {
    if (take_) {
        const model::Take* tk = app_.head().takes.find(*take_);
        return tk ? tk->chain : std::vector<model::Id>{};
    }
    const model::Track* t = app_.head().tracks.find(track_);
    return t ? t->chain : std::vector<model::Id>{};
}

std::vector<model::Id> FxWindow::inputChain() const {
    if (take_) return {};
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
    const auto* model = app_.head().fx.find(fx);
    if (!model) {
        latency_.erase(fx.str());
        return "-";
    }
    auto* live = app_.instanceOf(fx);
    const double rate = app_.head().project->sampleRate;
    const auto observed = live && live->active() ? std::optional(live->latency()) : std::nullopt;
    auto& cached = latency_[fx.str()];
    if (cached.model == model && cached.live == live && cached.rate == rate &&
        (!observed || cached.samples == observed))
        return cached.text;
    cached = {model, live, rate, observed, "-"};
    auto measured = observed ? Result<std::uint32_t>(*observed) : app_.fxLatencySamples(fx, *model);
    if (measured) {
        cached.samples = *measured;
        cached.text = std::to_string(*measured) + " spl";
    }
    return cached.text;
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

std::string FxWindow::badgeText(const model::FxInstance& fx, bool failed) {
    std::string text;
    switch (fx.isolation) {
        case model::Isolation::Dedicated:
            text = "isolated";
            break;
        case model::Isolation::SharedBridge:
            text = "shared bridge";
            break;
        case model::Isolation::InProcess:
        case model::Isolation::Inherit:
            text = "in-process";
            break;
    }
    if (fx.offline) text += " · offline";
    if (failed) text += " · failed";
    return text;
}

// engine.status lists the isolated instances whose process crashed or hung.
void FxWindow::checkFailures() {
    const double now = nowMs();
    if (now - failureCheckMs_ < 500.0) return;
    failureCheckMs_ = now;
    auto status = dispatcher_.execute({"engine.status", json::Value::object(), kUiOrigin});
    if (!status) return;
    std::set<std::string> failed;
    if (const json::Value* list = status->find("failed_fx"); list && list->isArray())
        for (const json::Value& id : list->asArray())
            if (id.isString()) failed.insert(id.asString());
    if (failed == failed_) return;
    failed_ = std::move(failed);
    refresh();
}

void FxWindow::tick() {
    browser_->tick();
    checkFailures();
    if (page_ && touched_) {
        const auto t = app_.lastTouched();
        const std::uint64_t serial = t ? t->serial : 0;
        if (serial != touchedSerial_) {
            touchedSerial_ = serial;
            touched_->setText(t ? "Last touched: " + nameOf(t->fx) + " · " +
                                      app_.paramName(t->fx, t->param)
                                : std::string());
            root_.relayout();
            root_.damageAll();
        }
    }
    if (app_.projectGeneration() != projectGeneration_) {
        latency_.clear();
        failed_.clear();
        selected_ = {};
        if (page_) root_.releaseWithin(page_);
    }
    if (page_ &&
        (app_.projectGeneration() != projectGeneration_ || app_.head().version != shownVersion_))
        refresh();
    if (page_ && body_)
        for (const auto& [id, cached] : latency_) {
            const auto fx = model::Id::parse(id);
            if (!fx || !app_.head().fx.find(*fx)) continue;
            const auto text = latencyOf(*fx);
            for (const char* prefix : {"fx.latency.", "ifx.latency."})
                if (auto* label = dynamic_cast<ui::Label*>(body_->find(prefix + id)))
                    label->setText(text);
        }
}

void FxWindow::select(model::Id fx) {
    selected_ = fx;
    anchor_ = fx;
    picked_.clear();
    const auto input = inputChain();
    choosePresetChain(std::find(input.begin(), input.end(), fx) != input.end());
    if (tab_ == Tab::Params)
        refresh();
    else {
        if (body_)
            for (const auto& child : body_->children())
                if (auto* row = dynamic_cast<Row*>(child.get()))
                    row->selected =
                        row->id() == "fx.row." + fx.str() || row->id() == "ifx.row." + fx.str();
        root_.damageAll();
    }
}

bool FxWindow::picked(model::Id fx) const {
    return fx == selected_ || picked_.count(fx.str()) != 0;
}

std::vector<model::Id> FxWindow::selection() const {
    std::vector<model::Id> out;
    for (const model::Id& fx : rowOrder())
        if (picked(fx)) out.push_back(fx);
    return out;
}

// The rows top to bottom: the input chain, then the chain (a take: its chain).
std::vector<model::Id> FxWindow::rowOrder() const {
    std::vector<model::Id> order = take_ ? std::vector<model::Id>{} : inputChain();
    for (const model::Id& fx : chain()) order.push_back(fx);
    return order;
}

std::vector<model::Id> FxWindow::targets(model::Id fx) const {
    const std::vector<model::Id> all = selection();
    if (all.size() > 1 && picked(fx)) return all;
    return {fx};
}

Status FxWindow::setEach(model::Id fx, const char* field, bool value) {
    const std::vector<model::Id> each = targets(fx);
    auto set = dispatcher_.transact(
        std::string(field) == "bypass" ? "Bypass effects" : "Offline effects", kUiOrigin,
        [&]() -> Result<json::Value> {
            for (const model::Id& one : each) {
                json::Value args = json::Value::object();
                args["fx"] = one.str();
                args[field] = value;
                SUPPLE_TRY(dispatcher_.execute({"fx.set", std::move(args), kUiOrigin}));
            }
            return json::Value::object();
        });
    if (!set) {
        setStatus(std::string("fx.set: ") + set.error().message);
        return std::move(set).error();
    }
    return {};
}

// A press on a row: alone it selects the effect (a double click also opens
// its editor, only that one); Ctrl adds or takes it out of the selection;
// Shift selects every row from the last one clicked.
void FxWindow::press(model::Id fx, unsigned mods, int clicks) {
    const bool ctrl = (mods & ui::kCtrl) != 0, shift = (mods & ui::kShift) != 0;
    if (!ctrl && !shift) {
        if (clicks >= 2) {
            if (selected_ != fx || !picked_.empty()) select(fx);
            if (hooks_.openEditor)
                if (auto opened = hooks_.openEditor(fx); !opened)
                    setStatus("editor: " + opened.error().message);
            return;
        }
        select(fx);
        return;
    }
    if (shift && hasSelected()) {
        const std::vector<model::Id> order = rowOrder();
        const model::Id from = anchor_.n != 0 ? anchor_ : selected_;
        auto a = std::find(order.begin(), order.end(), from);
        auto b = std::find(order.begin(), order.end(), fx);
        if (a != order.end() && b != order.end()) {
            if (a > b) std::swap(a, b);
            picked_.clear();
            for (auto it = a; it <= b; ++it) picked_.insert(it->str());
        }
        selected_ = fx;
    } else {
        if (picked_.empty() && hasSelected()) picked_.insert(selected_.str());
        if (picked_.erase(fx.str()) != 0) {
            if (selected_ == fx) selected_ = picked_.empty() ? model::Id{} : selection().front();
        } else {
            picked_.insert(fx.str());
            selected_ = fx;
        }
        anchor_ = fx;
    }
    if (body_)
        for (const auto& child : body_->children())
            if (auto* row = dynamic_cast<Row*>(child.get())) {
                const std::string& rid = row->id();
                const auto dot = rid.rfind('.');
                const auto id = model::Id::parse(rid.substr(dot + 1));
                row->selected = id && picked(*id);
            }
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
    if (body_ && draggingSlider(*body_)) return;  // keep capture and previews until release
    gesture_.clear();
    const std::string focusId = root_.focused() ? root_.focused()->id() : std::string();
    const model::Track* track = app_.head().tracks.find(track_);
    if (!track) {
        body_->clear();
        selected_ = {};
        shownVersion_ = app_.head().version;
        projectGeneration_ = app_.projectGeneration();
        setStatus("the track is gone");
        return;
    }
    const std::string trackName = track->name.empty() ? track_.str() : track->name;
    if (take_ && !app_.head().takes.find(*take_)) take_.reset();  // the take is gone
    if (title_)
        title_->setText(take_ ? "Take FX: " + trackName + " / " + take_->str()
                              : "FX: " + trackName);
    body_->clear();
    if (hasSelected() && !app_.head().fx.find(selected_)) selected_ = {};
    for (auto it = picked_.begin(); it != picked_.end();) {
        const auto id = model::Id::parse(*it);
        it = id && app_.head().fx.find(*id) ? std::next(it) : picked_.erase(it);
    }
    if (!hasSelected()) {
        const auto main = chain(), input = inputChain();
        if (!main.empty())
            selected_ = main.front();
        else if (!input.empty())
            selected_ = input.front();
    }
    if (tab_ == Tab::Chain)
        buildChain(*body_);
    else
        buildParams(*body_);
    shownVersion_ = app_.head().version;
    projectGeneration_ = app_.projectGeneration();
    root_.relayout();
    root_.damageAll();
    if (!focusId.empty())
        if (ui::Node* node = root_.find(focusId)) root_.focus(node);
}

void FxWindow::buildChain(ui::Node& body) {
    const model::Track* track = app_.head().tracks.find(track_);
    if (!track) return;
    if (take_) {  // a take has one chain, and no input chain
        const auto takeChain = chain();
        body.emplace<ui::Label>("fx.section.take", "Take chain (before the track's input)",
                                ui::Font::UiBold, "caption", "ink-quiet");
        if (takeChain.empty())
            body.emplace<ui::Label>("fx.empty", "No take FX yet — use “Add plugin…”.", ui::Font::Ui,
                                    "small", "ink-quiet");
        for (std::size_t i = 0; i < takeChain.size(); ++i) buildRow(body, takeChain[i], i, false);
        return;
    }
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
        prefix + "row." + id, [this, fx](unsigned mods, int clicks) { press(fx, mods, clicks); },
        input ? std::function<void(int)>{}
              : std::function<void(int)>{[this, fx](int delta) { moveFx(fx, delta); }});
    row.selected = picked(fx);
    if (!input)  // drag the row to reorder the chain
        row.onDrag = [this, fx](Row::Phase phase, ui::Point p) {
            dragRow(fx, static_cast<int>(phase), p);
        };
    const bool isFailed = failed_.count(id) != 0;

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
        if (isFailed)
            l1.emplace<ui::Button>(prefix + "restart." + id, "Restart", [this, fx] {
                json::Value args = json::Value::object();
                args["fx"] = fx.str();
                if (dispatch("fx.restart", std::move(args))) {
                    latency_.erase(fx.str());
                    failed_.erase(fx.str());
                    refresh();
                }
            });
        // Bypass, offline and remove act on every selected effect when the
        // row is one of several selected (gui.md §4.5).
        l1.emplace<ui::Button>(prefix + "bypass." + id, inst->bypass ? "bypassed" : "bypass",
                               [this, fx, bypass = inst->bypass] {
                                   if (setEach(fx, "bypass", !bypass)) refresh();
                               });
        l1.emplace<ui::Button>(prefix + "offline." + id, inst->offline ? "offlined" : "offline",
                               [this, fx, offline = inst->offline] {
                                   if (setEach(fx, "offline", !offline)) refresh();
                               });
        l1.emplace<ui::Button>(prefix + "editor." + id, "UI", [this, fx] {
            if (hooks_.openEditor) {
                if (auto opened = hooks_.openEditor(fx); !opened)
                    setStatus("editor: " + opened.error().message);
            }
        });
        l1.emplace<ui::Button>(prefix + "params." + id, "params", [this, fx] {
            selected_ = fx;
            showParamsTab();
        });
        l1.emplace<ui::Button>(prefix + "remove." + id, "Remove", [this, fx] {
            const std::vector<model::Id> each = targets(fx);
            auto removed = dispatcher_.transact(
                each.size() > 1 ? "Remove effects" : "Remove effect", kUiOrigin,
                [&]() -> Result<json::Value> {
                    for (const model::Id& one : each) {
                        json::Value args = json::Value::object();
                        args["fx"] = one.str();
                        SUPPLE_TRY(dispatcher_.execute({"fx.remove", std::move(args), kUiOrigin}));
                    }
                    return json::Value::object();
                });
            if (!removed) {
                setStatus("fx.remove: " + removed.error().message);
                return;
            }
            for (const model::Id& one : each) {
                picked_.erase(one.str());
                if (selected_ == one) selected_ = {};
            }
            refresh();
        });
    }
    {
        auto& l2 = line(row, prefix + "line2." + id);
        auto& badge =
            l2.emplace<ui::Label>(prefix + "badge." + id, badgeText(*inst, isFailed),
                                  ui::Font::Mono, "caption", isFailed ? "clip" : "ink-faint");
        badge.layout.width = ui::SizeRule::fixed(170);
        auto* wetLabel = &l2.emplace<ui::Label>(prefix + "wetlabel." + id, "", ui::Font::Mono,
                                                "caption", "ink-quiet");
        wetLabel->layout.width = ui::SizeRule::fixed(76);
        auto* wet = &l2.emplace<ui::Slider>(prefix + "wet." + id);
        wet->onChange = [this, fx, wetLabel](double normalized, ui::Gesture g) {
            if (g == ui::Gesture::End) {
                gesture_.clear();
                return;
            }
            if (g == ui::Gesture::Begin) {
                gesture_ = "fx.wet:" + fx.str() + ":" + std::to_string(++gestureSerial_);
                return;
            }
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
        // Pins (ADR-0020): shown where there is a choice — a strip with
        // auxiliary channels, or pins already set.
        const auto owner = input ? std::nullopt : model::trackOfFx(app_.head(), fx);
        const model::Track* tr = owner ? app_.head().tracks.find(*owner) : nullptr;
        if (tr && (tr->channels > 2 || !inst->pins.empty())) {
            auto& pins = l2.emplace<ui::Button>(prefix + "pins." + id, pinsText(inst->pins));
            pins.onClick = [this, fx, at = &pins] { openPinsMenu(fx, at->bounds()); };
        }
    }
}

std::string FxWindow::pinsText(const model::PinMap& pins) {
    auto pair = [](const std::vector<int>& p) {
        if (p.empty()) return std::string("1/2");
        std::string s = std::to_string(p[0]);
        for (std::size_t i = 1; i < p.size(); ++i) s += "/" + std::to_string(p[i]);
        return s;
    };
    // out: the main pair, then the auxiliary outputs' channels (ADR-0021).
    const std::vector<int> mainOut(
        pins.out.begin(),
        pins.out.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(2, pins.out.size())));
    const bool defaultOut = mainOut.empty() || mainOut == std::vector<int>{1, 2};
    std::string text =
        pins.in.empty() && defaultOut ? "pins 1/2" : pair(pins.in) + " \u2192 " + pair(mainOut);
    const auto outs = std::count_if(pins.out.begin() + static_cast<std::ptrdiff_t>(mainOut.size()),
                                    pins.out.end(), [](int c) { return c > 0; });
    if (outs > 0) text += " \u00b7 +" + std::to_string(outs) + " outs";
    if (!pins.key.empty()) text += " \u00b7 key " + pair(pins.key);
    return text;
}

void FxWindow::openPinsMenu(model::Id fx, const ui::Rect& anchor) {
    const model::Session& s = app_.head();
    const model::FxInstance* inst = s.fx.find(fx);
    const auto owner = model::trackOfFx(s, fx);
    if (!inst || !owner) return;
    const int channels = std::max(2, s.tracks.find(*owner)->channels);
    const model::PinMap now = inst->pins;
    auto set = [this, fx](model::PinMap pins) {
        json::Value args = json::Value::object(), p = json::Value::object();
        args["fx"] = fx.str();
        for (auto [list, name] :
             {std::pair{&pins.in, "in"}, {&pins.out, "out"}, {&pins.key, "key"}}) {
            if (list->empty()) continue;
            json::Value a = json::Value::array();
            for (int c : *list) a.push_back(static_cast<std::int64_t>(c));
            p[name] = std::move(a);
        }
        args["pins"] = std::move(p);
        if (dispatch("fx.set", std::move(args))) refresh();
    };
    // A pair as stored: 1/2 is the default (empty).
    auto pairOf = [](int first) {
        return first == 1 ? std::vector<int>{} : std::vector<int>{first, first + 1};
    };
    std::vector<ui::MenuItem> items;
    auto label = [](const char* what, int first) {
        return std::string(what) + " " + std::to_string(first) + "/" + std::to_string(first + 1);
    };
    // Out sets the main pair; the auxiliary outputs' channels after it stay
    // (ADR-0021).
    const std::vector<int> auxOut(now.out.size() > 2 ? now.out.begin() + 2 : now.out.end(),
                                  now.out.end());
    auto mainOf = [](const std::vector<int>& out) {
        return out.size() >= 2 && out[0] == 1 && out[1] == 2
                   ? std::vector<int>{}
                   : std::vector<int>(out.begin(),
                                      out.begin() + static_cast<std::ptrdiff_t>(
                                                        std::min<std::size_t>(2, out.size())));
    };
    for (const char* part : {"In", "Out"}) {
        const bool in = part[0] == 'I';
        for (int first = 1; first + 1 <= channels; first += 2) {
            ui::MenuItem m;
            m.label = label(part, first);
            m.checked = (in ? now.in : mainOf(now.out)) == pairOf(first);
            m.run = [set, now, first, pairOf, in, auxOut] {
                model::PinMap p = now;
                if (in) {
                    p.in = pairOf(first);
                } else if (auxOut.empty()) {
                    p.out = pairOf(first);
                } else {
                    p.out = {first, first + 1};
                    p.out.insert(p.out.end(), auxOut.begin(), auxOut.end());
                }
                set(p);
            };
            items.push_back(std::move(m));
        }
        items.push_back({"", "", {}, true, true});
    }
    // Key pins need a sidechain input (when the engine runs it, it knows).
    const host::Instance* running = app_.instanceOf(fx);
    const bool keyable = !running || running->sidechainChannels() > 0;
    ui::MenuItem routes;
    routes.label = "Key: from key routes only";
    routes.checked = now.key.empty();
    routes.run = [set, now] {
        model::PinMap p = now;
        p.key.clear();
        set(p);
    };
    items.push_back(std::move(routes));
    for (int first = 1; first + 1 <= channels; first += 2) {
        ui::MenuItem m;
        m.label = label("Key", first);
        m.checked = now.key == std::vector<int>{first, first + 1};
        m.enabled = keyable;
        m.run = [set, now, first] {
            model::PinMap p = now;
            p.key = {first, first + 1};
            set(p);
        };
        items.push_back(std::move(m));
    }
    root_.openPopup(ui::makeMenu("fx.pins.menu", std::move(items), [this] { root_.closePopup(); }),
                    anchor, ui::Placement::Below);
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

// A chain row dragged among the others: the insertion bar follows the pointer
// (in front of the first row whose middle is below it); the drop is one fx.move.
void FxWindow::dragRow(model::Id fx, int phase, ui::Point p) {
    const std::vector<model::Id> c = chain();
    std::vector<Row*> rows;
    for (model::Id id : c)
        if (auto* row = dynamic_cast<Row*>(root_.find("fx.row." + id.str()))) rows.push_back(row);
    if (rows.size() != c.size()) return;
    const auto from = static_cast<std::size_t>(std::find(c.begin(), c.end(), fx) - c.begin());
    std::size_t gap = 0;
    for (Row* row : rows)
        if (row->bounds().y + row->bounds().h / 2 < p.y) ++gap;
    const std::size_t index = gap > from ? gap - 1 : gap;
    const auto shown = static_cast<Row::Phase>(phase);
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const bool moving = shown == Row::Phase::Move && index != from;
        rows[i]->dragged = shown == Row::Phase::Move && i == from;
        rows[i]->markerTop = moving && i == gap;
        rows[i]->markerBottom = moving && gap == rows.size() && i + 1 == rows.size();
    }
    root_.damageAll();
    if (shown != Row::Phase::Drop || index == from || from >= c.size()) return;
    json::Value args = json::Value::object();
    args["fx"] = fx.str();
    args["index"] = static_cast<std::int64_t>(index);
    if (dispatch("fx.move", std::move(args))) refresh();
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
    if (take_)
        args["take"] = take_->str();
    else
        args["track"] = track_.str();
    args["plugin"] = pluginId;
    if (pendingChain_ == "input" && !take_) args["chain"] = "input";
    auto result = dispatcher_.execute({"fx.add", std::move(args), kUiOrigin});
    if (!result) {
        setStatus("fx.add: " + result.error().message);
        return;
    }
    if (const json::Value* made = result->find("fx"); made && made->isString())
        if (auto id = model::Id::parse(made->asString())) {
            selected_ = *id;
            picked_.clear();
        }
    setStatus("added " + pluginId);
    refresh();
}

void FxWindow::choosePresetChain(bool input) {
    presetChain_ = input ? "input" : "main";
    if (presetChainButton_) presetChainButton_->setText(input ? "Input chain" : "Main chain");
}

void FxWindow::promptSaveChain() {
    if (!savePrompt_ || !saveName_) return;
    savePrompt_->visible = true;
    saveName_->setText("");
    root_.relayout();
    root_.focus(saveName_);
    setStatus("Save the " + presetChain_ + " chain");
}

void FxWindow::saveChain() {
    if (!saveName_) return;
    auto args = json::Value::object();
    args["track"] = track_.str();
    args["name"] = saveName_->text();
    args["chain"] = presetChain_;
    auto result = dispatcher_.execute({"fx.chain.save", std::move(args), kUiOrigin});
    if (!result) {
        setStatus(result.error().message);
        return;
    }
    setStatus("Saved " + saveName_->text());
    hideSavePrompt();
}

void FxWindow::hideSavePrompt() {
    if (!savePrompt_) return;
    savePrompt_->visible = false;
    root_.releaseWithin(savePrompt_);
    root_.focus(root_.find("fx.chain.save"));
    root_.relayout();
    root_.damageAll();
}

void FxWindow::loadChainMenu(bool replace) {
    if (chainMenuOpen_) {  // the button toggles its menu (and closes a menu above it)
        while (chainMenuOpen_ && root_.popupOpen()) root_.closePopup();
        return;
    }
    auto listed = dispatcher_.execute({"fx.chain.list", json::Value::object(), kUiOrigin});
    if (!listed) {
        setStatus(listed.error().message);
        return;
    }
    if ((*listed)["chains"].asArray().empty()) {
        setStatus("No saved chains yet");
        return;
    }
    setStatus("Load into " + presetChain_ + " (Shift = replace)");
    const auto track = track_;
    const auto chain = presetChain_;
    auto menu = std::make_unique<ChainMenu>(
        root_, (*listed)["chains"], replace,
        [this, track, chain](const std::string& path, bool replacing) {
            auto args = json::Value::object();
            args["track"] = track.str();
            args["path"] = path;
            args["chain"] = chain;
            args["mode"] = replacing ? "replace" : "append";
            if (dispatch("fx.chain.load", std::move(args))) {
                setStatus("Loaded chain");
                refresh();
            }
        },
        [this](const std::string& name) {
            auto args = json::Value::object();
            args["name"] = name;
            if (!dispatch("fx.chain.delete", std::move(args))) return false;
            setStatus("Deleted " + name);
            return true;
        });
    const auto* button = root_.find("fx.chain.load");
    chainMenuOpen_ = true;
    root_.openPopup(std::move(menu), button ? button->bounds() : ui::Rect{}, ui::Placement::Below,
                    [this] { chainMenuOpen_ = false; });
}

}  // namespace supple::gui
