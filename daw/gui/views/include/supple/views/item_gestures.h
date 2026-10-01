// Editing gestures in the arrange view (docs/specs/gui.md §4.3, P5.6;
// reaper-ux.md 8-13): select (click, Shift add, Ctrl toggle, marquee on empty
// space or a right-drag anywhere), move in time and across channels (snapped
// to the grid at the current zoom; Shift moves freely; Ctrl-drag copies), trim either edge (the
// left edge moves the take's source offset so the audio stays put; Alt on the
// right edge stretches: the play rate follows the length), slip
// (Alt-drag: the source moves under the item), fade handles at the top
// corners, the take badge cycles takes, double-click opens a MIDI item, and
// files dropped on the lanes. On an automation lane: click adds a point, drag
// moves one (snapped; Shift frees), Alt+click deletes one. Each gesture previews as a ghost on the
// root's overlay and commits ONE transaction on release.
#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "supple/base/result.h"
#include "supple/json/json.h"
#include "supple/model/session.h"
#include "supple/ui/geometry.h"

namespace supple::ui {
class Root;
class Node;
struct PointerEvent;
}  // namespace supple::ui

namespace supple::views {

class ArrangeView;

struct EditStep {
    std::string command;
    json::Value args;
};

class ItemGestures {
  public:
    // Runs the steps as one transaction (one undo step) labelled `label`; the
    // result is the last step's.
    using Runner = std::function<Result<json::Value>(const std::string& label,
                                                     const std::vector<EditStep>& steps)>;
    explicit ItemGestures(ArrangeView& view);
    ~ItemGestures();

    Runner run;
    std::function<void(const std::vector<model::Id>&)> onSelection;  // after every change
    std::function<void(model::Id item)> onOpenMidi;                  // double-click on a MIDI item
    std::function<void(double seconds)> onSeek;                      // a click on empty space
    std::function<void(const Error&)> onError;  // a refused gesture (status line)
    // REAPER's default: a move, copy or trim that makes two items overlap on a
    // track fades both over the overlap (in the same transaction).
    bool autoCrossfade = true;

    const std::vector<model::Id>& selection() const { return selection_; }
    void setSelection(std::vector<model::Id> items);  // no onSelection call
    bool isSelected(model::Id item) const;

    // From ArrangeView::onPointer (everything but the wheel); true when used.
    bool pointer(const ui::PointerEvent& e);
    void installOverlay(ui::Root& root);  // the ghost / selection layer
    // Files dropped at a window point: one transaction (an item per file, on
    // the channel under the point, further files on new tracks below).
    Status drop(const std::vector<std::filesystem::path>& files, ui::Point at);

    // The snap grid at the current zoom, in beats (1/16 .. 32).
    double gridBeats() const;
    // An empty MIDI item on a channel from the start of the bar holding
    // `fromBeat` to `toBeat` (at least that bar), selected and opened
    // (onOpenMidi): one transaction. Also a double-click on empty lane space.
    Result<model::Id> newMidiItem(model::Id track, double fromBeat,
                                  std::optional<double> toBeat = {});
    // The play rate that keeps a take's material filling the item when its
    // end moves by `deltaBeats` (the stretch gesture).
    double stretchRate(const model::Item& item, const model::Take& take, double deltaBeats) const;

    // What a drag is doing (the overlay and tests read it).
    enum class Drag { None, Move, TrimLeft, TrimRight, Slip, FadeIn, FadeOut, Marquee, EnvPoint };
    struct State {
        Drag drag = Drag::None;
        model::Id anchor{};      // the item under the press
        double deltaBeats = 0;   // move / trim-left / trim-right / slip, snapped
        int deltaRows = 0;       // move
        double fadeSeconds = 0;  // fade in/out length being drawn
        bool stretch = false;    // Alt on the right edge: length and play rate together
        ui::Rect marquee{};
        std::vector<model::Id> marqueeBase;
        // An automation lane's point being added or dragged (P5.14): a click
        // on the lane adds one, a drag moves it, Alt+click deletes one.
        std::optional<model::Id> env;
        std::vector<model::EnvPoint> envDraft;
        std::size_t envIndex = 0;
        double envOrigPos = 0;
        bool envCreated = false;
    };
    const State& state() const { return state_; }

    // Geometry helpers (also the overlay's).
    struct Hit {
        model::Id item{};
        std::size_t row = 0;
        ui::Rect rect{};
        Drag zone = Drag::None;
        bool takeBadge = false;
    };
    std::optional<Hit> hit(ui::Point p) const;
    std::optional<std::size_t> rowAt(float y) const;
    ArrangeView& view() { return view_; }
    const ArrangeView& view() const { return view_; }

  private:
    double beatAtX(float x) const;
    bool envPress(std::size_t row, const ui::PointerEvent& e);
    void envDrag(const ui::PointerEvent& e);
    void commitEnv(const std::string& label);
    double snap(double beat, bool free) const;
    void commit(const ui::PointerEvent& e);
    void changed();
    ArrangeView& view_;
    std::vector<model::Id> selection_;
    State state_;
    ui::Point press_{};
    bool moved_ = false, free_ = false, copy_ = false;
    std::size_t pressRow_ = 0;
    std::shared_ptr<ItemGestures*> alive_;  // the overlay's ghost checks it
    ui::Root* root_ = nullptr;
};

}  // namespace supple::views
