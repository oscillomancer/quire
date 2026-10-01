#include "supple/ui/node.h"

#include <algorithm>

#include "supple/ui/root.h"

namespace supple::ui {
namespace {
void forgetSubtree(Root* root, Node* node) {
    if (!root) return;
    root->forget(node);
    for (const auto& c : node->children()) forgetSubtree(root, c.get());
}

float clampMain(const Node& c, bool row, float v) {
    return row ? std::clamp(v, c.layout.minWidth, c.layout.maxWidth)
               : std::clamp(v, c.layout.minHeight, c.layout.maxHeight);
}
}  // namespace

Node::~Node() = default;

Node& Node::add(std::unique_ptr<Node> child) {
    child->parent_ = this;
    children_.push_back(std::move(child));
    relayout();
    return *children_.back();
}

std::unique_ptr<Node> Node::remove(Node* child) {
    auto it = std::find_if(children_.begin(), children_.end(),
                           [&](const auto& c) { return c.get() == child; });
    if (it == children_.end()) return nullptr;
    forgetSubtree(root(), child);
    invalidate(child->bounds());
    std::unique_ptr<Node> out = std::move(*it);
    children_.erase(it);
    out->parent_ = nullptr;
    relayout();
    return out;
}

void Node::clear() {
    for (const auto& c : children_) forgetSubtree(root(), c.get());
    children_.clear();
    invalidate();
    relayout();
}

Node* Node::find(std::string_view id) {
    if (id_ == id) return this;
    for (const auto& c : children_)
        if (Node* hit = c->find(id)) return hit;
    return nullptr;
}

Root* Node::root() const {
    const Node* n = this;
    while (n->parent_) n = n->parent_;
    return n->root_;
}

void Node::attach(Root* r) { root_ = r; }

void Node::relayout() {
    if (Root* r = root()) r->relayout();
}

void Node::invalidate() { invalidate(bounds_); }

void Node::invalidate(const Rect& area) {
    if (Root* r = root()) r->damage(area);
}

bool Node::hovered() const {
    const Root* r = root();
    return r && r->hovered() == this;
}

bool Node::focused() const {
    const Root* r = root();
    return r && r->focused() == this;
}

Size Node::measure(const Canvas& canvas, Size available) {
    if (layout.dir == Layout::Dir::None) return {0, 0};
    const bool row = layout.dir == Layout::Dir::Row;
    float main = 0, cross = 0;
    int shown = 0;
    for (const auto& c : children_) {
        if (!c->visible) continue;
        const SizeRule& mr = row ? c->layout.width : c->layout.height;
        const SizeRule& cr = row ? c->layout.height : c->layout.width;
        Size content{};
        if (mr.kind == SizeRule::Kind::Content || cr.kind == SizeRule::Kind::Content)
            content = c->measure(canvas, available);
        const float m = mr.kind == SizeRule::Kind::Fixed     ? mr.value
                        : mr.kind == SizeRule::Kind::Content ? (row ? content.w : content.h)
                                                             : 0.0f;
        const float x = cr.kind == SizeRule::Kind::Fixed     ? cr.value
                        : cr.kind == SizeRule::Kind::Content ? (row ? content.h : content.w)
                                                             : 0.0f;
        main += clampMain(*c, row, m);
        cross = std::max(cross, x);
        ++shown;
    }
    main += layout.gap * static_cast<float>(std::max(0, shown - 1));
    const float padMain = row ? layout.padding.left + layout.padding.right
                              : layout.padding.top + layout.padding.bottom;
    const float padCross = row ? layout.padding.top + layout.padding.bottom
                               : layout.padding.left + layout.padding.right;
    return row ? Size{main + padMain, cross + padCross} : Size{cross + padCross, main + padMain};
}

void Node::arrange(const Canvas& canvas, const Rect& r) {
    bounds_ = r;
    if (layout.dir == Layout::Dir::None) {
        for (const auto& c : children_) c->arrange(canvas, c->bounds());
        return;
    }
    const bool row = layout.dir == Layout::Dir::Row;
    const Rect inner{r.x + layout.padding.left, r.y + layout.padding.top,
                     std::max(0.0f, r.w - layout.padding.left - layout.padding.right),
                     std::max(0.0f, r.h - layout.padding.top - layout.padding.bottom)};
    const float mainSpace = row ? inner.w : inner.h;
    const float crossSpace = row ? inner.h : inner.w;

    struct Slot {
        Node* node;
        float main = 0, cross = 0, weight = 0;
    };
    std::vector<Slot> slots;
    float used = 0, weights = 0;
    for (const auto& c : children_) {
        if (!c->visible) continue;
        Slot s{c.get()};
        const SizeRule& mr = row ? c->layout.width : c->layout.height;
        const SizeRule& cr = row ? c->layout.height : c->layout.width;
        Size content{};
        if (mr.kind == SizeRule::Kind::Content || cr.kind == SizeRule::Kind::Content)
            content =
                c->measure(canvas, row ? Size{mainSpace, crossSpace} : Size{crossSpace, mainSpace});
        if (mr.kind == SizeRule::Kind::Fraction) {
            s.weight = std::max(0.0f, mr.value);
            weights += s.weight;
        } else {
            s.main = clampMain(
                *c, row,
                mr.kind == SizeRule::Kind::Fixed ? mr.value : (row ? content.w : content.h));
            used += s.main;
        }
        const float crossMax = row ? c->layout.maxHeight : c->layout.maxWidth;
        const float crossMin = row ? c->layout.minHeight : c->layout.minWidth;
        // A fraction on the cross axis fills it; alignment places the rest.
        float cross = crossSpace;
        if (cr.kind == SizeRule::Kind::Fixed)
            cross = cr.value;
        else if (cr.kind == SizeRule::Kind::Content)
            cross =
                layout.align == Layout::Align::Stretch ? crossSpace : (row ? content.h : content.w);
        s.cross = std::clamp(std::min(cross, crossSpace), crossMin, std::max(crossMin, crossMax));
        slots.push_back(s);
    }
    const float gaps = layout.gap * static_cast<float>(std::max<std::size_t>(1, slots.size()) - 1);
    const float leftover = std::max(0.0f, mainSpace - used - gaps);
    for (Slot& s : slots)
        if (s.weight > 0) s.main = clampMain(*s.node, row, leftover * s.weight / weights);

    float at = row ? inner.x : inner.y;
    for (const Slot& s : slots) {
        float crossAt = row ? inner.y : inner.x;
        if (layout.align == Layout::Align::Center)
            crossAt += (crossSpace - s.cross) / 2;
        else if (layout.align == Layout::Align::End)
            crossAt += crossSpace - s.cross;
        const Rect b =
            row ? Rect{at, crossAt, s.main, s.cross} : Rect{crossAt, at, s.cross, s.main};
        s.node->arrange(canvas, b);
        at += s.main + layout.gap;
    }
}

Node* Node::hitTest(Point p) {
    if (!visible || !bounds_.contains(p)) return nullptr;
    for (auto it = children_.rbegin(); it != children_.rend(); ++it)
        if (Node* hit = (*it)->hitTest(p)) return hit;
    return interactive && enabled ? this : nullptr;
}

}  // namespace supple::ui
