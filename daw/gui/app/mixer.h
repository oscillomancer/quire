// The mixer (docs/specs/gui.md §4.4), docked under the arrange view: strips
// in track order grouped by type — channels, busses, masters — each with its
// type colour, a small "fx" button at its top right, the plug-in chain (click:
// the plug-in's editor; drag: reorder it, or copy it onto another strip),
// sends, input/arm/monitor on channels, pan, fader and meter, mute and solo;
// Console 9 as a project toggle in the header. The panel's top edge resizes it
// (only the chain section grows). Dragging a strip's empty part reorders it
// among its own kind. Every change is a command; a fader or pan drag, a
// reorder or a copy is one undo step.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "app_actions.h"
#include "meter_feed.h"
#include "supple/ui/root.h"
#include "track_menu.h"

namespace supple::ui {
class Label;
class Fader;
class PanControl;
class Toggle;
class Button;
class Meter;
}  // namespace supple::ui

namespace supple::gui {

class Mixer {
  public:
    Mixer(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher, engine::EngineApp& app,
          MeterFeed& meters);
    ~Mixer();
    std::unique_ptr<ui::Node> build();  // the docked panel (hidden until shown)
    void toggle();
    void setVisible(bool visible);
    bool visible() const;
    void sync();  // every loop pass

    // The panel's height (UI state, persisted with the panels): clamped to what
    // the window can give it; the default fits about five plug-ins.
    float height() const { return height_; }
    void setHeight(float height);
    static float defaultHeight();
    json::Value saveLayout() const;  // the "mixer" section of the UI sidecar
    void loadLayout(const json::Value& v);

    // Tests.
    struct Strip {
        std::string track, type;
        ui::Node* node = nullptr;
        ui::Label* name = nullptr;
        ui::Fader* fader = nullptr;
        ui::PanControl* pan = nullptr;
        ui::Toggle *mute = nullptr, *solo = nullptr, *arm = nullptr;
        ui::Button* monitor = nullptr;
        ui::Label* value = nullptr;
        ui::Meter* meter = nullptr;
        ui::Button* fx = nullptr;   // "fx" at the strip's top right
        ui::Node* chain = nullptr;  // the plug-in list
    };
    const std::vector<Strip>& strips() const { return strips_; }

    enum class DragPhase { Begin, Move, End, Cancel };
    // A drag in progress, for the feedback layer (and tests).
    struct DragView {
        enum class Kind { None, Strip, Fx } kind = Kind::None;
        enum class Drop { None, Reorder, Copy } drop = Drop::None;
        bool active = false;  // past the movement threshold
        std::string track;    // the dragged strip, or the plug-in's own strip
        std::string fx;       // the dragged plug-in
        std::string label;    // chip text (plug-ins)
        ui::Point pos;
        ui::Rect source, marker;  // the dragged item; the insertion bar or target strip
        std::string target;       // Copy: the strip dropped on
        std::size_t index = 0;    // Reorder: the track.move / fx.move index
    };
    const DragView& drag() const { return *drag_; }

  private:
    void run(std::string_view action, json::Value args);  // an app action; errors to onError
    float maxHeight() const;
    void applyHeight();
    void rebuild();
    void update();
    void dragStrip(const std::string& track, DragPhase phase, ui::Point p);
    void dragFx(const std::string& track, model::Id fx, DragPhase phase, ui::Point p);
    void endDrag(bool commit);
    void resolveStripDrop();
    void resolveFxDrop();
    const Strip* stripAt(ui::Point p) const;
    std::string structure() const;
    Result<json::Value> set(const std::string& track, const char* field, json::Value value,
                            const std::string& gesture = {});
    ui::Root& root_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    MeterFeed& meters_;
    TrackMenu trackMenu_;  // a strip's right-click menu
    ui::Node* panel_ = nullptr;
    ui::Node* row_ = nullptr;  // the strips' container
    ui::Toggle* console9_ = nullptr;
    std::vector<Strip> strips_;
    float height_ = defaultHeight(), shown_ = 0;  // asked for; on screen
    std::shared_ptr<DragView> drag_ = std::make_shared<DragView>();
    ui::Root::OverlayHandle ghost_;
    std::string structure_;
    std::uint64_t version_ = ~0ull;
    std::uint64_t gesture_ = 0;  // a serial per drag: separate drags are separate undo steps
};

}  // namespace supple::gui
