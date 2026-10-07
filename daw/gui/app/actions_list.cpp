#include "actions_list.h"

#include <SDL3/SDL_keycode.h>

#include <memory>

#include "chrome.h"
#include "supple/json/parse.h"
#include "supple/ui/list.h"
#include "supple/ui/popup.h"
#include "supple/ui/text_field.h"
#include "supple/ui/widgets.h"

namespace supple::gui {
namespace {

// Captures the next chord (any key, printable or not) for a binding.
class KeyCapture : public ui::Node {
  public:
    explicit KeyCapture(std::function<void(std::optional<actions::Chord>)> done)
        : Node("actions.capture", ui::Role::Custom), done_(std::move(done)) {
        focusable = true;
        layout.height = ui::SizeRule::fixed(0);
    }
    bool onKey(const ui::KeyEvent& e) override {
        switch (e.key) {
            case SDLK_LCTRL:
            case SDLK_RCTRL:
            case SDLK_LSHIFT:
            case SDLK_RSHIFT:
            case SDLK_LALT:
            case SDLK_RALT:
            case SDLK_LGUI:
            case SDLK_RGUI:
                return true;  // wait for the key itself
            case SDLK_ESCAPE:
                done_(std::nullopt);
                return true;
            default:
                done_(actions::Chord{e.key, e.mods});
                return true;
        }
    }
    bool onText(std::string_view) override { return true; }  // swallow the typed character

  private:
    std::function<void(std::optional<actions::Chord>)> done_;
};

class ActionsPanel : public ui::Node {
  public:
    ActionsPanel(ui::Root& root, AppActions& actions, Chrome* chrome)
        : Node("actions"), root_(root), actions_(actions), chrome_(chrome) {
        layout.dir = ui::Layout::Dir::Column;
        layout.width = ui::SizeRule::fixed(600);
        layout.height = ui::SizeRule::fixed(440);
        layout.padding = ui::Insets::all(10);
        layout.gap = 8;
        search_ = &emplace<ui::TextField>("actions.search", "", "Search actions…");
        list_ = &emplace<ui::List>("actions.results");
        capture_ = &emplace<KeyCapture>([this](std::optional<actions::Chord> c) { captured(c); });
        hint_ = &emplace<ui::Label>("actions.hint", chrome_ ? kChromeHint : kHint, ui::Font::Ui,
                                    "caption", "ink-quiet");
        hint_->layout.width = ui::SizeRule::fraction();
        search_->onChange = [this](const std::string& q) { filter(q); };
        search_->onSubmit = [this](const std::string&) { activate(); };
        search_->onOtherKey = [this](const ui::KeyEvent& e) {
            if (e.key == SDLK_B && (e.mods & ui::kCtrl)) {
                startCapture();
                return true;
            }
            if (e.key == SDLK_T && (e.mods & ui::kCtrl) && chrome_) {
                toggleToolbar();
                return true;
            }
            if (e.key == SDLK_M && (e.mods & ui::kCtrl) && chrome_) {
                openMenuChoice();
                return true;
            }
            return list_->onKey(e);  // Up/Down/PgUp/PgDn/Home/End move the selection
        };
        list_->rowHeight = 30;
        list_->onActivate = [this](std::size_t) { activate(); };
        list_->paintRow = [this](ui::Canvas& c, std::size_t i, const ui::Rect& r, bool) {
            const ui::Theme& t = c.theme();
            const actions::Action& a = *matches_[i];
            const std::string keys = actions_.keysFor(a.id);
            const float keysW =
                keys.empty() ? 0 : c.textWidth(keys, ui::Font::Mono, t.type("caption")) + 16;
            const float nameW = c.textWidth(a.name, ui::Font::Ui, t.type("small"));
            c.text({r.x + 10, r.y, r.w - 20 - keysW, r.h}, a.name, ui::Font::Ui, t.type("small"),
                   t.color("ink"));
            if (!a.command.empty() && a.command != a.name)
                c.text({r.x + 22 + nameW, r.y, std::max(0.0f, r.w - 34 - nameW - keysW), r.h}, a.id,
                       ui::Font::Mono, t.type("caption"), t.color("ink-faint"));
            if (!keys.empty())
                c.text({r.x, r.y, r.w - 10, r.h}, keys, ui::Font::Mono, t.type("caption"),
                       t.color("ink-quiet"), ui::kRight | ui::kMiddle);
        };
        filter("");
    }

    void paint(ui::Canvas&) override {}

  private:
    static constexpr const char* kHint = "Enter run · Ctrl+B bind a key · Esc close";
    static constexpr const char* kChromeHint =
        "Enter run · Ctrl+B bind a key · Ctrl+T toolbar · Ctrl+M menu · Esc close";

    void toggleToolbar() {
        const actions::Action* a = current();
        if (!a) return;
        const std::string id = a->id;
        chrome_->toggleToolbar(id);
        hint_->setText((chrome_->inToolbar(id) ? "On the toolbar: " : "Off the toolbar: ") + id);
    }

    // Which menu holds the action: a checked entry per menu; choosing one
    // adds the action there or takes it out.
    void openMenuChoice() {
        const actions::Action* a = current();
        if (!a) return;
        const std::string id = a->id;
        std::vector<ui::MenuItem> items;
        for (const ChromeLayout::Menu& m : chrome_->layout().menus) {
            ui::MenuItem item;
            item.label = m.title;
            item.checked = chrome_->inMenu(m.title, id);
            item.run = [this, title = m.title, id] {
                chrome_->toggleMenu(title, id);
                hint_->setText((chrome_->inMenu(title, id) ? "In " : "Out of ") + title + ": " +
                               id);
            };
            items.push_back(std::move(item));
        }
        root_.openPopup(ui::makeMenu("actions.menus", std::move(items),
                                     [this] {
                                         root_.closePopup();
                                         root_.focus(search_);
                                     }),
                        {}, ui::Placement::Center);
    }

    void filter(const std::string& q) {
        matches_.clear();
        if (q.empty()) {
            for (const actions::Action* a : actions_.registry().all()) matches_.push_back(a);
            std::sort(matches_.begin(), matches_.end(),
                      [](const actions::Action* x, const actions::Action* y) {
                          return x->name < y->name;
                      });
        } else {
            for (const auto& m : actions_.registry().search(q, 200)) matches_.push_back(m.action);
        }
        list_->setCount(matches_.size());
        list_->select(matches_.empty() ? std::nullopt : std::optional<std::size_t>(0));
    }

    const actions::Action* current() const {
        const auto i = list_->selected();
        return i && *i < matches_.size() ? matches_[*i] : nullptr;
    }

    void activate() {
        const actions::Action* a = current();
        if (!a) return;
        ui::Root& root = root_;
        AppActions& actions = actions_;
        const actions::Action action = *a;  // the panel is destroyed by closePopup
        root.closePopup();
        if (action.prompts.empty()) {
            (void)actions.run(action);
            return;
        }
        openActionPrompt(root, actions, action);
    }

    void startCapture() {
        if (!current()) return;
        capturing_ = current()->id;
        hint_->setText("Press the keys for \"" + current()->name + "\" (Esc cancels)");
        root_.focus(capture_);
    }

    void captured(std::optional<actions::Chord> chord) {
        root_.focus(search_);
        if (!chord) {
            hint_->setText(chrome_ ? kChromeHint : kHint);
            return;
        }
        actions::Binding b{{*chord}, "global", capturing_};
        actions_.keymap().bind(b);
        std::string note = actions::describe(*chord) + " → " + capturing_;
        if (auto saved = actions_.keymap().save(actions_.keymapFile()); !saved)
            note += " (not saved: " + saved.error().message + ")";
        for (const auto& [x, y] : actions_.keymap().conflicts())
            if (x == b || y == b) note += " — conflicts with " + (x == b ? y.action : x.action);
        hint_->setText(note);
        list_->invalidate();
    }

    ui::Root& root_;
    AppActions& actions_;
    Chrome* chrome_ = nullptr;
    ui::TextField* search_ = nullptr;
    ui::List* list_ = nullptr;
    ui::Node* capture_ = nullptr;
    ui::Label* hint_ = nullptr;
    std::vector<const actions::Action*> matches_;
    std::string capturing_;
};

}  // namespace

void openActionPrompt(ui::Root& root, AppActions& actions, const actions::Action& action) {
    auto form = std::make_unique<ui::Node>("actions.prompt");
    form->layout.dir = ui::Layout::Dir::Column;
    form->layout.width = ui::SizeRule::fixed(420);
    form->layout.height =
        ui::SizeRule::fixed(64.0f + 56.0f * static_cast<float>(action.prompts.size()));
    form->layout.padding = ui::Insets::all(12);
    form->layout.gap = 6;
    form->emplace<ui::Label>("actions.prompt.title", action.name, ui::Font::UiBold, "body");
    auto fields = std::make_shared<std::vector<ui::TextField*>>();
    for (const std::string& p : action.prompts) {
        form->emplace<ui::Label>("actions.prompt.label." + p, p, ui::Font::Mono, "caption",
                                 "ink-quiet");
        fields->push_back(&form->emplace<ui::TextField>("actions.prompt." + p));
    }
    auto submit = [rootp = &root, actionsp = &actions, action, fields](const std::string&) {
        json::Value values = json::Value::object();
        for (std::size_t i = 0; i < fields->size(); ++i) {
            const std::string& text = (*fields)[i]->text();
            auto parsed = json::parse(text);  // numbers, booleans, arrays; else the text itself
            values[action.prompts[i]] = parsed ? *parsed : json::Value(text);
        }
        // closePopup destroys this lambda (it lives in the form): copy first.
        ui::Root* owner = rootp;
        AppActions* runner = actionsp;
        const actions::Action act = action;
        owner->closePopup();
        (void)runner->run(act, values);
    };
    for (ui::TextField* f : *fields) f->onSubmit = submit;
    root.openPopup(std::move(form), {}, ui::Placement::Center);
}

void openActionsList(ui::Root& root, AppActions& actions, Chrome* chrome) {
    if (root.popupOpen()) root.closePopups();
    root.openPopup(std::make_unique<ActionsPanel>(root, actions, chrome), {},
                   ui::Placement::Center);
}

}  // namespace supple::gui
