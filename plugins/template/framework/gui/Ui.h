// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Controls: the interaction half of every knob, slider, toggle, segmented
// switch, dropdown, button, XY pad, tab bar, list, keyboard and envelope.
// Drawing lives in Widgets.h; this file decides what a press, drag, wheel
// turn or key does, and what the host is told. A readout only displays, and
// so does any control bound to an output parameter.
//
// The rules, enforced here so no product has to remember them:
//
//   - One host gesture is open at a time: one control's, touching at most
//     two parameters together (an XY pad's two, an envelope corner's decay
//     and sustain), or a product's own drawing's, touching every parameter
//     it edits from a press to its release. Anything else that starts an
//     edit closes the open gesture first. A parameter's begin is sent with
//     its first change, so a pad moved only sideways never touches the
//     vertical parameter's automation.
//   - Dragging: vertical for knobs, along the track for sliders, absolute on
//     an XY pad or an envelope's handles. Shift is fine adjustment, and
//     pressing or releasing it mid-drag re-anchors rather than jumping. The
//     gesture opens on the first actual change.
//   - Wheel and keyboard adjustments are one gesture per burst, closed after
//     250 ms without input (measured with real time from idle()).
//   - Double-click resets to the default. Ctrl- or Cmd-click, or Enter on a
//     focused knob or slider, opens a text field that accepts anything the
//     parameter's parser does ("1.2k", "-6", "50%").
//   - A right-click opens the control's context menu: the host's own, with
//     the plugin's items in it, where the host can show one; otherwise the
//     editor's, with reset and enter-value first and the host's items
//     (automation, MIDI learn) after them.
//   - Tab and Shift-Tab move focus in the order controls were added; arrows
//     adjust (Shift: fine), Page Up/Down in tenths, Home/End to the limits,
//     Backspace/Delete to the default, Enter toggles or opens, Escape backs
//     out. Space is deliberately unbound: it belongs to the host's transport.
//   - A note the keyboard starts always ends: on release, on sliding to
//     another key, and on losing focus.
//   - A text field selects what a click-drag, Shift-click or Shift-arrow
//     covers, a word on double-click; Ctrl- or Cmd-A selects all, and
//     -C, -X and -V copy, cut and paste.
//   - Losing focus ends every gesture and closes every overlay.
//   - Hovering still over a control for a moment shows its tooltip: the
//     parameter's help() text, or the control's own.
//
// Some controls are not bound to a parameter: a text field edits a property
// (core/Property.h) by its ID, the preset bar and preset list drive the
// preset library (core/Presets.h), a button runs the product's code, a tab
// bar switches pages, and a list can show the product's own items.
// popup() and prompt() give a product's own drawing the same menu and text
// field the controls use.
//
// Coordinates are design units throughout; Window converts.
#pragma once

#include <algorithm>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/Param.h"
#include "gui/EditorHost.h"
#include "gui/Layout.h"
#include "gui/Menu.h"

namespace supple::gui {

enum class Widget : uint8_t {
    Knob, HSlider, VSlider, Toggle, Segmented, Dropdown, Readout, TextField, PresetBar,
    Button, Momentary, XYPad, Tabs, List, Keyboard, Envelope, File,
};

// An unused parameter slot.
inline constexpr uint32_t kNoParam = 0xFFFFFFFFu;

// The rows of a list the product fills (a file browser, a bank). Main thread.
class ListSource {
  public:
    virtual ~ListSource() = default;
    virtual int size() const = 0;
    virtual std::string item(int row) const = 0;
    // A header row is drawn as a caption and cannot be chosen.
    virtual bool header(int row) const { (void)row; return false; }
    virtual int selected() const = 0;  // -1 for none
    virtual void choose(int row) = 0;
};

// What the editor can put on the system clipboard and read back. Window
// provides it; tests and tools may not, and then copy and paste are off.
class Clipboard {
  public:
    virtual ~Clipboard() = default;
    virtual bool copy(std::string_view text) = 0;
    // The text arrives later, from the window's event loop, or never.
    virtual bool paste(std::function<void(std::string_view)> done) = 0;
};

// The system's open and save dialogs, which Window provides; tests and tools
// may not. Each returns at once: done() runs later, on the main thread, with
// the paths chosen (UTF-8), or with none when the dialog was cancelled. Only
// one dialog is open at a time; asking while one is returns false.
class FileDialogs {
  public:
    struct Options {
        std::string title;
        std::vector<std::string> extensions;  // ".wav", ".preset"; empty for any file
        std::string name;                     // save: the suggested file name
        std::string folder;                   // where it opens; empty for the system's choice
        bool multiple = false;                // open: more than one file
    };
    using Done = std::function<void(const std::vector<std::string>& paths)>;
    virtual ~FileDialogs() = default;
    virtual bool open(const Options& options, Done done) = 0;
    virtual bool save(const Options& options, Done done) = 0;
};

// The parts of a preset bar, left to right.
namespace presetbar {
inline constexpr int kPrevious = 0, kName = 1, kNext = 2, kCompare = 3;
}  // namespace presetbar

enum class Cursor : uint8_t { Arrow, Hand, Caret, LeftRight, UpDown, Move };

// Modifier bits and key codes. Values match pugl's so Window passes them
// through untouched; printable keys are their Unicode code points.
namespace mod {
inline constexpr uint32_t kShift = 1u << 0;
inline constexpr uint32_t kCtrl = 1u << 1;
inline constexpr uint32_t kAlt = 1u << 2;
inline constexpr uint32_t kSuper = 1u << 3;
}  // namespace mod

namespace key {
inline constexpr uint32_t kBackspace = 0x0008u, kTab = 0x0009u, kEnter = 0x000Du, kEscape = 0x001Bu,
                          kSpace = 0x0020u, kDelete = 0x007Fu, kPageUp = 0xE031u, kPageDown = 0xE032u,
                          kEnd = 0xE033u, kHome = 0xE034u, kLeft = 0xE035u, kUp = 0xE036u,
                          kRight = 0xE037u, kDown = 0xE038u, kPadEnter = 0xE070u;
}  // namespace key

struct Control {
    static constexpr int kSlots = 4;

    uint32_t param = 0;  // a property's ID for a text field; kNoParam for controls without one
    Widget widget = Widget::Knob;
    Rect bounds;
    const char* label = nullptr;  // the parameter's short label when absent
    int variant = 0;              // free for a product's own drawing (e.g. a small knob)
    bool visible = true;
    bool enabled = true;
    // The parameters after the first, for a control with more than one: an
    // XY pad's vertical one; an envelope's decay, sustain and release.
    uint32_t more[kSlots - 1] = {kNoParam, kNoParam, kNoParam};
    const char* tooltip = nullptr;   // replaces the parameter's help() text
    std::function<bool()> rule;      // enabledWhen()
    int tabs = -1, page = 0;         // shown only on this page of that tab bar
    std::function<void()> action;    // a button's
    std::vector<std::string> pages;  // a tab bar's labels
    int selected = 0;                // a tab bar's page
    ListSource* source = nullptr;    // a list's rows
    int scroll = 0;                  // a list's first row shown
    int lowKey = 48, highKey = 72;   // a keyboard's range, inclusive

    Control& withLabel(const char* text) { label = text; return *this; }
    Control& withVariant(int v) { variant = v; return *this; }
    Control& withTooltip(const char* text) { tooltip = text; return *this; }
    // Enabled while rule() is true, checked every idle tick: a free rate
    // while synced, a setting another one makes moot. A disabled control is
    // dimmed and takes no input.
    Control& enabledWhen(std::function<bool()> check) { rule = std::move(check); return *this; }

    // The parameter in slot k (0: `param`), or kNoParam.
    uint32_t paramAt(int k) const { return k == 0 ? param : k < kSlots ? more[k - 1] : kNoParam; }
};

// Everything a widget needs to draw one control.
struct ControlState {
    // One of the control's parameters.
    struct Slot {
        const ParamDef* def = nullptr;  // null for an unused slot
        double value = 0.0;             // host value
        double normalized = 0.0;        // 0..1
        double modulated = 0.0;         // 0..1 including host modulation
    };
    const ParamDef* def = nullptr;  // slot 0's, as are the next three
    double value = 0.0;
    double normalized = 0.0;
    double modulated = 0.0;
    Slot slots[Control::kSlots];
    bool hover = false;
    bool focused = false;
    bool active = false;      // being dragged or edited right now
    bool editing = false;     // its text field is open
    int hoverIndex = -1;      // segment, preset bar part, tab, list row, key or envelope handle under the pointer
    int activeIndex = -1;     // the envelope handle, or the key, being held
    std::string_view text;    // a text field's value; a preset bar's preset name; a file slot's path
    bool modified = false;    // preset bar: changed since the preset loaded
    int slot = 0;             // preset bar: 0 for A, 1 for B
    bool available = true;    // preset bar: there is a preset library
    uint64_t keys[2] = {};    // keyboard: keys held, from anywhere (bit k of keys[k / 64])
    FileStatus::State file = FileStatus::State::Empty;  // file slot: what became of it
    std::string detail;       // file slot: the loaded file's summary, or why it failed

    const char* label(const Control& c) const { return c.label ? c.label : def ? def->displayLabel() : ""; }
    bool keyHeld(int key) const { return key >= 0 && key < 128 && (keys[key >> 6] >> (key & 63) & 1u); }
};

class Ui {
  public:
    static constexpr double kGestureIdleSeconds = 0.25;
    static constexpr double kTooltipSeconds = 0.6;
    static constexpr float kListRow = 22.0f;

    Ui(EditorHost& host, float designWidth, float designHeight);
    ~Ui();

    // --- building -----------------------------------------------------------
    Control& add(uint32_t param, Widget widget, Rect bounds);
    Control& knob(uint32_t param, Rect r) { return add(param, Widget::Knob, r); }
    Control& hslider(uint32_t param, Rect r) { return add(param, Widget::HSlider, r); }
    Control& vslider(uint32_t param, Rect r) { return add(param, Widget::VSlider, r); }
    Control& toggle(uint32_t param, Rect r) { return add(param, Widget::Toggle, r); }
    Control& segmented(uint32_t param, Rect r) { return add(param, Widget::Segmented, r); }
    Control& dropdown(uint32_t param, Rect r) { return add(param, Widget::Dropdown, r); }
    // A value with its label and a bar, for display only: typically an
    // output parameter (a gain reduction meter the host can show too).
    Control& readout(uint32_t param, Rect r) { return add(param, Widget::Readout, r); }
    // A property's value as one line of text, edited in place.
    Control& textField(uint32_t property, Rect r) { return add(property, Widget::TextField, r); }
    // A file() property's file: its name and what became of it (loading,
    // loaded, missing). Clicking opens a dialog, a file dropped on it is
    // taken, and the right-click menu clears it.
    Control& fileSlot(uint32_t property, Rect r) { return add(property, Widget::File, r); }
    // Previous, the preset's name (a menu of presets and what to do with
    // them), next, and A/B. Needs PRODUCT_PRESETS ON.
    Control& presetBar(Rect r) { return add(kNoParam, Widget::PresetBar, r); }
    // Every preset, by category, as a list: choosing one loads it.
    Control& presetList(Rect r);
    // Runs action() when pressed (or Enter while focused). No parameter.
    Control& button(Rect r, const char* text, std::function<void()> action);
    // On while held: a toggle (or any stepped parameter) at its maximum
    // from press to release, one gesture, back to its minimum after.
    Control& momentary(uint32_t param, Rect r) { return add(param, Widget::Momentary, r); }
    // Two parameters, across and up, dragged together.
    Control& xyPad(uint32_t across, uint32_t up, Rect r);
    // Pages: controls added after onPage(k) show only while this tab bar
    // is on page k, until onAllPages(). page(control) is the one showing.
    Control& tabs(Rect r, std::vector<std::string> labels);
    void onPage(int page);
    void onAllPages() { building_ = -1; }
    int page(int control) const;
    void setPage(int control, int page);
    // A choice parameter's options as a scrolling list, or the product's rows.
    Control& list(uint32_t param, Rect r) { return add(param, Widget::List, r); }
    Control& list(Rect r, ListSource& rows);
    // Keys from lowKey to highKey: pressing one plays it through the
    // processor's note() (EditorHost::playNote), sliding plays the next;
    // keys held anywhere (the host, MIDI) light up. A product with note input.
    Control& keyboard(Rect r, int lowKey = 48, int highKey = 72);
    // Attack, decay, sustain and release, each dragged by a handle on the
    // curve; kNoParam leaves a stage out (an AR envelope).
    Control& envelope(Rect r, uint32_t attack, uint32_t decay, uint32_t sustain, uint32_t release);

    float designWidth() const { return designW_; }
    float designHeight() const { return designH_; }
    int count() const { return static_cast<int>(controls_.size()); }
    const Control& control(int i) const { return controls_[static_cast<size_t>(i)]; }
    Control& control(int i) { return controls_[static_cast<size_t>(i)]; }
    // The first control bound to this parameter (in any slot, not a text
    // field), or -1.
    int find(uint32_t param) const;
    ControlState state(int i) const;
    // Visible, and on the page showing, if it is on a page.
    bool shown(int i) const;

    // --- input; each returns true when it used the event ---------------------
    bool pointerDown(float x, float y, int button, uint32_t mods, bool doubleClick);
    bool pointerMove(float x, float y, uint32_t mods);
    bool pointerUp(float x, float y, int button, uint32_t mods);
    void pointerLeave();
    bool wheel(float x, float y, float dx, float dy, uint32_t mods);
    bool key(uint32_t code, uint32_t mods);
    bool text(uint32_t codepoint);
    void focusLost();
    void idle(double seconds);
    // Re-evaluates every enabledWhen() rule; idle() does this too.
    void applyRules();

    // --- state for drawing ------------------------------------------------------
    // Visual state changed since the last call (hover, focus, overlays).
    bool takeDirty();
    // Something time-based is in flight, so idle() must keep being called.
    bool animating() const;
    Cursor cursor() const;
    int focus() const { return focus_; }
    int hover() const { return hover_; }
    bool dragging() const { return drag_.control >= 0; }
    // Takes pointer and keyboard input: shown, enabled, and neither a
    // readout nor bound to an output parameter.
    bool interactive(int control) const;
    // Keys held anywhere, for the keyboard to light (EditorBase passes what
    // telemetry says).
    void setKeysHeld(const uint64_t keys[2]);
    // Files dragged over the editor: the file slot under (x, y) that would
    // take them, or -1; dropping them there sets its property (the first
    // file it accepts).
    int fileSlotAt(float x, float y, const std::vector<std::string>& paths) const;
    bool dropFiles(float x, float y, const std::vector<std::string>& paths);
    // The file slot's dialog, as a click opens it.
    void chooseFile(int control);

    struct Menu {
        bool open = false;
        int control = -1;  // the control it belongs to, or -1
        Rect bounds;
        int hover = -1;
        std::vector<MenuItem> items;
        int first = 0;    // the first row shown, when there are more than fit
        int visible = 0;  // rows shown
        float rowHeight = 24.0f;
        std::function<void(int)> chosen;
        int count() const { return static_cast<int>(items.size()); }
        // Row i's place, for i in [first, first + visible).
        Rect row(int i) const {
            return {bounds.x, bounds.y + 4.0f + rowHeight * static_cast<float>(i - first), bounds.w, rowHeight};
        }
        bool scrollsUp() const { return first > 0; }
        bool scrollsDown() const { return first + visible < count(); }
    };
    struct TextEdit {
        static constexpr int kCapacity = 256;
        bool open = false;
        int control = -1;
        Rect bounds;
        char buffer[kCapacity] = {};
        int length = 0;
        int caret = 0;
        int anchor = 0;             // the selection's other end; the caret's own place when none
        int limit = kCapacity - 1;  // bytes
        bool ascii = true;          // a value for a parser; otherwise UTF-8 text
        float scroll = 0.0f;        // text slides left this far to keep the caret in view
        bool selecting = false;     // a press in it is dragging out a selection
        bool caretOn = true;
        double blink = 0.0;
        bool rejected = false;  // the last commit was refused
        std::function<bool(std::string_view)> commit;  // a prompt's; a value's goes to its parameter
        bool selected() const { return anchor != caret; }
        int from() const { return std::min(anchor, caret); }
        int to() const { return std::max(anchor, caret); }
    };
    struct Tooltip {
        bool open = false;
        int control = -1;
        Rect anchor;        // the control it describes
        std::string title;  // the parameter's full name, or empty
        std::string text;
    };
    const Menu& menu() const { return menu_; }
    const TextEdit& textEdit() const { return text_; }
    const Tooltip& tooltip() const { return tooltip_; }

    // For a product's own interactive drawing -- a graph handle, a node --
    // that edits the parameter behind control i the way a drag would. Ui
    // keeps the one-gesture rule: the drawing's edits until endDragEdit()
    // (at the release) are one gesture, which every parameter they change
    // joins with its first change, however many; any other gesture is
    // closed first. The second form edits slot k of a multi-parameter
    // control; the third, two controls' parameters at once (an EQ node's
    // frequency and gain). For a parameter only the drawing edits, add a
    // control for it with visible = false.
    void dragEdit(int control, double host) { dragEdit(control, 0, host); }
    void dragEdit(int control, int slot, double host) { drawingEdit(control, slot, host, false); }
    void dragEdit(int controlX, double hostX, int controlY, double hostY);
    void endDragEdit() { endGesture(); }
    // The same drawing's wheel (or keys): as the controls' own, one gesture
    // per burst, holding all the burst changes, closed after
    // kGestureIdleSeconds without another.
    void wheelEdit(int control, double host) { drawingEdit(control, 0, host, true); }

    void setFocus(int control);
    void openMenu(int control);
    // A parameter control's context menu, as a right-click at (x, y) opens it.
    void openContextMenu(int control, float x, float y);
    void openTextEdit(int control);

    // A menu below (or above) `anchor`; chosen(index) runs for the item
    // picked, after the menu closed. Nothing runs if it is dismissed.
    void popup(Rect anchor, std::vector<MenuItem> items, std::function<void(int)> chosen,
               float minWidth = 160.0f, int control = -1);
    // A text field over `box` holding `initial`, at most `limit` bytes of
    // UTF-8. commit(text) runs on Enter; returning false keeps it open and
    // marked rejected. Escape, or a click outside, abandons it.
    void prompt(Rect box, std::string_view initial, int limit, std::function<bool(std::string_view)> commit,
                int control = -1);

    // How wide the text field draws its text, in design units: where a
    // click puts the caret depends on it. EditorBase measures with its
    // fonts; without it, each character counts as 7 units.
    using TextWidth = std::function<float(std::string_view text, bool ascii)>;
    void setTextWidth(TextWidth width) { textWidth_ = std::move(width); }
    // Where the open text field's text starts, `width` wide: a value
    // centred, text from the left, slid along by its scroll.
    static float textStart(const TextEdit& e, float width);

    void setClipboard(Clipboard* clipboard) { clipboard_ = clipboard; }
    Clipboard* clipboard() const { return clipboard_; }
    void setFileDialogs(FileDialogs* files) { files_ = files; }
    FileDialogs* fileDialogs() const { return files_; }
    // Where a control's text field opens.
    static Rect valueBox(const Control& c);
    // A preset bar's parts (presetbar::kPrevious ...), for drawing and input.
    static Rect presetPart(const Rect& bar, int part);
    // A tab bar's tab k of n.
    static Rect tab(const Rect& bar, int k, int n);
    // A list's rows that fit, and row k's place (scrolled).
    static int listRows(const Control& c);
    static Rect listRow(const Control& c, int row);
    // A keyboard's key: where it is, and which key is at a point (-1: none).
    static bool blackKey(int key);
    static Rect keyRect(const Control& c, int key);
    static int keyAt(const Control& c, float x, float y);
    // An envelope's handles (0 attack peak, 1 decay/sustain corner, 2
    // release end) for its slots' normalized values; the curve passes
    // through start, the three handles, and the sustain's end.
    struct EnvelopeShape {
        float x[5], y[5];  // start, peak, corner, sustain end, release end
        bool has[3];       // which handles exist
    };
    static EnvelopeShape envelopeShape(const Control& c, const ControlState& s);

  private:
    struct Drag {
        int control = -1;
        float anchorX = 0.0f, anchorY = 0.0f;
        double anchorNormal = 0.0, anchorNormal2 = 0.0;
        double normal = 0.0, normal2 = 0.0;
        bool fine = false;
        bool relative = false;  // an XY pad keeps its offset after changing speed
        int handle = -1;  // an envelope's handle; a keyboard's key
    };
    // One host gesture: a control's parameters (two at most), or all a
    // product's drawing changes in it. Each entry is a (control, slot).
    struct Gesture {
        struct Entry {
            int control = -1;
            int slot = 0;
            bool open = false;  // its begin was sent
        };
        std::vector<Entry> entries;
        bool timed = false;
        bool drawing = false;  // dragEdit's or wheelEdit's: they add to it
        double idle = 0.0;
        bool active() const {
            return std::any_of(entries.begin(), entries.end(), [](const Entry& e) { return e.control >= 0; });
        }
        bool involves(int c) const {
            return std::any_of(entries.begin(), entries.end(), [c](const Entry& e) { return e.control == c; });
        }
    };

    const ParamDef* def(int i, int slot = 0) const;
    uint32_t paramOf(int i, int slot) const;
    double currentValue(int i, int slot = 0) const;
    int controlAt(float x, float y) const;
    int segmentAt(int i, float x) const;
    int partAt(int i, float x, float y) const;  // what hoverIndex reports
    bool textEditable(int i) const;
    bool parameterless(int i) const;
    int slots(int i) const;  // parameters it edits

    void beginGesture(int c0, int s0, int c1, int s1, bool timed);
    void endGesture();
    void send(int entry, double host);
    void edit(int i, int slot, double host, bool timed);
    void drawingEdit(int i, int slot, double host, bool timed);
    void oneShot(int i, double host) { oneShot(i, 0, host); }
    void oneShot(int i, int slot, double host);
    void resetAll(int i);
    void timedEdit(int i, double host) { edit(i, 0, host, true); }
    void step(int i, double normalDelta, int steps, int slot = 0);

    void closeMenu();
    void chooseMenu(int row);
    bool selectableRow(int row) const;
    void moveMenuHover(int direction);
    void revealMenuHover();
    int menuRowAt(float x, float y) const;
    void openPresetMenu(int control);
    void presetAction(int control, int part);
    int presetPartAt(int control, float x) const;
    void syncPresets(bool force);
    void commitText();
    void cancelText();
    void cancelInactiveOverlays();
    bool textKey(uint32_t code, uint32_t mods);
    void insertText(std::string_view bytes);
    void eraseSelection();
    void pasteText(std::string_view text);
    float measureText(int bytes) const;
    int caretAt(float x) const;
    void revealCaret();
    void textChanged();
    bool menuKey(uint32_t code);
    void moveFocus(int direction);
    void hideTooltip();
    void openTooltip();

    void xyDrag(int i, float x, float y);
    int envelopeHandleAt(int i, float x, float y) const;
    void envelopeDrag(int i, float x, float y);
    void listChoose(int i, int row);
    void listReveal(int i);
    void listStep(int i, int direction);
    void play(int key, float y);
    void release();

    // A file() property by ID, or null.
    const PropertyDef* fileProperty(uint32_t id) const;

    EditorHost& host_;
    float designW_, designH_;
    std::deque<Control> controls_;  // stable references across add()
    std::vector<std::unique_ptr<ListSource>> ownSources_;  // lists over a parameter or the presets
    Drag drag_;
    Gesture gesture_;
    Menu menu_;
    TextEdit text_;
    Tooltip tooltip_;
    double tooltipWait_ = 0.0;
    bool tooltipArmed_ = false;
    double wheelAccumulator_ = 0.0;
    int wheelControl_ = -1;
    int focus_ = -1;
    int hover_ = -1;
    int hoverPart_ = -1;
    float pointerX_ = -1.0f, pointerY_ = -1.0f;
    bool dirty_ = true;
    int building_ = -1;     // the tab bar onPage() applies to
    int buildingPage_ = 0;
    int playing_ = -1;      // the key the keyboard is sounding
    uint64_t keysHeld_[2] = {};
    std::vector<uint8_t> fileStates_;  // each control's last FileStatus::State, to repaint when one moves
    Clipboard* clipboard_ = nullptr;
    FileDialogs* files_ = nullptr;
    TextWidth textWidth_;
    unsigned textSerial_ = 0;  // which text field a paste is for
    // What the preset bars show, polled from the library.
    std::string presetName_;
    bool presetModified_ = false;
    int presetSlot_ = 0;
    uint64_t presetRevision_ = 0;
    double presetPoll_ = 0.0;
};

}  // namespace supple::gui
