// The MIDI editor (docs/specs/gui.md §4.7), docked under the arrange view: a
// piano roll (draw, select, move, resize, velocity, quantize, transpose) and
// a velocity lane over midi.notes / midi.edit, and the notation panel — the
// item's notation text beside the roll, editable, compiled live with located
// diagnostics, written back with midi.write. Either side's edits reach the
// other through the session: one model, two views.
#pragma once

#include <chrono>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "app_actions.h"
#include "supple/ui/root.h"
#include "supple/ui/text_area.h"

namespace supple::ui {
class Button;
class Label;
class TextBlock;
}  // namespace supple::ui

namespace supple::gui {

class MidiEventList;

struct RollNote {
    int pitch = 60;
    double start = 0, length = 1;  // beats from the item's start
    int velocity = 96, channel = 1;
};

// What the roll, keyboard and velocity lane share.
struct CcPoint {
    double beat = 0;  // from the item's start
    int value = 0;    // 0..127
};

struct RollState {
    std::uint64_t gestureVersion = 0;
    std::string item;         // "it_3"; empty: closed
    double itemPosition = 0;  // project beats
    double itemLength = 4;    // beats
    std::vector<RollNote> notes;
    std::set<std::size_t> selected;
    // The CC lane: one controller at a time (channel 1).
    int ccNumber = 1;
    std::vector<CcPoint> cc;
    std::vector<CcPoint> ccDraft;  // a drag in progress (drawn, not yet committed)
    bool ccErasing = false;
    double ccFrom = 0, ccTo = 0;  // the drag's span (beats)
    // View: beats from the item start at the left edge, the pitch at the top
    // edge, zoom.
    double scrollBeat = -0.5, pixelsPerBeat = 80;
    double topPitch = 84;
    float rowHeight = 12;
    double grid = 0.25;  // beats
    bool snap = true;
    double lastLength = 0.25;
    int lastVelocity = 96;
    // A drag in progress, previewed locally and committed once on release.
    enum class Drag { None, Move, ResizeEnd, ResizeStart, Marquee, Velocity } drag = Drag::None;
    double dBeats = 0, dLength = 0, dStart = 0;
    int dPitch = 0, dVelocity = 0;
    std::optional<std::size_t> velocityNote;  // the note a velocity drag picked
    ui::Rect marquee{};
    // Project grid for bars/beats (from the time map).
    std::vector<double> barLines;  // project beats of bar starts in view
    double playheadBeat = -1;      // project beats; < 0: hidden
    void cancelGesture() {
        drag = Drag::None;
        dBeats = dLength = dStart = 0;
        dPitch = dVelocity = 0;
        velocityNote.reset();
        ccDraft.clear();
        ccErasing = false;
    }
    double snapBeat(double itemBeat) const;  // to the project grid when snapping
};

class MidiEditor {
  public:
    MidiEditor(ui::Root& root, AppActions& actions, cmd::Dispatcher& dispatcher,
               engine::EngineApp& app, UiState& ui);
    ~MidiEditor();

    std::unique_ptr<ui::Node> build();  // the docked panel (hidden until opened)
    Status open(const std::string& item);
    void close();
    bool isOpen() const { return !state_.item.empty(); }
    // Every loop pass: reload when the session changed, compile the notation
    // after a pause in typing, follow the playhead.
    void sync();
    std::optional<std::chrono::steady_clock::time_point> nextDeadline() const;

    // The panel's height, set by dragging its top border (double-click resets
    // to kDefaultHeight). The stored height is the preference; sync() shrinks
    // what is laid out when the window cannot fit it.
    static constexpr float kDefaultHeight = 420, kMinHeight = 160;
    float height() const { return height_; }
    void setHeight(float height);
    json::Value saveLayout() const;  // {"height"}: the UI sidecar's "midi" section
    void loadLayout(const json::Value& v);
    // Row (key) height zoom, anchored at the window y whose pitch stays put.
    static constexpr float kMinRowHeight = 4, kMaxRowHeight = 48;
    void zoomRows(float wheel, float anchorY);

    // Tests and the actions.
    RollState& state() { return state_; }
    ui::Node* roll() const { return roll_; }
    ui::TextArea* notation() const { return text_; }
    MidiEventList* eventList() const { return eventList_.get(); }
    std::uint64_t beginGesture();
    Status edit(json::Value ops, std::optional<std::uint64_t> expectedVersion =
                                     {});  // midi.edit on the open item, keeping the selection
    void selectAll();
    void quantize();
    void transpose(int semitones);
    void removeSelected();
    void duplicate();

  private:
    Status reload();
    void reloadText();
    // `submit`: the user pressed Ctrl+Enter, which also overwrites MIDI that
    // changed elsewhere while the draft was being typed.
    void compileText(bool write, bool submit = false);
    std::optional<std::string> modelText() const;  // the item's MIDI, decompiled
    void refreshTitle();
    float maxHeight() const;
    void applyHeight();
    void showDiagnostics(const std::vector<ui::TextArea::Mark>& marks, const std::string& summary);
    ui::Root& root_;
    AppActions& actions_;
    cmd::Dispatcher& dispatcher_;
    engine::EngineApp& app_;
    UiState& ui_;
    RollState state_;
    std::string lastItem_;  // the item opened last (ui.midi.toggle reopens it)
    float height_ = kDefaultHeight;
    ui::Node* panel_ = nullptr;
    std::unique_ptr<MidiEventList> eventList_;
    ui::Node* listPane_ = nullptr;
    ui::Node* rollPane_ = nullptr;
    ui::Node* roll_ = nullptr;
    ui::Node* keys_ = nullptr;
    ui::Node* velocity_ = nullptr;
    ui::Node* ccLane_ = nullptr;
    ui::Button* ccButton_ = nullptr;
    ui::Node* notationPane_ = nullptr;
    ui::Label* title_ = nullptr;
    ui::TextArea* text_ = nullptr;
    ui::TextBlock* diagnostics_ = nullptr;
    ui::Root::OverlayHandle playheadHandle_;
    ui::Node* playhead_ = nullptr;  // in the root's overlay
    std::uint64_t version_ = 0;
    std::uint64_t gestureGeneration_ = 0;
    std::uint64_t projectGeneration_ = ~std::uint64_t{0};
    std::string appliedText_;  // the text last compiled into the item
    std::string titleBase_;
    bool textDirty_ = false, compilePending_ = false;
    bool textConflict_ = false;        // the MIDI changed under a draft: only Ctrl+Enter writes it
    std::string baseText_;             // the item's MIDI, decompiled, when the text last matched it
    std::uint64_t typingSession_ = 0;  // one undo step per focus of the notation panel
    std::chrono::steady_clock::time_point lastEdit_{};
};

}  // namespace supple::gui
