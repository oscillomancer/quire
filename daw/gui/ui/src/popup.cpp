#include "supple/ui/popup.h"

#include <algorithm>
#include <memory>

#include "supple/ui/list.h"
#include "supple/ui/root.h"

namespace supple::ui {
namespace {

class MenuList : public List {
  public:
    MenuList(std::string id, std::vector<MenuItem> items, std::function<void()> close)
        : List(std::move(id)), items_(std::move(items)), close_(std::move(close)) {
        rowHeight = 26;
        setCount(items_.size());
        paintRow = [this](Canvas& c, std::size_t i, const Rect& r, bool) {
            const Theme& t = c.theme();
            const MenuItem& item = items_[i];
            if (item.separator) {
                c.fillRect({r.x + 8, r.y + r.h / 2, r.w - 16, 1}, t.color("rule"));
                return;
            }
            const Color ink = item.enabled ? t.color("ink") : t.color("ink-faint");
            if (item.checked)
                c.text({r.x + 8, r.y, 16, r.h}, "\xE2\x9C\x93", Font::Ui, t.type("small"), ink);
            c.text({r.x + 26, r.y, r.w - 34, r.h}, item.label, Font::Ui, t.type("small"), ink);
            if (!item.shortcut.empty())
                c.text({r.x, r.y, r.w - 10, r.h}, item.shortcut, Font::Mono, t.type("caption"),
                       t.color("ink-quiet"), kRight | kMiddle);
        };
        onActivate = [this](std::size_t i) {
            const MenuItem& item = items_[i];
            if (item.separator || !item.enabled) return;
            auto run = item.run;  // the menu is destroyed by close
            if (close_) close_();
            if (run) run();
        };
    }
    Size measure(const Canvas& c, Size) override {
        float w = 120;
        for (const MenuItem& item : items_)
            w = std::max(w, c.textWidth(item.label, Font::Ui, c.theme().type("small")) +
                                c.textWidth(item.shortcut, Font::Mono, c.theme().type("caption")) +
                                60);
        return {w, rowHeight * static_cast<float>(items_.size())};
    }
    bool onPointer(const PointerEvent& e) override {
        // A single click activates a menu row (lists need a double click).
        if (e.type == PointerEvent::Type::Up && e.button == 1 && selected()) {
            if (bounds().contains(e.pos)) onActivate(*selected());
            return true;
        }
        if (e.type == PointerEvent::Type::Move && bounds().contains(e.pos)) {
            const auto i = static_cast<std::size_t>((e.pos.y - bounds().y + scroll()) / rowHeight);
            if (i < items_.size() && !items_[i].separator) select(i);
            return true;
        }
        return List::onPointer(e);
    }

  private:
    std::vector<MenuItem> items_;
    std::function<void()> close_;
};

}  // namespace

std::unique_ptr<Node> makeMenu(std::string id, std::vector<MenuItem> items,
                               std::function<void()> close) {
    return std::make_unique<MenuList>(std::move(id), std::move(items), std::move(close));
}

// --- Root's popup stack

class PopupFrame : public Node {
  public:
    PopupFrame(std::unique_ptr<Node> content, Rect anchor, Placement placement,
               std::function<void()> closed, Node* focusAfter)
        : Node("popup", Role::Menu),
          anchor_(anchor),
          placement_(placement),
          onClose(std::move(closed)),
          restoreFocus(focusAfter) {
        interactive = true;  // swallow clicks on the frame's padding
        content_ = &add(std::move(content));
    }
    void arrange(const Canvas& c, const Rect& window) override {
        Size want = content_->measure(c, {window.w, window.h});
        if (content_->layout.width.kind == SizeRule::Kind::Fixed)
            want.w = content_->layout.width.value;
        if (content_->layout.height.kind == SizeRule::Kind::Fixed)
            want.h = content_->layout.height.value;
        want.w = std::min(want.w, window.w - 16);
        want.h = std::min(want.h, window.h - 16);
        Rect r{anchor_.x, anchor_.bottom() + 2, want.w + 2, want.h + 2};
        if (placement_ == Placement::Above ||
            (placement_ == Placement::Below && r.bottom() > window.bottom() - 8 &&
             anchor_.y - want.h - 4 > window.y))
            r.y = anchor_.y - r.h - 2;
        if (placement_ == Placement::Right) r = {anchor_.right() + 2, anchor_.y, r.w, r.h};
        if (placement_ == Placement::Center)
            r = {window.x + (window.w - r.w) / 2, window.y + window.h * 0.18f, r.w, r.h};
        r.x = std::clamp(r.x, window.x + 8, std::max(window.x + 8, window.right() - r.w - 8));
        r.y = std::clamp(r.y, window.y + 8, std::max(window.y + 8, window.bottom() - r.h - 8));
        setBounds(r);
        content_->arrange(c, r.inset(1));
    }
    void paint(Canvas& c) override {
        const Theme& t = c.theme();
        const Rect b = bounds();
        c.fillRound({b.x + 2, b.y + 4, b.w, b.h}, t.radius("m"), Color{0, 0, 0, 0.35f});  // shadow
        c.fillRound(b, t.radius("m"), t.color("raised"));
        c.strokeRound(b, t.radius("m"), t.color("rule"));
    }
    Node* content_ = nullptr;
    Rect anchor_;
    Placement placement_;
    std::function<void()> onClose;
    Node* restoreFocus;
};

Node& Root::openPopup(std::unique_ptr<Node> content, const Rect& anchor, Placement placement,
                      std::function<void()> onClose) {
    auto frame = std::make_unique<PopupFrame>(std::move(content), anchor, placement,
                                              std::move(onClose), focused_);
    PopupFrame& f = static_cast<PopupFrame&>(overlay_->add(std::move(frame)));
    popups_.push_back(&f);
    relayout();
    requestAnimationFrame();
    // Focus the first focusable node inside.
    std::function<Node*(Node&)> first = [&](Node& n) -> Node* {
        if (n.focusable && n.enabled && n.visible) return &n;
        for (const auto& c : n.children())
            if (Node* hit = first(*c)) return hit;
        return nullptr;
    };
    focus(first(*f.content_));
    return *f.content_;
}

void Root::closePopup() {
    if (popups_.empty()) return;
    auto* f = static_cast<PopupFrame*>(popups_.back());
    popups_.pop_back();
    auto onClose = std::move(f->onClose);
    Node* restore = f->restoreFocus;
    focus(nullptr);
    overlay_->remove(f);  // destroys it
    requestAnimationFrame();
    // Restore focus only if that node still exists in the tree.
    std::function<bool(Node&)> alive = [&](Node& n) {
        if (&n == restore) return true;
        for (const auto& c : n.children())
            if (alive(*c)) return true;
        return false;
    };
    if (restore && ((content_ && alive(*content_)) || alive(*overlay_))) focus(restore);
    if (onClose) onClose();
}

void Root::closePopups() {
    while (!popups_.empty()) closePopup();
}

}  // namespace supple::ui
