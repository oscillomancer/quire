#include "fx_window.h"

#include <SDL3/SDL_keycode.h>

#include <algorithm>
#include <cstdio>
#include <map>

#include "app_actions.h"
#include "supple/ui/controls.h"
#include "supple/ui/list.h"
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

class ChainMenu : public ui::List {
  public:
    ChainMenu(ui::Root& root, json::Value chains, bool replace,
              std::function<void(const std::string&, bool)> load)
        : List("fx.chain.menu"),
          root_(root),
          chains_(std::move(chains)),
          replace_(replace),
          load_(std::move(load)) {
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
    ui::Root& root_;
    json::Value chains_;
    bool replace_, shift_ = false;
    std::function<void(const std::string&, bool)> load_;
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
    presetChainButton_ = &header.emplace<ui::Button>(
        "fx.chain.target", "Main chain", [this] { choosePresetChain(presetChain_ != "input"); });
    header.emplace<ui::Button>("fx.chain.save", "Save chain…", [this] {
        if (take_) return setStatus("Chain presets are for track chains");
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
    saveName_->onCancel = [this] {
        savePrompt_->visible = false;
        root_.focus(root_.find("fx.chain.save"));
        root_.relayout();
        root_.damageAll();
    };
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
    take_.reset();
    selected_ = {};
    tab_ = Tab::Chain;
    presetChain_ = "main";
    if (presetChainButton_) presetChainButton_->setText("Main chain");
    if (savePrompt_) savePrompt_->visible = false;
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

Status FxWindow::openToAdd(const UiState& ui, const json::Value& prompt) {
    if (auto opened = openFor(ui, prompt); !opened) return opened;
    browser_->open([this](const std::string& pluginId) { addFx(pluginId); });
    return {};
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
    if (page_ && app_.head().version != shownVersion_) refresh();
}

void FxWindow::select(model::Id fx) {
    selected_ = fx;
    const auto input = inputChain();
    choosePresetChain(std::find(input.begin(), input.end(), fx) != input.end());
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
    const std::string trackName = track->name.empty() ? track_.str() : track->name;
    if (take_ && !app_.head().takes.find(*take_)) take_.reset();  // the take is gone
    if (title_)
        title_->setText(take_ ? "Take FX: " + trackName + " / " + take_->str()
                              : "FX: " + trackName);
    body_->clear();
    if (hasSelected() && !app_.head().fx.find(selected_)) selected_ = {};
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
        prefix + "row." + id, [this, fx] { select(fx); },
        input ? std::function<void(int)>{}
              : std::function<void(int)>{[this, fx](int delta) { moveFx(fx, delta); }});
    row.selected = selected_ == fx;
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
                    failed_.erase(fx.str());
                    refresh();
                }
            });
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
        if (auto id = model::Id::parse(made->asString())) selected_ = *id;
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
    savePrompt_->visible = false;
    root_.focus(root_.find("fx.chain.save"));
    root_.relayout();
    root_.damageAll();
}

void FxWindow::loadChainMenu(bool replace) {
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
    auto menu =
        std::make_unique<ChainMenu>(root_, (*listed)["chains"], replace,
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
                                    });
    const auto* button = root_.find("fx.chain.load");
    root_.openPopup(std::move(menu), button ? button->bounds() : ui::Rect{}, ui::Placement::Below);
}

}  // namespace supple::gui
