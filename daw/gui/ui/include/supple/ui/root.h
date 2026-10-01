// One window's UI (docs/specs/gui.md §2): the content tree, an overlay tree
// (popups, drag ghosts, playhead-like layers painted every frame), focus,
// hover, pointer capture and damage. Content repaints only damaged regions
// into a cached framebuffer; each frame blits the cache, paints overlays
// and presents.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "supple/gui/window.h"
#include "supple/ui/node.h"
#include "supple/ui/popup.h"

union SDL_Event;

namespace supple::ui {

class Root {
  public:
    Root(gui::Window& window, const gui::Fonts& fonts, Theme theme);
    ~Root();
    Root(const Root&) = delete;
    Root& operator=(const Root&) = delete;

    void setContent(std::unique_ptr<Node> content);
    Node* content() const { return content_.get(); }
    Node& overlay() { return *overlay_; }
    Node* find(std::string_view id) const;
    const Theme& theme() const { return theme_; }
    void setTheme(Theme theme);

    // --- window integration
    void handle(const SDL_Event& e);
    bool wantsFrame() const;
    // Lays out if needed, repaints damage into the cache, blits it, paints the
    // overlays. present=false leaves the frame in the back buffer (snapshots).
    void frame(bool present = true);
    gui::Image readBack() const { return window_.readPixels(); }

    // --- damage and layout
    void damage(const Rect& area);
    void damageAll();
    void relayout() { layoutPending_ = true; }
    void requestAnimationFrame() { animate_ = true; }

    // --- focus, hover, capture
    void focus(Node* node);
    Node* focused() const { return focused_; }
    Node* hovered() const { return hovered_; }
    void capture(Node* node) { captured_ = node; }
    void forget(Node* node);  // a node is going away

    // --- popups (popup.h): returns the content node, now owned by the overlay
    Node& openPopup(std::unique_ptr<Node> content, const Rect& anchor,
                    Placement placement = Placement::Below, std::function<void()> onClose = {});
    void closePopup();  // the topmost
    void closePopups();
    bool popupOpen() const { return !popups_.empty(); }

    // --- driving the UI from tests and scripts (same paths as SDL input)
    void pointer(PointerEvent e);
    bool click(std::string_view id, int button = 1);
    void key(int keycode, unsigned mods = 0);
    void text(std::string_view utf8);
    void composition(std::string_view utf8, int cursor);
    // Where the focused text field's caret is (design units), for the IME
    // candidate window.
    void setTextInputArea(const Rect& caret);
    // Clipboard (the system's; tests may run without one).
    std::string clipboard() const;
    void setClipboard(const std::string& text);

    struct Stats {
        std::uint64_t frames = 0, repaints = 0;
        double repaintedArea = 0;  // design units², content cache only
    };
    const Stats& stats() const { return stats_; }

  private:
    void layoutNow(const Canvas& canvas);
    void paintTree(Node& node, Canvas& canvas, const Rect& clip);
    void paintGl(Node& node, const Rect& clip);
    void ensureCache();
    gui::Window& window_;
    gui::Fonts fonts_;
    Theme theme_;
    std::unique_ptr<Node> content_, overlay_;
    Node* focused_ = nullptr;
    Node* hovered_ = nullptr;
    Node* captured_ = nullptr;
    std::vector<Rect> damage_;
    bool layoutPending_ = true, animate_ = false;
    Size laidOut_{};
    float ratio_ = 1;  // of the frame being painted
    struct Cache;
    std::unique_ptr<Cache> cache_;
    Stats stats_;
    std::vector<Node*> popups_;  // PopupFrames owned by overlay_, topmost last
};

}  // namespace supple::ui
