#include "supple/ui/root.h"

#include "supple/ui/gl.h"

#include <SDL3/SDL.h>
#define GL_GLEXT_PROTOTYPES
#include <GL/gl.h>
#include <GL/glext.h>
#include <nanovg.h>
#define NANOVG_GL3 1
#include <nanovg_gl.h>
extern "C" {  // compiled as C in nanovg_gl3.c; the header has no guards of its own
#include <nanovg_gl_utils.h>
}

#include <algorithm>
#include <cmath>

namespace supple::ui {

struct Root::Cache {
    NVGLUframebuffer* fb = nullptr;
    int width = 0, height = 0;  // pixels
};

namespace {
unsigned modsFrom(SDL_Keymod m) {
    unsigned out = 0;
    if (m & SDL_KMOD_SHIFT) out |= kShift;
    if (m & SDL_KMOD_CTRL) out |= kCtrl;
    if (m & SDL_KMOD_ALT) out |= kAlt;
    if (m & SDL_KMOD_GUI) out |= kSuper;
    return out;
}

int buttonFrom(Uint8 b) {
    return b == SDL_BUTTON_LEFT ? 1 : b == SDL_BUTTON_MIDDLE ? 2 : b == SDL_BUTTON_RIGHT ? 3 : 0;
}

// Pointer events bubble from the target to its ancestors until handled.
bool deliver(Node* target, const PointerEvent& e) {
    for (Node* n = target; n; n = n->parent())
        if (n->enabled && n->onPointer(e)) return true;
    return false;
}

void collectFocusable(Node& n, std::vector<Node*>& out) {
    if (!n.visible) return;
    if (n.focusable && n.enabled) out.push_back(&n);
    for (const auto& c : n.children()) collectFocusable(*c, out);
}
}  // namespace

Root::Root(gui::Window& window, const gui::Fonts& fonts, Theme theme)
    : window_(window),
      fonts_(fonts),
      theme_(std::move(theme)),
      overlay_(std::make_unique<Node>("overlay")) {
    overlay_->attach(this);
}

Root::~Root() {
    if (cache_ && cache_->fb) nvgluDeleteFramebuffer(cache_->fb);
}

void Root::setContent(std::unique_ptr<Node> content) {
    focused_ = hovered_ = captured_ = nullptr;
    content_ = std::move(content);
    if (content_) content_->attach(this);
    relayout();
    damageAll();
}

Node* Root::find(std::string_view id) const {
    if (Node* n = overlay_->find(id)) return n;
    return content_ ? content_->find(id) : nullptr;
}

void Root::setTheme(Theme theme) {
    theme_ = std::move(theme);
    relayout();
    damageAll();
}

bool Root::wantsFrame() const { return !damage_.empty() || layoutPending_ || animate_; }

void Root::damage(const Rect& area) {
    const Rect r = area.intersect({0, 0, window_.width(), window_.height()});
    if (r.empty()) return;
    // Merge overlapping regions; past a handful, one bounding region is cheaper.
    for (Rect& d : damage_)
        if (!d.intersect(r).empty()) {
            d = d.unite(r);
            return;
        }
    damage_.push_back(r);
    if (damage_.size() > 8) {
        Rect all;
        for (const Rect& d : damage_) all = all.unite(d);
        damage_ = {all};
    }
}

void Root::damageAll() {
    damage_.clear();
    damage_.push_back({0, 0, window_.width(), window_.height()});
}

void Root::layoutNow(const Canvas& canvas) {
    const Rect full{0, 0, window_.width(), window_.height()};
    if (content_) content_->arrange(canvas, full);
    overlay_->setBounds(full);
    for (const auto& c : overlay_->children()) {
        const bool popup = std::find(popups_.begin(), popups_.end(), c.get()) != popups_.end();
        c->arrange(canvas, popup ? full : c->bounds());  // popups place themselves in the window
    }
    laidOut_ = {full.w, full.h};
    layoutPending_ = false;
    damageAll();  // positions may have moved anywhere
}

void Root::ensureCache() {
    int pw = 0, ph = 0;
    SDL_GetWindowSizeInPixels(window_.sdl(), &pw, &ph);
    if (cache_ && cache_->width == pw && cache_->height == ph) return;
    if (!cache_) cache_ = std::make_unique<Cache>();
    if (cache_->fb) nvgluDeleteFramebuffer(cache_->fb);
    cache_->fb = nvgluCreateFramebuffer(window_.vg(), pw, ph, 0);
    cache_->width = pw;
    cache_->height = ph;
    damageAll();
}

void Root::paintTree(Node& node, Canvas& canvas, const Rect& clip) {
    if (!node.visible) return;
    if (&node != overlay_.get() && node.bounds().intersect(clip).empty()) return;
    node.paint(canvas);
    if (node.drawsGl) paintGl(node, clip);
    const Rect inner = node.clipsChildren ? clip.intersect(node.bounds()) : clip;
    if (inner.empty()) return;
    if (node.clipsChildren) nvgScissor(window_.vg(), inner.x, inner.y, inner.w, inner.h);
    for (const auto& c : node.children()) paintTree(*c, canvas, inner);
    if (node.clipsChildren) nvgScissor(window_.vg(), clip.x, clip.y, clip.w, clip.h);
}

// NanoVG batches until nvgEndFrame, so a GL layer flushes what is pending,
// draws under a GL scissor, and resumes NanoVG with the same clip.
void Root::paintGl(Node& node, const Rect& clip) {
    NVGcontext* vg = window_.vg();
    nvgEndFrame(vg);
    const float w = window_.width(), h = window_.height();
    const Rect r = clip.intersect(node.bounds()).snapOut(ratio_);
    if (!r.empty()) {
        glEnable(GL_SCISSOR_TEST);
        glScissor(static_cast<GLint>(std::lround(r.x * ratio_)),
                  static_cast<GLint>(std::lround((h - r.bottom()) * ratio_)),
                  static_cast<GLsizei>(std::lround(r.w * ratio_)),
                  static_cast<GLsizei>(std::lround(r.h * ratio_)));
        node.paintGl(GlFrame{w, h, ratio_, r});
        glDisable(GL_SCISSOR_TEST);
    }
    nvgBeginFrame(vg, w, h, ratio_);
    nvgScissor(vg, clip.x, clip.y, clip.w, clip.h);
}

void Root::frame(bool present) {
    Canvas canvas(window_.vg(), fonts_, theme_);
    NVGcontext* vg = window_.vg();
    window_.makeCurrent();
    const float w = window_.width(), h = window_.height(), ratio = window_.pixelRatio();
    ratio_ = ratio;
    if (laidOut_.w != w || laidOut_.h != h) layoutPending_ = true;
    if (layoutPending_) layoutNow(canvas);
    ensureCache();

    // 1. Repaint the damaged regions of the content into the cache.
    if (!damage_.empty()) {
        nvgluBindFramebuffer(cache_->fb);
        glViewport(0, 0, cache_->width, cache_->height);
        const Color ground = theme_.color("ground");
        glEnable(GL_SCISSOR_TEST);
        glClearColor(ground.r, ground.g, ground.b, 1.0f);
        std::vector<Rect> regions;
        for (const Rect& d : damage_) regions.push_back(d.snapOut(ratio));
        for (const Rect& r : regions) {
            // GL's origin is bottom-left.
            const auto px = static_cast<GLint>(std::lround(r.x * ratio));
            const auto py = static_cast<GLint>(std::lround((h - r.bottom()) * ratio));
            glScissor(px, py, static_cast<GLsizei>(std::lround(r.w * ratio)),
                      static_cast<GLsizei>(std::lround(r.h * ratio)));
            glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        }
        glDisable(GL_SCISSOR_TEST);
        nvgBeginFrame(vg, w, h, ratio);
        for (const Rect& r : regions) {
            nvgScissor(vg, r.x, r.y, r.w, r.h);
            if (content_) paintTree(*content_, canvas, r);
            nvgResetScissor(vg);
            stats_.repaintedArea += static_cast<double>(r.w) * static_cast<double>(r.h);
        }
        nvgEndFrame(vg);
        ++stats_.repaints;
        damage_.clear();
    }

    // 2. Blit the cache to the window, then paint the overlays on top.
    glBindFramebuffer(GL_READ_FRAMEBUFFER, cache_->fb->fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, cache_->width, cache_->height, 0, 0, cache_->width, cache_->height,
                      GL_COLOR_BUFFER_BIT, GL_NEAREST);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, cache_->width, cache_->height);
    if (!overlay_->children().empty()) {
        glClear(GL_STENCIL_BUFFER_BIT);
        nvgBeginFrame(vg, w, h, ratio);
        paintTree(*overlay_, canvas, {0, 0, w, h});
        nvgEndFrame(vg);
    }
    animate_ = false;
    ++stats_.frames;
    if (present) window_.present();
}

void Root::focus(Node* node) {
    if (node == focused_) return;
    Node* old = focused_;
    focused_ = node;
    if (old) {
        old->onFocus(false);
        old->invalidate();
    }
    if (node) {
        node->onFocus(true);
        node->invalidate();
    }
    const bool wantsText = node && node->role() == Role::TextField;
    if (wantsText)
        SDL_StartTextInput(window_.sdl());
    else if (old && old->role() == Role::TextField)
        SDL_StopTextInput(window_.sdl());
}

void Root::forget(Node* node) {
    if (focused_ == node) focused_ = nullptr;
    if (hovered_ == node) hovered_ = nullptr;
    if (captured_ == node) captured_ = nullptr;
}

void Root::pointer(PointerEvent e) {
    // A press outside every popup closes them and goes no further.
    if (e.type == PointerEvent::Type::Down && !popups_.empty() && !captured_) {
        const bool inside = std::any_of(popups_.begin(), popups_.end(),
                                        [&](const Node* p) { return p->bounds().contains(e.pos); });
        if (!inside) {
            closePopups();
            return;
        }
    }
    Node* target = captured_;
    if (!target) {
        target = overlay_->hitTest(e.pos);
        if (!target && content_) target = content_->hitTest(e.pos);
    }
    if (e.type == PointerEvent::Type::Move && !captured_ && target != hovered_) {
        if (hovered_) {
            hovered_->onHover(false);
            hovered_->invalidate();
        }
        hovered_ = target;
        if (hovered_) {
            hovered_->onHover(true);
            hovered_->invalidate();
        }
    }
    if (e.type == PointerEvent::Type::Down) {
        Node* f = target;
        while (f && !f->focusable) f = f->parent();
        focus(f);
        captured_ = target;  // implicit capture for drags
    }
    if (target) deliver(target, e);
    if (e.type == PointerEvent::Type::Up) captured_ = nullptr;
}

bool Root::click(std::string_view id, int button) {
    Node* n = find(id);
    if (!n || !n->visible || !n->enabled) return false;
    const Point c{n->bounds().x + n->bounds().w / 2, n->bounds().y + n->bounds().h / 2};
    pointer({PointerEvent::Type::Move, c});
    pointer({PointerEvent::Type::Down, c, button});
    pointer({PointerEvent::Type::Up, c, button});
    return true;
}

void Root::key(int keycode, unsigned mods) {
    const KeyEvent e{keycode, mods, false};
    for (Node* n = focused_; n; n = n->parent())
        if (n->enabled && n->onKey(e)) return;
    if (keycode == SDLK_ESCAPE && !popups_.empty()) {
        closePopup();
        return;
    }
    if (popups_.empty() && onUnhandledKey && onUnhandledKey(e)) return;
    if (keycode == SDLK_TAB) {  // focus traversal
        std::vector<Node*> order;
        collectFocusable(*overlay_, order);
        if (content_) collectFocusable(*content_, order);
        if (order.empty()) return;
        auto it = std::find(order.begin(), order.end(), focused_);
        if (mods & kShift)
            focus(it == order.end() || it == order.begin() ? order.back() : *(it - 1));
        else
            focus(it == order.end() || it + 1 == order.end() ? order.front() : *(it + 1));
    }
}

void Root::text(std::string_view utf8) {
    if (focused_ && focused_->enabled) focused_->onText(utf8);
}

void Root::composition(std::string_view utf8, int cursor) {
    if (focused_ && focused_->enabled) focused_->onComposition(utf8, cursor);
}

void Root::setTextInputArea(const Rect& caret) {
    const SDL_Rect r{static_cast<int>(caret.x), static_cast<int>(caret.y),
                     std::max(1, static_cast<int>(caret.w)),
                     std::max(1, static_cast<int>(caret.h))};
    SDL_SetTextInputArea(window_.sdl(), &r, 0);
}

std::string Root::clipboard() const {
    char* text = SDL_GetClipboardText();
    std::string out = text ? text : "";
    SDL_free(text);
    return out;
}

void Root::setClipboard(const std::string& text) { SDL_SetClipboardText(text.c_str()); }

void Root::handle(const SDL_Event& e) {
    switch (e.type) {
        case SDL_EVENT_MOUSE_MOTION:
            pointer({PointerEvent::Type::Move,
                     {e.motion.x, e.motion.y},
                     0,
                     1,
                     modsFrom(SDL_GetModState())});
            break;
        case SDL_EVENT_MOUSE_BUTTON_DOWN:
        case SDL_EVENT_MOUSE_BUTTON_UP:
            pointer({e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ? PointerEvent::Type::Down
                                                           : PointerEvent::Type::Up,
                     {e.button.x, e.button.y},
                     buttonFrom(e.button.button),
                     e.button.clicks,
                     modsFrom(SDL_GetModState())});
            break;
        case SDL_EVENT_MOUSE_WHEEL: {
            PointerEvent p{PointerEvent::Type::Wheel, {e.wheel.mouse_x, e.wheel.mouse_y}};
            p.mods = modsFrom(SDL_GetModState());
            p.wheelX = e.wheel.x;
            p.wheelY = e.wheel.y;
            pointer(p);
            break;
        }
        case SDL_EVENT_WINDOW_MOUSE_LEAVE:
            if (hovered_) {
                hovered_->onHover(false);
                hovered_->invalidate();
                hovered_ = nullptr;
            }
            break;
        case SDL_EVENT_KEY_DOWN:
            key(static_cast<int>(e.key.key), modsFrom(e.key.mod));
            break;
        case SDL_EVENT_TEXT_INPUT:
            text(e.text.text);
            break;
        case SDL_EVENT_TEXT_EDITING:
            composition(e.edit.text ? e.edit.text : "", e.edit.start);
            break;
        case SDL_EVENT_WINDOW_RESIZED:
        case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
        case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
            relayout();
            damageAll();
            break;
        case SDL_EVENT_WINDOW_EXPOSED:
            animate_ = true;  // the cache is intact; just present it again
            break;
        default:
            break;
    }
}

}  // namespace supple::ui
