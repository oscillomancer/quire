// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The editor's interaction contract, checked against the product's real
// editor and against controls bound to the product's own parameters:
// gestures always balance and never nest, edits land where they should,
// repaint follows changes from anywhere, and the layout is sane.
#include <algorithm>
#include <cmath>
#include <complex>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <string>
#include <thread>

#include "Check.h"
#include "HeadlessCanvas.h"
#include "core/Assets.h"
#include "core/Engine.h"
#include "core/Fft.h"
#include "core/ParamTable.h"
#include "core/Preset.h"
#include "core/Files.h"
#include "core/PresetLibrary.h"
#include "core/PropertyTable.h"
#include "gui/EditorBase.h"
#include "gui/Fonts.h"
#include "gui/Gui.h"
#include "gui/Images.h"
#include "gui/Layout.h"
#include "gui/NativeFiles.h"
#include "gui/Offscreen.h"
#include "gui/Plot.h"
#include "gui/Spectrum.h"
#include "gui/Widgets.h"
#include "gui/Window.h"
#include "ProductInfo.h"
#include "gui/Ui.h"

using namespace supple;
using namespace supple::gui;

namespace {

// A host that records the gesture protocol and objects to every breach. It
// keeps properties, and a preset library over a folder of its own.
struct MockHost : EditorHost, StateAccess {
    double values[params::kCount];
    double mods[params::kCount]{};
    bool open[params::kCount]{};
    int openCount = 0, maxOpen = 0, begins = 0, ends = 0, sets = 0, propertySets = 0;
    bool nested = false, strayEnd = false, valueOutsideGesture = false;
    TelemetryRing ring;
    PropertyStore props;
    std::filesystem::path folder;
    std::unique_ptr<PresetLibrary> presets;

    MockHost() {
        for (size_t i = 0; i < params::kCount; ++i) values[i] = hostDefault(params::at(i));
        folder = std::filesystem::temp_directory_path() / ("supple-ui-presets-" + std::to_string(std::random_device{}()));
        presets = std::make_unique<PresetLibrary>(*this, folder);
    }
    ~MockHost() override {
        presets.reset();
        std::error_code ec;
        std::filesystem::remove_all(folder, ec);
    }
    std::string_view property(uint32_t id) const override {
        const int i = properties::indexOf(id);
        return i >= 0 ? std::string_view(props.get(static_cast<size_t>(i))) : std::string_view();
    }
    bool setProperty(uint32_t id, std::string_view value) override {
        const int i = properties::indexOf(id);
        ++propertySets;
        return i >= 0 && props.set(static_cast<size_t>(i), value);
    }
    uint64_t propertiesVersion() const override { return props.version(); }
    FileStatus files;  // what every file property's file says, as a test sets it
    FileStatus fileStatus(uint32_t) const override { return files; }
    PresetLibrary* presetLibrary() override { return presets.get(); }
    std::vector<uint8_t> saveState(state::Context context) override {
        state::Snapshot s;
        for (size_t i = 0; i < params::kCount; ++i) s.host[i] = values[i];
        for (size_t i = 0; i < properties::kCount; ++i) {
            s.property[i] = props.get(i);
            s.hasProperty[i] = true;
        }
        return state::save(state::fourcc(product::info::kStateTag), s, context);
    }
    bool loadState(const uint8_t* data, size_t size, state::Context) override {
        state::Snapshot s;
        if (state::load(data, size, state::fourcc(product::info::kStateTag), s) != state::LoadResult::Ok) return false;
        for (size_t i = 0; i < params::kCount; ++i)
            if (!params::at(i).isOutput()) values[i] = s.host[i];
        for (size_t i = 0; i < properties::kCount; ++i)
            if (!properties::at(i).perProjectOnly())
                props.set(i, s.hasProperty[i] ? std::string_view(s.property[i]) : std::string_view(properties::at(i).def));
        return true;
    }
    static size_t slot(uint32_t id) {
        const int i = params::indexOf(id);
        return i >= 0 ? static_cast<size_t>(i) : 0;
    }
    double value(uint32_t id) const override { return values[slot(id)]; }
    double modulation(uint32_t id) const override { return mods[slot(id)]; }
    void beginEdit(uint32_t id) override {
        ++begins;
        if (open[slot(id)]) nested = true;
        open[slot(id)] = true;
        maxOpen = std::max(maxOpen, ++openCount);
    }
    void setValue(uint32_t id, double v) override {
        ++sets;
        if (!open[slot(id)]) valueOutsideGesture = true;
        values[slot(id)] = v;
    }
    void endEdit(uint32_t id) override {
        ++ends;
        if (!open[slot(id)]) strayEnd = true;
        open[slot(id)] = false;
        --openCount;
    }
    // A host's context menu, when a test gives it one.
    bool hasHostMenu = false, canPopup = false;
    HostMenu hostItems;
    int popups = 0;
    std::vector<std::pair<uint32_t, uint32_t>> performed;  // parameter, action
    bool hostPopup(uint32_t, float, float) override {
        if (canPopup) ++popups;
        return canPopup;
    }
    bool hostMenu(uint32_t, HostMenu& menu) override {
        if (hasHostMenu) menu = hostItems;
        return hasHostMenu;
    }
    bool performHostMenu(uint32_t param, uint32_t action) override {
        performed.push_back({param, action});
        return true;
    }

    // Notes the keyboard plays: every one it starts must end.
    std::multiset<int> held;
    int noteOns = 0, noteOffs = 0;
    bool strayNoteOff = false;
    double lastVelocity = 0.0;
    void playNote(int key, double velocity, bool on) override {
        if (on) {
            held.insert(key);
            ++noteOns;
            lastVelocity = velocity;
        } else if (const auto it = held.find(key); it != held.end()) {
            held.erase(it);
            ++noteOffs;
        } else {
            strayNoteOff = true;
        }
    }

    bool popTelemetry(TelemetryFrame& f) override { return ring.pop(f); }
    int latencySamples() const override { return 0; }
    double rate = 48000.0;
    double sampleRate() const override { return rate; }

    // One gesture at a time; two parameters in it at most (an XY pad).
    bool clean() const { return !nested && !strayEnd && !valueOutsideGesture && maxOpen <= 2 && !strayNoteOff; }
};

int findParam(bool (*want)(const ParamDef&)) {
    for (size_t i = 0; i < params::kCount; ++i)
        if (want(params::at(i)) && !params::at(i).is(pflag::kHidden) && !params::at(i).isOutput())
            return static_cast<int>(i);
    return -1;
}
bool isContinuous(const ParamDef& p) { return p.kind == ParamKind::Continuous; }
bool isChoice(const ParamDef& p) { return p.kind == ParamKind::Choice || p.kind == ParamKind::Integer; }
bool isToggle(const ParamDef& p) { return p.kind == ParamKind::Toggle; }

// A Ui with one control of each kind the product's table can support.
struct Bench {
    MockHost host;
    Ui ui{host, 800.0f, 600.0f};
    int knob = -1, slider = -1, toggle = -1, segmented = -1, dropdown = -1;
    uint32_t knobParam = 0, choiceParam = 0;

    Bench() {
        if (const int c = findParam(isContinuous); c >= 0) {
            knobParam = params::at(static_cast<size_t>(c)).id;
            ui.knob(knobParam, {20, 20, 100, 120});
            knob = 0;
            ui.hslider(knobParam, {150, 40, 200, 30});
            slider = 1;
        }
        if (const int c = findParam(isToggle); c >= 0) {
            ui.toggle(params::at(static_cast<size_t>(c)).id, {400, 40, 100, 30});
            toggle = ui.count() - 1;
        }
        if (const int c = findParam(isChoice); c >= 0) {
            choiceParam = params::at(static_cast<size_t>(c)).id;
            ui.segmented(choiceParam, {20, 300, 240, 28});
            segmented = ui.count() - 1;
            ui.dropdown(choiceParam, {300, 300, 160, 28});
            dropdown = ui.count() - 1;
        }
    }
    const Control& c(int i) const { return ui.control(i); }
    double normal(int i) const { return ui.state(i).normalized; }
};

}  // namespace

// --- dragging ------------------------------------------------------------------------

TEST(a_press_without_movement_is_not_a_gesture) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerUp(r.cx(), r.cy(), 0, 0);
    CHECK(b.host.begins == 0 && b.host.sets == 0);
}

TEST(shift_mid_drag_reanchors_instead_of_jumping) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.host.values[params::indexOf(b.knobParam)] = params::find(b.knobParam)->hostMin();
    const double start = b.normal(b.knob);
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerMove(r.cx(), r.cy() - 40, 0);  // coarse, 40 px up
    const double coarse = b.normal(b.knob);
    b.ui.pointerMove(r.cx(), r.cy() - 41, mod::kShift);  // one more pixel, now fine
    const double fine = b.normal(b.knob);
    b.ui.pointerUp(r.cx(), r.cy() - 41, 0, mod::kShift);
    CHECK(coarse > start + 0.1);
    CHECK_MSG(std::fabs(fine - coarse) < 0.002, "a 1 px shift move changed the value by %g", fine - coarse);
    CHECK(b.host.begins == 1 && b.host.ends == 1 && b.host.clean());
}

TEST(dragging_past_a_limit_reverses_at_once) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerMove(r.cx(), -600, 0);  // far beyond the top
    CHECK(b.normal(b.knob) == 1.0);
    b.ui.pointerMove(r.cx(), -590, 0);  // ten pixels back down
    CHECK(b.normal(b.knob) < 1.0);
    b.ui.pointerUp(r.cx(), -590, 0, 0);
}

TEST(a_drag_survives_the_pointer_leaving_the_window) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerMove(r.cx(), r.cy() - 20, 0);
    b.ui.pointerLeave();
    b.ui.pointerMove(r.cx(), -50, 0);
    CHECK(b.host.openCount == 1);
    b.ui.pointerUp(r.cx(), -50, 0, 0);
    CHECK(b.host.openCount == 0 && b.host.clean());
}

TEST(double_click_resets_to_default) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.host.values[params::indexOf(b.knobParam)] = params::find(b.knobParam)->hostMax();
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerUp(r.cx(), r.cy(), 0, 0);
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, true);
    b.ui.pointerUp(r.cx(), r.cy(), 0, 0);
    CHECK(b.host.value(b.knobParam) == hostDefault(*params::find(b.knobParam)));
    CHECK(b.host.clean() && b.host.openCount == 0);
}

// --- wheel and keyboard -----------------------------------------------------------

TEST(a_wheel_burst_is_one_gesture_closed_by_real_time) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    for (int i = 0; i < 12; ++i) {
        b.ui.wheel(r.cx(), r.cy(), 0, 1.0f, 0);
        b.ui.idle(0.05);
    }
    CHECK(b.host.begins == 1 && b.host.openCount == 1);
    b.ui.idle(0.15);  // 0.20 s since the last notch
    CHECK(b.host.openCount == 1);
    b.ui.idle(0.06);
    CHECK(b.host.openCount == 0 && b.host.ends == 1 && b.host.clean());
}

TEST(wheel_then_key_on_the_same_control_stays_balanced) {
    // The sequence that left the old template's host latched mid-gesture.
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.ui.wheel(r.cx(), r.cy(), 0, -1.0f, 0);
    b.ui.setFocus(b.knob);
    b.ui.key(key::kRight, 0);
    b.ui.key(key::kRight, 0);
    b.ui.idle(0.3);
    CHECK(b.host.clean() && b.host.openCount == 0);
}

TEST(wheel_over_another_control_mid_drag_is_ignored) {
    Bench b;
    if (b.knob < 0 || b.slider < 0) return;
    const Rect k = b.c(b.knob).bounds, s = b.c(b.slider).bounds;
    b.ui.pointerDown(k.cx(), k.cy(), 0, 0, false);
    b.ui.pointerMove(k.cx(), k.cy() - 30, 0);
    b.ui.wheel(s.cx(), s.cy(), 0, 1.0f, 0);
    b.ui.pointerUp(k.cx(), k.cy() - 30, 0, 0);
    b.ui.idle(1.0);
    CHECK(b.host.clean() && b.host.openCount == 0 && b.host.begins == 1);
}

TEST(held_arrow_keys_are_one_gesture) {
    Bench b;
    if (b.knob < 0) return;
    b.ui.setFocus(b.knob);
    for (int i = 0; i < 20; ++i) {
        b.ui.key(key::kUp, 0);
        b.ui.idle(0.03);  // key repeat
    }
    b.ui.idle(0.3);
    CHECK(b.host.begins == 1 && b.host.ends == 1 && b.host.clean());
}

TEST(tab_moves_focus_in_order_and_skips_hidden_controls) {
    Bench b;
    if (b.ui.count() < 3) return;
    b.ui.control(1).visible = false;
    b.ui.key(key::kTab, 0);
    CHECK(b.ui.focus() == 0);
    b.ui.key(key::kTab, 0);
    CHECK(b.ui.focus() == 2);
    b.ui.key(key::kTab, mod::kShift);
    CHECK(b.ui.focus() == 0);
    b.ui.key(key::kEscape, 0);
    CHECK(b.ui.focus() == -1);
}

TEST(keys_reach_limits_and_the_default) {
    Bench b;
    if (b.knob < 0) return;
    const ParamDef& p = *params::find(b.knobParam);
    b.ui.setFocus(b.knob);
    b.ui.key(key::kEnd, 0);
    CHECK(b.host.value(b.knobParam) == p.hostMax());
    b.ui.key(key::kHome, 0);
    CHECK(b.host.value(b.knobParam) == p.hostMin());
    b.ui.key(key::kBackspace, 0);
    CHECK(b.host.value(b.knobParam) == hostDefault(p));
    b.ui.idle(1.0);
    CHECK(b.host.clean() && b.host.openCount == 0);
}

TEST(space_is_left_to_the_host) {
    Bench b;
    if (b.toggle < 0) return;
    b.ui.setFocus(b.toggle);
    CHECK(!b.ui.key(key::kSpace, 0));
    CHECK(b.host.sets == 0);
}

// --- text entry ----------------------------------------------------------------------

TEST(command_click_opens_a_text_field_that_parses_the_parameters_units) {
    Bench b;
    if (b.knob < 0) return;
    const ParamDef& p = *params::find(b.knobParam);
    const Rect r = b.c(b.knob).bounds;
    b.ui.pointerDown(r.cx(), r.cy(), 0, mod::kCtrl, false);
    CHECK(b.ui.textEdit().open);
    // Type the text of a value three quarters up the range, in its own units.
    char wanted[64];
    formatValue(p, fromNormalized(p, 0.75), wanted, sizeof(wanted));
    for (const char* s = wanted; *s; ++s) b.ui.text(static_cast<unsigned char>(*s));
    b.ui.key(key::kEnter, 0);
    CHECK(!b.ui.textEdit().open);
    char got[64];
    formatValue(p, b.host.value(b.knobParam), got, sizeof(got));
    CHECK_MSG(std::string(got) == wanted, "typed \"%s\", got \"%s\"", wanted, got);
    CHECK(b.host.clean() && b.host.openCount == 0);
}

TEST(rejected_text_stays_open_and_escape_cancels) {
    Bench b;
    if (b.knob < 0) return;
    b.ui.openTextEdit(b.knob);
    for (const char* s = "nonsense"; *s; ++s) b.ui.text(static_cast<unsigned char>(*s));
    b.ui.key(key::kEnter, 0);
    CHECK(b.ui.textEdit().open && b.ui.textEdit().rejected);
    b.ui.key(key::kEscape, 0);
    CHECK(!b.ui.textEdit().open && b.host.sets == 0);
}

// --- discrete controls ------------------------------------------------------------------

TEST(toggle_segmented_and_dropdown_edit_in_one_shot) {
    Bench b;
    if (b.toggle >= 0) {
        const Rect r = b.c(b.toggle).bounds;
        const double before = b.ui.state(b.toggle).value;
        b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
        b.ui.pointerUp(r.cx(), r.cy(), 0, 0);
        CHECK(b.ui.state(b.toggle).value != before);
    }
    if (b.segmented >= 0) {
        const Rect r = b.c(b.segmented).bounds;
        b.ui.pointerDown(r.right() - 2, r.cy(), 0, 0, false);  // the last segment
        CHECK(b.host.value(b.choiceParam) == params::find(b.choiceParam)->hostMax());
        b.ui.pointerDown(r.x + 2, r.cy(), 0, 0, false);  // the first
        CHECK(b.host.value(b.choiceParam) == params::find(b.choiceParam)->hostMin());
    }
    if (b.dropdown >= 0) {
        const Rect r = b.c(b.dropdown).bounds;
        b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
        CHECK(b.ui.menu().open);
        b.ui.key(key::kDown, 0);
        b.ui.key(key::kEnter, 0);
        CHECK(!b.ui.menu().open);
        CHECK(b.host.value(b.choiceParam) == params::find(b.choiceParam)->hostMin() + 1);
        b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
        b.ui.pointerDown(790, 590, 0, 0, false);  // outside: closes, changes nothing
        CHECK(!b.ui.menu().open);
        CHECK(b.host.value(b.choiceParam) == params::find(b.choiceParam)->hostMin() + 1);
    }
    CHECK(b.host.clean() && b.host.openCount == 0);
}

TEST(losing_focus_ends_every_gesture_and_overlay) {
    Bench b;
    if (b.knob < 0) return;
    const Rect r = b.c(b.knob).bounds;
    b.ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    b.ui.pointerMove(r.cx(), r.cy() - 25, 0);
    CHECK(b.host.openCount == 1);
    b.ui.focusLost();
    CHECK(b.host.openCount == 0 && b.host.clean());
}

TEST(a_control_on_an_output_only_displays_it) {
    int output = -1;
    for (size_t i = 0; i < params::kCount && output < 0; ++i)
        if (params::at(i).isOutput()) output = static_cast<int>(i);
    if (output < 0) {
        std::printf("      (no output parameters in this product)\n");
        return;
    }
    MockHost host;
    Ui ui{host, 800.0f, 600.0f};
    const uint32_t id = params::at(static_cast<size_t>(output)).id;
    const Rect knobRect{20, 20, 100, 120}, readoutRect{150, 40, 200, 40};
    ui.knob(id, knobRect);
    ui.readout(id, readoutRect);
    const int normal = findParam(isContinuous);
    if (normal >= 0) ui.knob(params::at(static_cast<size_t>(normal)).id, {400, 20, 100, 120});
    for (const Rect& r : {knobRect, readoutRect}) {
        CHECK(!ui.pointerMove(r.cx(), r.cy(), 0) || ui.hover() < 0);
        CHECK(ui.cursor() == Cursor::Arrow);
        ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
        ui.pointerMove(r.cx(), r.cy() - 40, 0);
        ui.pointerUp(r.cx(), r.cy() - 40, 0, 0);
        ui.pointerDown(r.cx(), r.cy(), 0, 0, true);
        ui.pointerDown(r.cx(), r.cy(), 0, mod::kCtrl, false);
        ui.wheel(r.cx(), r.cy(), 0.0f, 3.0f, 0);
        ui.idle(1.0);
    }
    for (int k = 0; k < 4; ++k) {
        ui.key(key::kTab, 0);
        CHECK(ui.focus() < 0 || ui.focus() == 2);  // focus passes over both
        ui.key(key::kHome, 0);
    }
    CHECK(!ui.textEdit().open && !ui.menu().open);
    CHECK(host.values[static_cast<size_t>(output)] == hostDefault(params::at(static_cast<size_t>(output))));
    CHECK(host.clean());
}

// --- text fields, menus, prompts, the preset bar ---------------------------------------------

TEST(a_text_field_edits_its_property_as_utf8) {
    if constexpr (properties::kCount == 0) {
        std::puts("      (no properties in this product)");
        return;
    }
    MockHost host;
    Ui ui{host, 800.0f, 600.0f};
    const PropertyDef& d = properties::at(0);
    const Control& field = ui.textField(d.id, {20, 20, 300, 26});
    CHECK(ui.interactive(0) && ui.state(0).text == d.def);
    ui.pointerDown(field.bounds.cx(), field.bounds.cy(), 0, 0, false);
    CHECK(ui.textEdit().open && ui.textEdit().from() == 0 && ui.textEdit().to() == ui.textEdit().length &&
          !ui.textEdit().ascii);
    ui.text('a');
    ui.text(0xE9);     // é: two bytes
    ui.text(0x1F600);  // four
    CHECK(ui.textEdit().length == 7);
    ui.key(key::kBackspace, 0);  // a whole character, not a byte
    CHECK(ui.textEdit().length == 3);
    ui.key(key::kLeft, 0);
    CHECK(ui.textEdit().caret == 1);
    ui.key(key::kEnter, 0);
    CHECK(!ui.textEdit().open && host.property(d.id) == "a\xC3\xA9");
    CHECK(ui.state(0).text == "a\xC3\xA9");

    // Typing stops at the property's limit.
    ui.key(key::kEnter, 0);  // focused: Enter opens it again
    CHECK(ui.textEdit().open);
    for (uint32_t k = 0; k < d.limit + 10 && k < 300; ++k) ui.text('z');
    CHECK(ui.textEdit().length == static_cast<int>(std::min<uint32_t>(d.limit, Ui::TextEdit::kCapacity - 1)));
    ui.key(key::kEscape, 0);  // abandoned: nothing changes
    CHECK(!ui.textEdit().open && host.property(d.id) == "a\xC3\xA9");
    // A click elsewhere abandons it too.
    ui.pointerDown(field.bounds.cx(), field.bounds.cy(), 0, 0, false);
    ui.text('q');
    ui.pointerDown(700, 500, 0, 0, false);
    CHECK(!ui.textEdit().open && host.property(d.id) == "a\xC3\xA9");
    CHECK(host.clean() && host.begins == 0);  // no parameter gesture anywhere
}

TEST(unavailable_controls_discard_their_owned_overlays) {
    Bench b;
    if (b.knob < 0) return;
    for (bool pageChange : {false, true}) {
        for (bool property : {false, true}) {
            if (property && properties::kCount == 0) continue;
            MockHost host;
            Ui ui(host, 800, 600);
            ui.tabs({20, 20, 300, 30}, {"One", "Two"});
            ui.onPage(0);
            bool allowed = true;
            if (property) ui.textField(properties::at(0).id, {20, 80, 300, 26});
            else ui.knob(b.knobParam, {20, 80, 100, 120});
            ui.control(1).enabledWhen([&] { return allowed; });
            const double before = host.value(b.knobParam);
            const std::string textBefore = property ? std::string(host.property(properties::at(0).id)) : "";
            ui.openTextEdit(1);
            CHECK(ui.textEdit().open);
            ui.text('7');
            if (pageChange) ui.setPage(0, 1);
            else { allowed = false; ui.applyRules(); }
            CHECK(!ui.textEdit().open);
            ui.key(key::kEnter, 0);
            CHECK(host.value(b.knobParam) == before && host.propertySets == 0);
            if (property) CHECK(host.property(properties::at(0).id) == textBefore);
            allowed = true;
            ui.setPage(0, 0);
            ui.applyRules();
            ui.popup({20, 80, 100, 20}, {MenuItem::item("Action")}, [](int) {}, 160, 1);
            CHECK(ui.menu().open);
            if (pageChange) ui.setPage(0, 1);
            else { allowed = false; ui.applyRules(); }
            CHECK(!ui.menu().open && host.clean());
        }
    }
}

TEST(a_popup_skips_what_cannot_be_chosen_and_scrolls) {
    MockHost host;
    Ui ui{host, 400.0f, 300.0f};
    std::vector<MenuItem> items;
    for (int k = 0; k < 40; ++k) {
        if (k % 10 == 0) items.push_back(MenuItem::header("Group " + std::to_string(k / 10)));
        else if (k % 10 == 9) items.push_back(MenuItem::separator());
        else items.push_back(MenuItem::item("Item " + std::to_string(k), k == 15, k != 2));
    }
    int chosen = -1;
    ui.popup({10, 10, 100, 20}, items, [&](int row) { chosen = row; });
    const Ui::Menu& m = ui.menu();
    CHECK(m.open && m.visible < m.count() && m.hover == 15);
    CHECK(m.bounds.y >= 4.0f && m.bounds.bottom() <= 296.0f);
    ui.key(key::kHome, 0);
    CHECK(m.hover == 1);
    ui.key(key::kDown, 0);  // 2 is disabled
    CHECK(m.hover == 3);
    ui.key(key::kEnd, 0);
    CHECK(m.hover == 38 && m.first + m.visible > 38 && m.scrollsUp());
    ui.wheel(m.bounds.cx(), m.bounds.cy(), 0, 1.0f, 0);  // scroll back up a row
    CHECK(m.first + m.visible == 38);
    // A click on a header does nothing; on an item, chooses it.
    ui.key(key::kHome, 0);  // shows rows 1 to 11; 10 is a header
    const Rect header = m.row(10);
    ui.pointerDown(header.cx(), header.cy(), 0, 0, false);
    CHECK(m.open && chosen < 0);
    const Rect row = m.row(4);
    ui.pointerMove(row.cx(), row.cy(), 0);
    ui.pointerDown(row.cx(), row.cy(), 0, 0, false);
    CHECK(!m.open && chosen == 4);
    // Dismissed: nothing chosen.
    chosen = -1;
    ui.popup({10, 10, 100, 20}, items, [&](int row) { chosen = row; });
    ui.key(key::kEscape, 0);
    CHECK(!ui.menu().open && chosen < 0);
}

TEST(a_right_click_opens_the_context_menu_with_the_hosts_items) {
    Bench b;
    if (b.knob < 0) {
        std::puts("      (no continuous parameter in this product)");
        return;
    }
    const Rect r = b.c(b.knob).bounds;
    const Ui::Menu& m = b.ui.menu();
    auto rightClick = [&] { CHECK(b.ui.pointerDown(r.cx(), r.cy(), 1, 0, false)); };
    auto choose = [&](const char* label) {
        for (int row = 0; row < m.count(); ++row)
            if (m.items[static_cast<size_t>(row)].label == label) {
                const Rect at = m.row(row);
                b.ui.pointerDown(at.cx(), at.cy(), 0, 0, false);
                return;
            }
        CHECK_MSG(false, "no '%s' in the menu", label);
    };

    // Without a host menu: reset (nothing to do at the default) and enter.
    rightClick();
    CHECK(m.open && m.count() == 2 && !m.items[0].enabled && m.items[1].selectable());
    CHECK(b.host.begins == 0);
    b.ui.key(key::kEscape, 0);

    // The host's items follow a separator, flattened: a submenu's label is a
    // header over its items, and no separator doubles up or ends the menu.
    b.host.hasHostMenu = true;
    b.host.hostItems.items = {MenuItem::item("Automate"),      MenuItem::header("MIDI"),
                              MenuItem::item("Learn", true),   MenuItem::separator(),
                              MenuItem::separator(),           MenuItem::item("Forget", false, false),
                              MenuItem::separator()};
    b.host.hostItems.actions = {10, 0, 11, 0, 0, 12, 0};
    b.host.values[params::indexOf(b.knobParam)] = params::find(b.knobParam)->hostMax();
    rightClick();
    CHECK(m.open && m.count() == 8 && m.items[0].selectable() && m.items[2].kind == MenuItem::Kind::Separator);
    CHECK(m.items[4].kind == MenuItem::Kind::Header && m.items[5].checked && !m.items[7].selectable());
    CHECK(m.hover < 0);  // the checked item is not preselected
    choose("Learn");
    CHECK(!m.open && b.host.performed.size() == 1 && b.host.performed[0].first == b.knobParam &&
          b.host.performed[0].second == 11);

    // Reset is one whole gesture; Enter Value opens the text field.
    rightClick();
    CHECK(m.items[0].label.rfind("Reset to ", 0) == 0);
    choose(m.items[0].label.c_str());
    CHECK(b.host.value(b.knobParam) == hostDefault(*params::find(b.knobParam)));
    CHECK(b.host.begins == 1 && b.host.ends == 1 && b.host.clean());
    rightClick();
    choose("Enter Value\xE2\x80\xA6");
    CHECK(b.ui.textEdit().open && b.ui.textEdit().control == b.knob);
    b.ui.key(key::kEscape, 0);

    // A host that shows its own menu shows it instead.
    b.host.canPopup = true;
    rightClick();
    CHECK(b.host.popups == 1 && !m.open);
}

TEST(a_prompt_can_refuse_or_hand_over_to_a_confirmation) {
    MockHost host;
    Ui ui{host, 400.0f, 300.0f};
    int asked = 0;
    ui.prompt({10, 10, 200, 26}, "name", 16, [&](std::string_view text) {
        ++asked;
        return text != "name";  // the same name is refused
    });
    ui.key(key::kEnter, 0);
    CHECK(asked == 1 && ui.textEdit().open && ui.textEdit().rejected);
    ui.key(key::kEnd, 0);
    ui.text('2');
    ui.key(key::kEnter, 0);
    CHECK(asked == 2 && !ui.textEdit().open);
    bool confirmed = false;
    ui.prompt({10, 10, 200, 26}, "x", 16, [&](std::string_view) {
        ui.popup({10, 10, 200, 26}, {MenuItem::header("Sure?"), MenuItem::item("Yes")}, [&](int) { confirmed = true; });
        return true;
    });
    ui.key(key::kEnter, 0);
    CHECK(!ui.textEdit().open && ui.menu().open && ui.menu().hover < 0);
    ui.key(key::kDown, 0);
    ui.key(key::kEnter, 0);
    CHECK(confirmed && !ui.menu().open);
}

namespace {
int menuRow(const Ui& ui, const char* label) {
    for (int r = 0; r < ui.menu().count(); ++r)
        if (ui.menu().items[static_cast<size_t>(r)].label == label) return r;
    return -1;
}
void choose(Ui& ui, int row) {
    while (ui.menu().hover != row && ui.menu().open) {
        const int before = ui.menu().hover;
        ui.key(row > ui.menu().hover ? key::kDown : key::kUp, 0);
        if (ui.menu().hover == before) break;
    }
    ui.key(key::kEnter, 0);
}
}  // namespace

namespace {
struct FakeDialogs : FileDialogs {
    Options last;
    Done done;
    int opened = 0;
    bool open(const Options& o, Done d) override {
        last = o;
        done = std::move(d);
        ++opened;
        return true;
    }
    bool save(const Options& o, Done d) override { return open(o, std::move(d)); }
};
}  // namespace

TEST(a_file_slot_chooses_takes_drops_clears_and_shows_its_state) {
    int k = -1;
    for (size_t i = 0; i < properties::kCount && k < 0; ++i)
        if (properties::at(i).isFile()) k = static_cast<int>(i);
    if (k < 0) {
        std::printf("      (no file properties in this product)\n");
        return;
    }
    const PropertyDef& p = properties::at(static_cast<size_t>(k));
    MockHost host;
    Ui ui{host, 400.0f, 200.0f};
    FakeDialogs dialogs;
    ui.setFileDialogs(&dialogs);
    ui.fileSlot(p.id, {20, 20, 300, 30});
    CHECK(ui.find(p.id) < 0);  // a property, not a parameter
    // A click opens the dialog, offering what the property takes.
    CHECK(ui.pointerDown(100, 35, 0, 0, false));
    ui.pointerUp(100, 35, 0, 0);
    CHECK(dialogs.opened == 1);
    CHECK(!ui.key(key::kSpace, 0));
    CHECK(dialogs.opened == 1);  // Space belongs to the host transport
    CHECK(dialogs.last.title.find(p.name) != std::string::npos);
    std::string offered;
    for (const std::string& e : dialogs.last.extensions) offered += (offered.empty() ? "" : " ") + e.substr(1);
    CHECK(offered == p.extensions);
    dialogs.done({"/some/where/ir.wav"});
    CHECK(host.property(p.id) == "/some/where/ir.wav");
    dialogs.done({});  // cancelled: nothing changes
    CHECK(host.property(p.id) == "/some/where/ir.wav");
    ui.pointerMove(100, 35, 0);
    ui.idle(Ui::kTooltipSeconds + 0.01);
    CHECK(ui.tooltip().open && ui.tooltip().text == "/some/where/ir.wav");
    ui.pointerLeave();
    CHECK(!ui.tooltip().open);
    // A drop takes the first file it accepts, over the slot only.
    const bool picky = *p.extensions != 0;
    if (picky) CHECK(ui.fileSlotAt(100, 35, {"/x/notes.txt.unknown"}) < 0);
    CHECK(ui.fileSlotAt(100, 35, {}) == 0);  // still being dragged
    CHECK(ui.fileSlotAt(5, 5, {"/x/b.wav"}) < 0);
    CHECK(ui.dropFiles(100, 35, {"/x/notes.txt.unknown", "/x/b.wav"}) == (!picky || files::hasExtension("b.wav", p.extensions)));
    // Delete clears it; so does the menu.
    ui.key(key::kDelete, 0);
    CHECK(host.property(p.id).empty());
    host.setProperty(p.id, "/x/b.wav");
    CHECK(ui.pointerDown(100, 35, 1, 0, false));
    CHECK(ui.menu().open && menuRow(ui, "Clear") >= 0);
    choose(ui, menuRow(ui, "Clear"));
    CHECK(host.property(p.id).empty());
    // What it shows follows the host, and a change repaints.
    host.setProperty(p.id, "/gone/cathedral.wav");
    host.files.state = FileStatus::State::Missing;
    ui.idle(0.01);
    ui.takeDirty();
    CHECK(ui.state(0).file == FileStatus::State::Missing && ui.state(0).text == "/gone/cathedral.wav");
    host.files.state = FileStatus::State::Ready;
    host.files.summary = "stereo, 1.20 s";
    ui.idle(0.01);
    CHECK(ui.takeDirty());
    CHECK(ui.state(0).file == FileStatus::State::Ready && ui.state(0).detail == "stereo, 1.20 s");
    // It draws in every state.
    NVGcontext* vg = test::createHeadlessCanvas();
    for (auto state : {FileStatus::State::Empty, FileStatus::State::Loading, FileStatus::State::Ready,
                       FileStatus::State::Missing, FileStatus::State::Failed}) {
        host.files.state = state;
        nvgBeginFrame(vg, 400, 200, 1.0f);
        widgets::control(vg, Theme{}, ui.control(0), ui.state(0));
        nvgEndFrame(vg);
    }
    nvgDeleteInternal(vg);
}

TEST(a_preset_list_follows_every_change_to_the_library) {
    MockHost host;
    PresetLibrary& lib = *host.presets;
    Ui ui(host, 800.0f, 600.0f);
    ui.presetList({20, 20, 200, 300});
    const ListSource& rows = *ui.control(0).source;
    auto shows = [&](const char* name) {
        for (int r = 0; r < rows.size(); ++r)
            if (rows.item(r) == name && !rows.header(r)) return r;
        return -1;
    };
    // Rows are kept between changes, and built again at each.
    uint64_t revision = lib.revision();
    CHECK(lib.save("Kept", false) == PresetLibrary::SaveResult::Ok);
    CHECK(lib.revision() != revision);
    CHECK(shows("Kept") >= 0 && rows.selected() == shows("Kept") && rows.header(shows("Kept") - 1));
    revision = lib.revision();
    CHECK(lib.rename(lib.current(), "Renamed"));
    CHECK(lib.revision() != revision && shows("Kept") < 0 && shows("Renamed") >= 0);
    CHECK(lib.remove(lib.current()));
    bool userHeader = false;
    for (int r = 0; r < rows.size(); ++r) userHeader = userHeader || (rows.header(r) && rows.item(r) == "User");
    CHECK(shows("Renamed") < 0 && !userHeader);
}

TEST(the_preset_bar_steps_compares_saves_renames_and_deletes) {
    MockHost host;
    Ui ui{host, 800.0f, 600.0f};
    PresetLibrary& lib = *host.presets;
    const Control& bar = ui.presetBar({20, 20, 320, 28});
    auto click = [&](int part) {
        const Rect r = Ui::presetPart(bar.bounds, part);
        ui.pointerDown(r.cx(), r.cy(), 0, 0, false);
    };
    CHECK(ui.interactive(0) && ui.state(0).text == "Default" && !ui.state(0).modified);
    for (int part : {presetbar::kPrevious, presetbar::kName, presetbar::kNext, presetbar::kCompare})
        CHECK(ui.presetPart(bar.bounds, part).w > 0.0f);

    click(presetbar::kCompare);
    CHECK(lib.slot() == 1 && ui.state(0).slot == 1);
    click(presetbar::kCompare);
    CHECK(lib.slot() == 0);
    if (presets::kFactoryCount > 0) {
        click(presetbar::kNext);
        CHECK(lib.current() == 0 && ui.state(0).text == presets::factory(0).name);
        click(presetbar::kPrevious);
        CHECK(lib.current() == static_cast<int>(lib.entries().size()) - 1);
    }

    // Change something, then Save As from the menu.
    size_t i = 0;
    while (i + 1 < params::kCount && (params::at(i).isOutput() || params::at(i).is(pflag::kBypass))) ++i;
    host.values[i] = params::at(i).hostMax() == host.values[i] ? params::at(i).hostMin() : params::at(i).hostMax();
    ui.idle(0.3);
    CHECK(ui.state(0).modified);
    click(presetbar::kName);
    CHECK(ui.menu().open && menuRow(ui, "Save") >= 0 && !ui.menu().items[static_cast<size_t>(menuRow(ui, "Save"))].enabled);
    choose(ui, menuRow(ui, "Save As…"));
    CHECK(ui.textEdit().open);
    for (char c : std::string("Ui Test")) ui.text(static_cast<uint32_t>(c));
    ui.key(key::kEnter, 0);
    CHECK(!ui.textEdit().open && lib.currentName() == "Ui Test" && ui.state(0).text == "Ui Test");
    CHECK(std::filesystem::exists(host.folder / "Ui Test.preset") && !ui.state(0).modified);

    // Save As the same name asks before replacing.
    click(presetbar::kName);
    choose(ui, menuRow(ui, "Save As…"));
    ui.key(key::kEnter, 0);  // "Ui Test" again
    CHECK(ui.menu().open && menuRow(ui, "Replace") >= 0);
    choose(ui, menuRow(ui, "Replace"));
    CHECK(!ui.menu().open);

    // Rename, then delete after confirming.
    click(presetbar::kName);
    choose(ui, menuRow(ui, "Rename…"));
    ui.key(key::kBackspace, 0);  // everything was selected
    for (char c : std::string("Renamed")) ui.text(static_cast<uint32_t>(c));
    ui.key(key::kEnter, 0);
    CHECK(lib.currentName() == "Renamed" && std::filesystem::exists(host.folder / "Renamed.preset"));
    click(presetbar::kName);
    choose(ui, menuRow(ui, "Delete…"));
    CHECK(ui.menu().open && menuRow(ui, "Delete") >= 0);
    choose(ui, menuRow(ui, "Delete"));
    CHECK(!std::filesystem::exists(host.folder / "Renamed.preset") && lib.current() < 0);

    // Without a clipboard, copy and paste are there but off.
    click(presetbar::kName);
    CHECK(!ui.menu().items[static_cast<size_t>(menuRow(ui, "Copy Settings"))].enabled);
    choose(ui, menuRow(ui, "Default Settings"));
    CHECK(lib.currentName() == "Default" && host.values[i] == hostDefault(params::at(i)));
    CHECK(host.clean() && host.begins == 0);
}

namespace {
struct FakeClipboard : Clipboard {
    std::string text;
    bool copy(std::string_view t) override {
        text = t;
        return true;
    }
    bool paste(std::function<void(std::string_view)> done) override {
        done(text);
        return true;
    }
};
}  // namespace

TEST(text_fields_select_copy_cut_paste_and_place_the_caret_where_clicked) {
    MockHost host;
    Ui ui{host, 800.0f, 600.0f};
    FakeClipboard clipboard;
    ui.setClipboard(&clipboard);
    std::string committed;
    const Rect box{20, 20, 200, 26};
    ui.prompt(box, "one two", 64, [&](std::string_view t) { committed = t; return true; });
    const Ui::TextEdit& e = ui.textEdit();
    CHECK(e.open && e.from() == 0 && e.to() == 7);  // all of it, to be typed over

    // Without fonts each character is 7 units: a click between "one" and
    // " two" puts the caret there; Shift-arrows select from it.
    const float start = Ui::textStart(e, 0.0f);
    ui.pointerDown(start + 3 * 7.0f + 2.0f, box.cy(), 0, 0, false);
    ui.pointerUp(start + 3 * 7.0f + 2.0f, box.cy(), 0, 0);
    CHECK(e.caret == 3 && !e.selected());
    ui.key(key::kRight, mod::kShift);
    ui.key(key::kRight, mod::kShift);
    CHECK(e.from() == 3 && e.to() == 5);
    ui.key(key::kLeft, 0);  // collapses to the selection's start
    CHECK(e.caret == 3 && !e.selected());

    // Dragging selects; Ctrl-C copies the selection, Ctrl-X cuts it.
    ui.pointerDown(start + 1.0f, box.cy(), 0, 0, false);
    ui.pointerMove(start + 3 * 7.0f, box.cy(), 0);
    ui.pointerUp(start + 3 * 7.0f, box.cy(), 0, 0);
    CHECK(e.from() == 0 && e.to() == 3);
    ui.key('c', mod::kCtrl);
    CHECK(clipboard.text == "one" && std::string(e.buffer) == "one two");
    ui.key('x', mod::kSuper);
    CHECK(clipboard.text == "one" && std::string(e.buffer) == " two" && e.caret == 0);

    // Pasting goes in at the caret, without its line breaks.
    clipboard.text = "th\nree";
    ui.key('v', mod::kCtrl);
    CHECK(std::string(e.buffer) == "three two" && e.caret == 5);
    // A double-click selects the word; Ctrl-A all of it; typing replaces it.
    ui.pointerDown(start + 7 * 7.0f, box.cy(), 0, 0, true);
    ui.pointerUp(start + 7 * 7.0f, box.cy(), 0, 0);
    CHECK(e.from() == 6 && e.to() == 9);
    ui.key('a', mod::kCtrl);
    CHECK(e.from() == 0 && e.to() == 9);
    ui.text('x');
    ui.key(key::kEnter, 0);
    CHECK(!e.open && committed == "x");

    // A value takes only what it would from the keyboard; a paste that
    // arrives after its field closed goes nowhere.
    Bench b;
    if (b.knob >= 0) {
        b.ui.setClipboard(&clipboard);
        b.ui.openTextEdit(b.knob);
        clipboard.text = "\xE2\x88\x92" "3\xC3\xA9";  // a typographic minus, 3, é
        b.ui.key('v', mod::kCtrl);
        CHECK(std::string(b.ui.textEdit().buffer) == "-3");
    }
    struct LateClipboard : Clipboard {
        std::function<void(std::string_view)> pending;
        bool copy(std::string_view) override { return true; }
        bool paste(std::function<void(std::string_view)> done) override {
            pending = std::move(done);
            return true;
        }
    } late;
    ui.setClipboard(&late);
    ui.prompt(box, "kept", 64, [](std::string_view) { return true; });
    ui.key('v', mod::kCtrl);
    ui.key(key::kEscape, 0);
    ui.prompt(box, "new", 64, [](std::string_view) { return true; });
    late.pending("stale");
    CHECK(std::string(e.buffer) == "new");
}

TEST(a_click_in_a_scrolled_text_field_lands_on_the_character_under_it) {
    struct PlainEditor : EditorBase {
        using EditorBase::ui;
        explicit PlainEditor(EditorHost& host) : EditorBase(host, Theme{}, 400, 300) {}
        void paint(NVGcontext*) override {}
    };
    MockHost host;
    PlainEditor editor(host);
    NVGcontext* vg = test::createHeadlessCanvas();
    CHECK(editor.attach(vg));
    const Rect box{20, 20, 120, 26};
    std::string text;
    for (int k = 0; k < 40; ++k) text += static_cast<char>('a' + k % 26);
    editor.ui().prompt(box, text, 64, [](std::string_view) { return true; });
    const Ui::TextEdit& e = editor.ui().textEdit();
    editor.key(key::kEnd, 0);
    CHECK(e.scroll > 0.0f);  // the end is in view, the start slid off
    // Where each character boundary is drawn, by the fonts' own measure.
    const float start = Ui::textStart(e, widgets::textEntryWidth(vg, text, false));
    const float atA = start + widgets::textEntryWidth(vg, std::string_view(text).substr(0, 35), false);
    const float atB = start + widgets::textEntryWidth(vg, std::string_view(text).substr(0, 36), false);
    CHECK(atA > box.x && atB < box.right());
    const float scroll = e.scroll;
    editor.pointerDown(atA + 0.3f * (atB - atA), box.cy(), 0, 0, false);
    editor.pointerUp(atA, box.cy(), 0, 0);
    CHECK_MSG(e.caret == 35, "caret %d", e.caret);
    CHECK(e.scroll == scroll);  // nothing slid under the pointer
    editor.detach();
    nvgDeleteInternal(vg);
}

TEST(a_product_font_that_will_not_load_draws_in_inter) {
    NVGcontext* vg = test::createHeadlessCanvas();
    CHECK(loadFonts(vg));
    static const unsigned char broken[] = "not a font at all";
    CHECK(nvgFindFont(vg, "Broken") < 0);
    CHECK(loadFace(vg, "Broken", broken, sizeof(broken)));
    CHECK(nvgFindFont(vg, "Broken") >= 0);
    const float inter = widgets::textWidth(vg, "Level 12 dB", 12.0f, font::kUi);
    CHECK(inter > 0.0f && widgets::textWidth(vg, "Level 12 dB", 12.0f, "Broken") == inter);
    nvgDeleteInternal(vg);
}

TEST(the_serif_faces_draw_in_fraunces_not_inter) {
    NVGcontext* vg = test::createHeadlessCanvas();
    CHECK(loadFonts(vg));
    // Each face sets the same words to its own width: a face that failed to
    // load would fall back and measure exactly as Inter does.
    const char* words = "Equaliser \xE2\x80\x94 Band 3";
    const float bold = widgets::textWidth(vg, words, 14.0f, font::kUiBold);
    const float serif = widgets::textWidth(vg, words, 14.0f, font::kSerif);
    const float italic = widgets::textWidth(vg, words, 14.0f, font::kSerifItalic);
    CHECK(serif > 0.0f && italic > 0.0f);
    CHECK(serif != bold && italic != serif);
    CHECK(italic != widgets::textWidth(vg, words, 14.0f, font::kUi));
    nvgDeleteInternal(vg);
}

TEST(text_too_long_for_its_space_shrinks_then_ends_in_an_ellipsis) {
    NVGcontext* vg = test::createHeadlessCanvas();
    CHECK(loadFonts(vg));
    const char* face = font::kUiBold;
    std::string text = "Gain";
    CHECK(widgets::fitText(vg, text, 11.0f, 9.0f, 200.0f, face) == 11.0f && text == "Gain");
    // A little too wide: smaller, but whole.
    const float full = widgets::textWidth(vg, "Low Cut Frequency", 11.0f, face);
    text = "Low Cut Frequency";
    const float size = widgets::fitText(vg, text, 11.0f, 9.0f, full * 0.9f, face);
    CHECK(size < 11.0f && size >= 9.0f && text == "Low Cut Frequency");
    // Far too wide: the smallest size, cut at a character, and an ellipsis.
    text = "Low Cut Frequency";
    CHECK(widgets::fitText(vg, text, 11.0f, 9.0f, full * 0.5f, face) == 9.0f);
    CHECK(text.size() > 3 && text.compare(text.size() - 3, 3, "\xE2\x80\xA6") == 0);
    CHECK(std::string("Low Cut Frequency").find(text.substr(0, text.size() - 3)) == 0);
    CHECK(widgets::textWidth(vg, text.c_str(), 9.0f, face) <= full * 0.5f + 0.5f);
    // No space even for the ellipsis: nothing.
    text = "Low Cut Frequency";
    widgets::fitText(vg, text, 11.0f, 9.0f, 2.0f, face);
    CHECK(text.empty());
    // A cut never splits a character, nor leaves a space before the ellipsis.
    const std::string accented = "\xC3\xA9t\xC3\xA9 \xC3\xA9t\xC3\xA9 \xC3\xA9t\xC3\xA9";
    for (float width = 10.0f; width < 80.0f; width += 1.0f) {
        text = accented;
        widgets::fitText(vg, text, 11.0f, 11.0f, width, face);
        if (text.size() < 3 || text == accented) continue;  // nothing, or all of it
        const std::string kept = text.substr(0, text.size() - 3);
        CHECK(text.compare(kept.size(), 3, "\xE2\x80\xA6") == 0 && accented.compare(0, kept.size(), kept) == 0);
        CHECK((static_cast<unsigned char>(accented[kept.size()]) & 0xC0) != 0x80);
        CHECK(kept.empty() || kept.back() != ' ');
    }
    nvgDeleteInternal(vg);
}

TEST(settings_copy_and_paste_through_the_clipboard) {
    MockHost host;
    Ui ui{host, 800.0f, 600.0f};
    FakeClipboard clipboard;
    ui.setClipboard(&clipboard);
    const Control& bar = ui.presetBar({20, 20, 320, 28});
    const Rect name = Ui::presetPart(bar.bounds, presetbar::kName);
    size_t i = 0;
    while (i + 1 < params::kCount && (params::at(i).isOutput() || params::at(i).is(pflag::kBypass))) ++i;
    const double changed = params::at(i).hostMax() == host.values[i] ? params::at(i).hostMin() : params::at(i).hostMax();
    host.values[i] = changed;
    ui.pointerDown(name.cx(), name.cy(), 0, 0, false);
    choose(ui, menuRow(ui, "Copy Settings"));
    CHECK(clipboard.text.find(product::info::kName) == 0);
    host.values[i] = hostDefault(params::at(i));
    ui.pointerDown(name.cx(), name.cy(), 0, 0, false);
    choose(ui, menuRow(ui, "Paste Settings"));
    CHECK(host.values[i] == changed);
}

// --- layout -------------------------------------------------------------------------------

TEST(cutting_and_grids_lay_a_panel_out_without_arithmetic) {
    Rect area{10, 20, 300, 200};
    const Rect top = area.cutTop(40, 10);
    CHECK(top.x == 10 && top.y == 20 && top.w == 300 && top.h == 40);
    CHECK(area.y == 70 && area.h == 150);
    const Rect right = area.cutRight(100, 20);
    CHECK(right.x == 210 && right.w == 100 && area.w == 180);
    const Rect left = area.cutLeft(1000);  // more than there is: all of it
    CHECK(left.w == 180 && area.w == 0);
    const Grid g({0, 0, 320, 100}, 3, 2, 10);
    CHECK(g.cellWidth() == 100 && g.cellHeight() == 45);
    CHECK(g.cell(2, 1).x == 220 && g.cell(2, 1).y == 55);
    CHECK(g.span(0, 1, 3, 1).w == 320);
    CHECK(g[4].x == g.cell(1, 1).x && g[4].y == g.cell(1, 1).y);
    const Rect body = panelBody({0, 0, 200, 100});
    CHECK(body.x == 16 && body.y == 44 && body.right() == 184 && body.bottom() == 84);
    CHECK((Rect{0, 0, 100, 50}.centred(20, 10).x == 40));
    const Axis hz = Axis::logarithmic(20.0, 20000.0, 0.0f, 300.0f);
    CHECK_NEAR(hz.at(632.455), 150.0f, 0.01f);
    CHECK_NEAR(hz.value(hz.at(1234.0)), 1234.0, 0.01);
}

// --- tooltips, rules, buttons, pages --------------------------------------------------------

TEST(a_tooltip_shows_after_a_still_moment_and_goes_with_the_pointer) {
    MockHost host;
    Ui ui(host, 800.0f, 600.0f);
    ui.button({20, 20, 100, 30}, "Load", [] {}).withTooltip("Load a file.");
    ui.pointerMove(50, 30, 0);
    ui.idle(0.3);
    CHECK(!ui.tooltip().open);
    ui.idle(0.4);
    CHECK(ui.tooltip().open && ui.tooltip().text == "Load a file.");
    ui.pointerMove(400, 400, 0);  // off it
    CHECK(!ui.tooltip().open);
    ui.pointerMove(50, 30, 0);
    ui.idle(1.0);
    CHECK(ui.tooltip().open);
    ui.pointerDown(50, 30, 0, 0, false);  // using it dismisses it, until the next hover
    CHECK(!ui.tooltip().open);
    ui.idle(1.0);
    CHECK(!ui.tooltip().open);

    // A parameter's help() is its control's tooltip, titled with its name.
    int helped = -1;
    for (size_t i = 0; i < params::kCount && helped < 0; ++i)
        if (params::at(i).description && !params::at(i).isOutput() && !params::at(i).is(pflag::kHidden))
            helped = static_cast<int>(i);
    if (helped < 0) {
        std::printf("      (no help text in this product)\n");
        return;
    }
    Ui other(host, 800.0f, 600.0f);
    other.knob(params::at(static_cast<size_t>(helped)).id, {20, 20, 100, 120});
    other.pointerMove(60, 60, 0);
    other.idle(0.7);
    CHECK(other.tooltip().open && other.tooltip().title == params::at(static_cast<size_t>(helped)).name);
    CHECK(other.tooltip().text == params::at(static_cast<size_t>(helped)).description);
}

TEST(enabled_when_follows_its_rule_and_a_disabled_control_lets_go) {
    Bench b;
    if (b.knob < 0) return;
    bool allowed = true;
    b.ui.control(b.knob).enabledWhen([&] { return allowed; });
    b.ui.pointerDown(70, 50, 0, 0, false);
    b.ui.pointerMove(70, 20, 0);
    CHECK(b.host.openCount == 1);
    allowed = false;
    b.ui.idle(0.016);
    CHECK(!b.c(b.knob).enabled && !b.ui.interactive(b.knob));
    CHECK(b.host.openCount == 0 && b.host.clean());  // the drag it was in is over
    b.ui.pointerUp(70, 20, 0, 0);
    allowed = true;
    b.ui.idle(0.016);
    CHECK(b.c(b.knob).enabled);
}

TEST(a_button_runs_its_action_and_a_momentary_control_is_on_while_held) {
    MockHost host;
    Ui ui(host, 800.0f, 600.0f);
    int pressed = 0;
    ui.button({20, 20, 100, 30}, "Go", [&] { ++pressed; });
    ui.pointerDown(50, 30, 0, 0, false);
    ui.pointerUp(50, 30, 0, 0);
    CHECK(pressed == 1);
    ui.key(key::kEnter, 0);  // focused by the click
    CHECK(pressed == 2);
    CHECK(host.begins == 0);
    const int t = findParam(isToggle);
    if (t < 0) return;
    const size_t i = static_cast<size_t>(t);
    ui.momentary(params::at(i).id, {200, 20, 100, 30});
    host.values[i] = params::at(i).hostMin();  // off, whatever its default
    ui.pointerDown(250, 30, 0, 0, false);
    CHECK(host.values[i] == params::at(i).hostMax() && host.openCount == 1);
    ui.pointerMove(600, 500, 0);  // still held, wherever the pointer goes
    CHECK(host.values[i] == params::at(i).hostMax());
    ui.pointerUp(600, 500, 0, 0);
    CHECK(host.values[i] == params::at(i).hostMin() && host.openCount == 0 && host.clean());
}

TEST(a_momentary_control_releases_when_its_drag_is_cancelled) {
    const int t = findParam(isToggle);
    if (t < 0) return;
    const ParamDef& p = params::at(static_cast<size_t>(t));
    for (int reason = 0; reason < 4; ++reason) {
        MockHost host;
        host.values[static_cast<size_t>(t)] = p.hostMin();
        {
            Ui ui(host, 800.0f, 600.0f);
            ui.tabs({20, 100, 300, 30}, {"One", "Two"});
            ui.onPage(0);
            bool allowed = true;
            ui.momentary(p.id, {20, 20, 100, 30}).enabledWhen([&] { return allowed; });
            ui.pointerDown(50, 30, 0, 0, false);
            CHECK(host.value(p.id) == p.hostMax());
            if (reason == 0) ui.focusLost();
            if (reason == 1) { allowed = false; ui.idle(0.016); }
            if (reason == 2) ui.setPage(0, 1);
            if (reason != 3) {
                CHECK(host.value(p.id) == p.hostMin());
                CHECK(!ui.dragging());
            }
        }  // destruction is also a release, even without a Window
        CHECK(host.value(p.id) == p.hostMin());
        CHECK(host.begins == 1 && host.ends == 1 && host.openCount == 0 && host.clean());
    }
}

TEST(destroying_ui_closes_an_open_parameter_gesture) {
    MockHost host;
    const int i = findParam(isContinuous);
    if (i < 0) return;
    {
        Ui ui(host, 800.0f, 600.0f);
        ui.knob(params::at(static_cast<size_t>(i)).id, {20, 20, 100, 120});
        ui.setFocus(0);
        ui.key(key::kHome, 0);
        ui.key(key::kEnd, 0);
        CHECK(host.openCount == 1);
    }
    CHECK(host.openCount == 0 && host.begins == host.ends && host.clean());
}

TEST(tabs_show_one_page_and_what_is_hidden_takes_no_input) {
    Bench none;
    if (none.knob < 0) return;
    MockHost host;
    Ui ui(host, 800.0f, 600.0f);
    ui.tabs({20, 20, 300, 30}, {"One", "Two"});
    ui.onPage(0);
    ui.knob(none.knobParam, {20, 60, 100, 120});
    ui.onPage(1);
    ui.button({20, 60, 100, 30}, "Two", [] {});
    ui.onAllPages();
    ui.button({400, 60, 100, 30}, "Both", [] {});
    CHECK(ui.shown(1) && !ui.shown(2) && ui.shown(3));
    ui.pointerDown(250, 35, 0, 0, false);  // the second tab
    ui.pointerUp(250, 35, 0, 0);
    CHECK(!ui.shown(1) && ui.shown(2));
    CHECK(ui.page(0) == 1);
    ui.pointerDown(30, 100, 0, 0, false);
    CHECK(host.begins == 0);  // the knob under the button is not there
    ui.setFocus(0);
    ui.key(key::kLeft, 0);  // back with the keyboard
    CHECK(ui.page(0) == 0 && ui.shown(1));
    // Tab skips what is not on the page.
    ui.setFocus(-1);
    ui.key(key::kTab, 0);
    ui.key(key::kTab, 0);
    ui.key(key::kTab, 0);
    CHECK(ui.focus() != 2);
}

// --- two parameters, an envelope, a list, a keyboard -------------------------------------

namespace {
// The product's continuous parameters, as many as it has (up to n).
std::vector<uint32_t> continuousParams(size_t n) {
    std::vector<uint32_t> ids;
    for (size_t i = 0; i < params::kCount && ids.size() < n; ++i)
        if (isContinuous(params::at(i)) && !params::at(i).isOutput() && !params::at(i).is(pflag::kHidden))
            ids.push_back(params::at(i).id);
    return ids;
}
}  // namespace

TEST(a_two_slot_gesture_coalesces_duplicate_parameter_ids) {
    const auto ids = continuousParams(1);
    if (ids.empty()) return;
    const auto& p = params::at(static_cast<size_t>(params::indexOf(ids[0])));
    const double a = p.hostMin() + (p.hostMax() - p.hostMin()) * 0.25;
    const double b = p.hostMin() + (p.hostMax() - p.hostMin()) * 0.75;
    MockHost host;
    Ui ui(host, 800, 600);
    ui.knob(ids[0], {20, 20, 100, 120});
    ui.knob(ids[0], {140, 20, 100, 120});
    host.values[MockHost::slot(ids[0])] = b;
    ui.dragEdit(0, a, 1, b);
    CHECK(host.value(ids[0]) == b && host.begins == 1 && host.openCount == 1);
    ui.dragEdit(0, b, 1, a);
    CHECK(host.value(ids[0]) == a && host.begins == 1);
    ui.endDragEdit();
    CHECK(host.ends == 1 && host.clean());
    ui.xyPad(ids[0], ids[0], {300, 20, 200, 200});
    ui.pointerDown(400, 150, 0, 0, false);
    ui.pointerMove(420, 130, 0);
    ui.pointerUp(420, 130, 0, 0);
    CHECK(host.begins == 2 && host.ends == 2 && host.clean());
}

TEST(a_drawing_edits_any_number_of_parameters_in_one_gesture) {
    const auto ids = continuousParams(3);
    if (ids.size() < 3) return;
    const auto at = [&](int k, double t) {
        const ParamDef& p = params::at(static_cast<size_t>(params::indexOf(ids[static_cast<size_t>(k)])));
        return p.hostMin() + (p.hostMax() - p.hostMin()) * t;
    };
    MockHost host;
    Ui ui(host, 800, 600);
    for (const uint32_t id : ids) ui.knob(id, {0, 0, 1, 1}).visible = false;
    // A press setting three parameters, then drags moving two: one gesture.
    for (int k = 0; k < 3; ++k) ui.dragEdit(k, at(k, 0.2));
    for (int move = 1; move <= 8; ++move) {
        ui.dragEdit(0, at(0, 0.2 + 0.05 * move));
        ui.dragEdit(1, at(1, 0.2 + 0.05 * move));
    }
    CHECK(host.begins == 3 && host.openCount == 3 && host.ends == 0);
    ui.endDragEdit();
    CHECK(host.ends == 3 && host.openCount == 0);
    // The wheel turning two: one gesture per burst.
    for (int notch = 1; notch <= 4; ++notch) {
        ui.wheelEdit(1, at(1, 0.95 - 0.1 * notch));
        ui.wheelEdit(2, at(2, 0.2 + 0.1 * notch));
        ui.idle(0.05);
    }
    CHECK(host.begins == 5 && host.openCount == 2);
    ui.idle(Ui::kGestureIdleSeconds);
    CHECK(host.ends == 5 && host.openCount == 0);
    CHECK(!host.nested && !host.strayEnd && !host.valueOutsideGesture);
}

TEST(an_xy_pad_moves_two_parameters_in_one_gesture) {
    const std::vector<uint32_t> ids = continuousParams(2);
    if (ids.size() < 2) {
        std::printf("      (fewer than two continuous parameters)\n");
        return;
    }
    MockHost host;
    Ui ui(host, 800.0f, 600.0f);
    ui.xyPad(ids[0], ids[1], {100, 100, 216, 216});  // 200 x 200 inside its edge
    const size_t x = static_cast<size_t>(params::indexOf(ids[0])), y = static_cast<size_t>(params::indexOf(ids[1]));
    ui.pointerDown(208, 208, 0, 0, false);  // the middle: both jump there
    CHECK_NEAR(toNormalized(params::at(x), host.values[x]), 0.5, 1e-6);
    CHECK_NEAR(toNormalized(params::at(y), host.values[y]), 0.5, 1e-6);
    ui.pointerMove(258, 158, 0);
    CHECK_NEAR(toNormalized(params::at(x), host.values[x]), 0.75, 1e-6);
    CHECK_NEAR(toNormalized(params::at(y), host.values[y]), 0.75, 1e-6);
    CHECK(host.openCount == 2 && host.clean());
    ui.pointerUp(258, 158, 0, 0);
    CHECK(host.openCount == 0 && host.begins == 2 && host.ends == 2 && host.clean());
    // Sideways only: the vertical parameter is never touched.
    const int before = host.begins;
    ui.pointerDown(108, 158, 0, mod::kShift, false);  // fine: relative, no jump
    ui.pointerMove(158, 158, mod::kShift);
    ui.pointerUp(158, 158, 0, 0);
    CHECK(host.begins == before + 1 && host.clean());
    CHECK_NEAR(toNormalized(params::at(y), host.values[y]), 0.75, 1e-6);
    // Arrows: across and up; Backspace: both to their defaults.
    ui.key(key::kUp, 0);
    CHECK(toNormalized(params::at(y), host.values[y]) > 0.75);
    ui.idle(0.3);
    ui.key(key::kBackspace, 0);
    CHECK(host.values[x] == hostDefault(params::at(x)) && host.values[y] == hostDefault(params::at(y)));
    CHECK(host.openCount == 0 && host.clean());
}

TEST(an_xy_pad_keeps_its_position_when_shift_is_released) {
    const auto ids = continuousParams(2);
    if (ids.size() < 2) return;
    MockHost host;
    Ui ui(host, 800.0f, 600.0f);
    ui.xyPad(ids[0], ids[1], {100, 100, 216, 216});
    ui.pointerDown(208, 208, 0, 0, false);
    ui.pointerMove(208, 208, mod::kShift);
    ui.pointerMove(268, 148, mod::kShift);
    const auto fine = ui.state(0);
    ui.pointerMove(268, 148, 0);
    CHECK_NEAR(ui.state(0).slots[0].normalized, fine.slots[0].normalized, 1e-6);
    CHECK_NEAR(ui.state(0).slots[1].normalized, fine.slots[1].normalized, 1e-6);
    ui.pointerMove(278, 138, 0);
    CHECK_NEAR(ui.state(0).slots[0].normalized, fine.slots[0].normalized + 0.05, 1e-6);
    CHECK_NEAR(ui.state(0).slots[1].normalized, fine.slots[1].normalized + 0.05, 1e-6);
    ui.pointerUp(278, 138, 0, 0);
    CHECK(host.begins == 2 && host.ends == 2 && host.clean());
}

TEST(an_envelopes_handles_edit_their_stages_and_the_corner_moves_two) {
    const std::vector<uint32_t> ids = continuousParams(4);
    if (ids.size() < 4) {
        std::printf("      (fewer than four continuous parameters)\n");
        return;
    }
    MockHost host;
    Ui ui(host, 800.0f, 600.0f);
    ui.envelope({100, 100, 420, 120}, ids[0], ids[1], ids[2], ids[3]);
    auto index = [&](int k) { return static_cast<size_t>(params::indexOf(ids[static_cast<size_t>(k)])); };
    auto shape = [&] { return Ui::envelopeShape(ui.control(0), ui.state(0)); };
    // Each drag goes whichever way leaves room, whatever the defaults are.
    auto away = [&](int k, float d) { return ui.state(0).slots[k].normalized > 0.5 ? -d : d; };
    // The attack's peak, dragged across: only the attack moves.
    Ui::EnvelopeShape e = shape();
    const double decay = host.values[index(1)];
    float dx = away(0, 30.0f);
    ui.pointerDown(e.x[1], e.y[1], 0, 0, false);
    ui.pointerMove(e.x[1] + dx, e.y[1] + 40, 0);
    ui.pointerUp(e.x[1] + dx, e.y[1] + 40, 0, 0);
    CHECK(host.begins == 1 && host.values[index(1)] == decay);
    CHECK_NEAR(shape().x[1], e.x[1] + dx, 0.01f);
    // The corner, across and up or down: decay and sustain together.
    e = shape();
    dx = away(1, 20.0f);
    const float dy = -away(2, 30.0f);  // up is louder
    ui.pointerDown(e.x[2], e.y[2], 0, 0, false);
    ui.pointerMove(e.x[2] + dx, e.y[2] + dy, 0);
    CHECK(host.openCount == 2 && host.clean());
    ui.pointerUp(e.x[2] + dx, e.y[2] + dy, 0, 0);
    CHECK(host.openCount == 0 && host.begins == 3 && host.clean());
    CHECK_NEAR(shape().x[2], e.x[2] + dx, 0.01f);
    CHECK_NEAR(shape().y[2], e.y[2] + dy, 0.01f);
    // The release's end.
    e = shape();
    dx = away(3, 10.0f);
    ui.pointerDown(e.x[4], e.y[4], 0, 0, false);
    ui.pointerMove(e.x[4] + dx, e.y[4], 0);
    ui.pointerUp(e.x[4] + dx, e.y[4], 0, 0);
    CHECK(host.begins == 4 && host.clean());
    CHECK_NEAR(shape().x[4], e.x[4] + dx, 0.01f);
}

namespace {
struct Rows : ListSource {
    int chosen = -1;
    int size() const override { return 30; }
    std::string item(int row) const override { return "Row " + std::to_string(row); }
    bool header(int row) const override { return row % 10 == 0; }
    int selected() const override { return chosen; }
    void choose(int row) override { chosen = row; }
};
}  // namespace

TEST(a_list_scrolls_and_chooses_with_the_pointer_the_wheel_and_keys) {
    MockHost host;
    Ui ui(host, 800.0f, 600.0f);
    Rows rows;
    ui.list({20, 20, 200, 8 + Ui::kListRow * 6}, rows);
    CHECK(Ui::listRows(ui.control(0)) == 6);
    ui.pointerDown(100, 20 + 4 + Ui::kListRow * 2.5f, 0, 0, false);
    CHECK(rows.chosen == 2);
    ui.pointerDown(100, 20 + 4 + Ui::kListRow * 0.5f, 0, 0, false);
    CHECK(rows.chosen == 2);  // a header is not chosen
    ui.wheel(100, 60, 0, -3, 0);
    CHECK(ui.control(0).scroll == 3);
    ui.key(key::kEnd, 0);
    CHECK(rows.chosen == 29 && ui.control(0).scroll == 24);  // and in view
    ui.key(key::kHome, 0);
    CHECK(rows.chosen == 1 && ui.control(0).scroll == 0);
    for (int k = 0; k < 9; ++k) ui.key(key::kDown, 0);
    CHECK(rows.chosen == 11);  // past the header at 10
    CHECK(host.begins == 0);

    // A row cut off by the bottom edge takes a click on the part drawn,
    // and comes into view; below the list nothing does.
    Rows cut;
    Ui partial(host, 800.0f, 600.0f);
    partial.list({20, 20, 200, 8 + Ui::kListRow * 6.5f}, cut);
    CHECK(Ui::listRows(partial.control(0)) == 6);
    const float bottom = partial.control(0).bounds.bottom();
    partial.pointerDown(100, bottom + 1, 0, 0, false);
    CHECK(cut.chosen == -1);
    partial.pointerDown(100, bottom - 4, 0, 0, false);
    CHECK(cut.chosen == 6 && partial.control(0).scroll == 1);

    // A choice parameter's options, chosen in one edit each.
    const int c = findParam([](const ParamDef& p) { return p.kind == ParamKind::Choice; });
    if (c < 0) return;
    const ParamDef& p = params::at(static_cast<size_t>(c));
    Ui other(host, 800.0f, 600.0f);
    other.list(p.id, {20, 20, 200, 200});
    other.setFocus(0);
    other.key(key::kEnd, 0);
    CHECK(host.values[static_cast<size_t>(c)] == p.hostMax());
    CHECK(host.begins == 1 && host.ends == 1 && host.clean());
}

TEST(widgets_clip_inside_an_enclosing_clip_and_leave_it_in_place) {
    MockHost host;
    Ui ui(host, 400.0f, 400.0f);
    Rows rows;
    ui.list({50, 50, 200, 200}, rows);
    std::vector<test::Clip> clips;
    NVGcontext* vg = test::createClipRecorder(&clips);
    nvgBeginFrame(vg, 400, 400, 1.0f);
    nvgScissor(vg, 0, 0, 100, 100);
    const Rect r{50, 50, 200, 200};
    widgets::plot(vg, r, Axis::linear(0, 1, r.x, r.right()), Axis::linear(0, 1, r.bottom(), r.y),
                  [](double v) { return v; }, nvgRGB(255, 255, 255));
    widgets::control(vg, Theme{}, ui.control(0), ui.state(0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, 10, 10);
    nvgFill(vg);
    nvgEndFrame(vg);
    nvgDeleteInternal(vg);
    CHECK(clips.size() > 3);
    for (const test::Clip& c : clips)  // never outside the enclosing clip
        CHECK(c[0] >= 0.0f && c[1] >= 0.0f && c[2] > c[0] && c[3] > c[1] && c[2] <= 100.0f && c[3] <= 100.0f);
    CHECK(clips.back() == (test::Clip{0.0f, 0.0f, 100.0f, 100.0f}));
}

TEST(the_keyboard_plays_a_note_slides_to_the_next_and_always_ends_it) {
    MockHost host;
    Ui ui(host, 800.0f, 600.0f);
    ui.keyboard({0, 0, 280, 100}, 60, 71);  // seven white keys, 40 wide
    CHECK(Ui::keyAt(ui.control(0), 20, 90) == 60);
    CHECK(Ui::keyAt(ui.control(0), 40, 20) == 61);  // the black key, on top
    ui.pointerDown(20, 90, 0, 0, false);
    CHECK(host.held.count(60) == 1 && host.lastVelocity > 0.9);
    ui.pointerMove(60, 90, 0);  // to D
    CHECK(host.held.count(60) == 0 && host.held.count(62) == 1);
    ui.pointerMove(400, 90, 0);  // off the keys
    CHECK(host.held.empty());
    ui.pointerMove(100, 20, 0);  // back on, high on a white key: softer
    CHECK(host.held.count(64) == 1 && host.lastVelocity < 0.5);
    ui.focusLost();  // the release may never come
    CHECK(host.held.empty() && !host.strayNoteOff);
    ui.pointerUp(100, 20, 0, 0);
    CHECK(host.held.empty() && !host.strayNoteOff && host.begins == 0);
    // Keys held elsewhere light up.
    const uint64_t keys[2] = {0, uint64_t{1} << (67 - 64)};
    ui.setKeysHeld(keys);
    CHECK(ui.state(0).keyHeld(67) && !ui.state(0).keyHeld(66));
}

// --- what the editor measures and draws ------------------------------------------------------

TEST(the_fft_matches_a_direct_transform) {
    dsp::Fft fft(16);
    std::mt19937 rng(7);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    std::vector<std::complex<double>> x(16), y;
    for (auto& v : x) v = {u(rng), u(rng)};
    y = x;
    fft.forward(y.data());
    for (size_t k = 0; k < 16; ++k) {
        std::complex<double> sum = 0.0;
        for (size_t n = 0; n < 16; ++n) sum += x[n] * std::polar(1.0, -2.0 * 3.14159265358979323846 * static_cast<double>(k * n) / 16.0);
        CHECK_NEAR(std::abs(sum - y[k]), 0.0, 1e-9);
    }
}

TEST(the_spectrum_reads_a_full_scale_sine_at_0_dB_and_its_rms_at_minus_3) {
    AudioHistory audio;
    const double rate = 48000.0;
    const double hz = 4096.0 / 4096.0 * rate / 4096.0 * 100.0;  // bin 100, exactly
    std::vector<FeedFrame> frames(8192);
    for (size_t n = 0; n < frames.size(); ++n) {
        const float v = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * hz * static_cast<double>(n) / rate));
        frames[n] = {{v, v}, {v, v}};
    }
    audio.push(frames.data(), frames.size());
    Spectrum s(4096);
    s.analyse(audio, Signal::output(0), rate, 0.016);
    CHECK_NEAR(s.db(hz), 0.0, 0.05);
    CHECK(s.db(hz * 3.0) < -90.0);  // nothing where there is nothing
    CHECK_NEAR(s.peak(hz * 0.9, hz * 1.1), 0.0, 0.05);
    // No new samples: elapsed time alone brings the trace to rest.
    s.fall(-1.0);
    CHECK_NEAR(s.db(hz), 0.0, 0.05);
    s.fall(0.5);
    CHECK_NEAR(s.db(hz), -18.0, 0.05);
    CHECK(s.moving());
    s.fall(10.0);
    CHECK(s.db(hz) == Spectrum::kFloorDb && !s.moving());
    // Between bins, Hann's scalloping: never more than 1.5 dB low.
    frames.assign(8192, {});
    const double between = hz + 0.5 * rate / 4096.0;
    for (size_t n = 0; n < frames.size(); ++n) {
        const float v = static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * between * static_cast<double>(n) / rate));
        frames[n] = {{v, v}, {v, v}};
    }
    audio.push(frames.data(), frames.size());
    Spectrum t(4096);
    t.analyse(audio, Signal::output(), rate, 0.016);
    CHECK(t.peak(between * 0.95, between * 1.05) > -1.5);
    // Silence falls at the stated rate, never at once.
    const float before = t.peak(between * 0.95, between * 1.05);
    frames.assign(8192, {});
    audio.push(frames.data(), frames.size());
    t.analyse(audio, Signal::output(), rate, 0.1);
    CHECK_NEAR(t.peak(between * 0.95, between * 1.05), before - t.fallDbPerSecond * 0.1f, 1e-3);

    RmsMeter rms;
    std::vector<float> sine(144000);  // ten time constants: settled
    for (size_t n = 0; n < sine.size(); ++n) sine[n] = static_cast<float>(std::sin(0.1 * static_cast<double>(n)));
    rms.push(sine.data(), sine.size(), rate);
    CHECK_NEAR(rms.levelDb(), -3.01, 0.05);
    // With no samples, it falls as silence would: one time constant, -4.34 dB.
    RmsMeter silent = rms;
    std::vector<float> zeros(static_cast<size_t>(RmsMeter::kWindowSeconds * rate));
    silent.push(zeros.data(), zeros.size(), rate);
    rms.fall(RmsMeter::kWindowSeconds);
    CHECK_NEAR(rms.levelDb(), silent.levelDb(), 1e-3);
    CHECK_NEAR(rms.levelDb(), -3.01 - 4.34, 0.05);
    rms.fall(-1.0);
    CHECK_NEAR(rms.levelDb(), silent.levelDb(), 1e-3);
    CHECK(rms.moving());
    rms.fall(10.0);
    CHECK(rms.levelDb() == RmsMeter::kFloorDb && !rms.moving());
}

TEST(svgs_parse_and_draw_and_every_asset_is_found) {
    const char* source =
        "<svg xmlns='http://www.w3.org/2000/svg' width='24' height='12' viewBox='0 0 24 12'>"
        "<path d='M0 0 H24 V12 H0 Z M6 3 V9 H18 V3 Z' fill='#E8944A' fill-rule='evenodd'/>"
        "<circle cx='12' cy='6' r='2' fill='none' stroke='#FFFFFF' stroke-width='1'/></svg>";
    Svg svg(source);
    CHECK(svg.valid() && svg.width() == 24.0f && svg.height() == 12.0f);
    CHECK(!Svg("not an svg").valid());
    NVGcontext* vg = test::createHeadlessCanvas();
    nvgBeginFrame(vg, 100, 100, 1.0f);
    svg.draw(vg, {0, 0, 48, 48});
    const NVGcolor tint = nvgRGB(255, 0, 0);
    svg.draw(vg, {0, 0, 48, 48}, &tint);
    nvgEndFrame(vg);
    nvgDeleteInternal(vg);

    // A hole drawn the same way round as its outline: the evenodd rule
    // cuts it, the nonzero rule fills it, and an island inside the hole is
    // solid again.
    for (const char* rule : {"evenodd", "nonzero"}) {
        const std::string nested = std::string("<svg xmlns='http://www.w3.org/2000/svg' width='30' height='30'>"
                                               "<path d='M0 0 H30 V30 H0 Z M5 5 H25 V25 H5 Z M10 10 H20 V20 H10 Z' "
                                               "fill-rule='") + rule + "'/></svg>";
        std::vector<std::vector<int>> fills;
        NVGcontext* recorder = test::createFillRecorder(&fills);
        nvgBeginFrame(recorder, 100, 100, 1.0f);
        nvgBeginPath(recorder);
        nvgRect(recorder, 0, 0, 10, 10);  // solid, NanoVG's default
        nvgFill(recorder);
        Svg(nested).draw(recorder, {0, 0, 30, 30});
        nvgEndFrame(recorder);
        nvgDeleteInternal(recorder);
        CHECK(fills.size() == 2 && fills[0] == std::vector<int>{1});
        if (fills.size() != 2 || fills[1].size() != 3) continue;
        fills.erase(fills.begin());
        if (std::string(rule) == "evenodd") CHECK(fills[0][0] == 1 && fills[0][1] == -1 && fills[0][2] == 1);
        else CHECK(fills[0][0] == fills[0][1] && fills[0][1] == fills[0][2]);
    }
    CHECK(!assets::find("no-such-asset.png"));
    for (size_t i = 0; i < assets::count(); ++i) {
        const assets::Asset a = assets::at(i);
        CHECK(assets::find(a.name).data == a.data && a.size > 0);
        if (assets::hasExtension(a.name, ".svg"))
            CHECK_MSG(Svg(a.text()).valid(), "%.*s does not parse", static_cast<int>(a.name.size()), a.name.data());
    }
    std::printf("      %zu assets in this product\n", assets::count());
}

TEST(dropped_uri_lists_become_paths_and_dialog_commands_are_built) {
    const std::vector<std::string> paths =
        pathsFromUriList("# a comment\r\nfile:///home/a%20b/x.wav\r\nfile://localhost/tmp/y.preset\n"
                         "http://example.com/z\nfile://elsewhere/w\nfile:///C:/Users/q.wav\n\n");
    CHECK(paths.size() == 3);
    CHECK(paths[0] == "/home/a b/x.wav");
    CHECK(paths[1] == "/tmp/y.preset");
    CHECK(paths[2] == "C:/Users/q.wav");
    FileDialogs::Options o;
    o.title = "Load";
    o.extensions = {".wav", ".aif"};
    const std::vector<std::string> zenity = linuxDialogCommand("zenity", o, false, 42);
    CHECK(zenity[0] == "zenity" && zenity[1] == "--file-selection");
    CHECK(std::find(zenity.begin(), zenity.end(), "--file-filter=*.wav *.aif") != zenity.end());
    CHECK(std::find(zenity.begin(), zenity.end(), "--attach=42") != zenity.end());
    o.name = "Mine.preset";
    o.folder = "/tmp";
    const std::vector<std::string> kdialog = linuxDialogCommand("kdialog", o, true, 0);
    CHECK(kdialog[1] == "--getsavefilename" && kdialog[2] == "/tmp/Mine.preset");
}

#if defined(__linux__)
TEST(a_linux_dialog_finishes_even_when_the_host_reaps_it) {
    // A stand-in zenity that answers at once, chosen or cancelled.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "supple-dialog-test";
    std::filesystem::create_directories(dir);
    const std::string savedPath = std::getenv("PATH") ? std::getenv("PATH") : "";
    const std::string savedDesktop = std::getenv("XDG_CURRENT_DESKTOP") ? std::getenv("XDG_CURRENT_DESKTOP") : "";
    setenv("PATH", dir.c_str(), 1);
    unsetenv("XDG_CURRENT_DESKTOP");
    auto run = [&](const char* script) {
        {
            std::ofstream out(dir / "zenity");
            out << "#!/bin/sh\n" << script << "\n";
        }
        std::filesystem::permissions(dir / "zenity", std::filesystem::perms::owner_all);
        NativeFiles files(0);
        bool done = false;
        std::vector<std::string> chosen{"unset"};
        CHECK(files.open({}, [&](const std::vector<std::string>& paths) {
            done = true;
            chosen = paths;
        }));
        for (int i = 0; i < 500 && !done; ++i) {
            files.poll();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        CHECK(done);
        return chosen;
    };
    const std::vector<std::string> picked{"/home/someone/a b.wav"};
    CHECK(run("echo '/home/someone/a b.wav'") == picked);
    CHECK(run("exit 1").empty());
    // A host that collects every child itself: the exit status never comes.
    struct sigaction ignore{}, saved{};
    ignore.sa_handler = SIG_IGN;
    sigaction(SIGCHLD, &ignore, &saved);
    CHECK(run("echo '/home/someone/a b.wav'") == picked);
    CHECK(run("exit 1").empty());
    sigaction(SIGCHLD, &saved, nullptr);
    setenv("PATH", savedPath.c_str(), 1);
    if (!savedDesktop.empty()) setenv("XDG_CURRENT_DESKTOP", savedDesktop.c_str(), 1);
    std::filesystem::remove_all(dir);
}
#endif

// --- the product's editor -------------------------------------------------------------------

namespace {
struct ProductEditor {
    MockHost host;
    std::unique_ptr<EditorBase> editor = product::createEditor(host);
    NVGcontext* vg = test::createHeadlessCanvas();
    ProductEditor() { editor->attach(vg); }
    ~ProductEditor() {
        editor->detach();
        editor.reset();
        nvgDeleteInternal(vg);
    }
    void draw() {
        nvgBeginFrame(vg, editor->width(), editor->height(), 1.0f);
        editor->render(vg);
        nvgEndFrame(vg);
    }
};
}  // namespace

#if SUPPLE_AUTO_EDITOR
TEST(the_automatic_editor_dims_what_the_definition_says_does_not_apply) {
    // The parameter the first focused control edits applies only away from
    // its default, by the host's values; a dimmed control takes no keys.
    struct Moot final : Definition {
        using Definition::Definition;
        size_t index = 0;
        bool applies(size_t i, const std::function<double(size_t)>& host) const override {
            return i != index || host(i) != hostDefault(params().at(i));
        }
    };
    struct MootHost final : MockHost {
        const Definition* definition = nullptr;
        const Definition& pluginDefinition() const override { return *definition; }
    };
    const Definition& product = *productDefinition();
    Moot moot(product.info(), ParamTable(product::kParams, params::kCount));
    // Tab `tabs` times, then Up: the parameter that moved when only one did
    // (Up on the preset bar loads a preset), or -1.
    auto nudge = [](MockHost& host, int tabs) {
        double before[params::kCount];
        std::copy(std::begin(host.values), std::end(host.values), before);
        std::unique_ptr<EditorBase> editor = product::createEditor(host);
        editor->idle(0.0);
        for (int t = 0; t < tabs; ++t) editor->key(key::kTab, 0);
        editor->key(key::kUp, 0);
        int moved = -1;
        for (size_t i = 0; i < params::kCount; ++i)
            if (host.values[i] != before[i]) {
                if (moved >= 0) return -1;
                moved = static_cast<int>(i);
            }
        return moved;
    };
    // The first continuous parameter Tab reaches.
    int tabs = 0, index = -1;
    for (int t = 1; t <= 16 && index < 0; ++t) {
        MockHost probe;
        const int moved = nudge(probe, t);
        if (moved >= 0 && params::at(static_cast<size_t>(moved)).kind == ParamKind::Continuous) {
            tabs = t;
            index = moved;
        }
    }
    if (index < 0) {
        std::printf("    (no continuous control takes arrow keys: skipped)\n");
        return;
    }
    moot.index = static_cast<size_t>(index);
    MootHost dimmed;  // at its default: dimmed, so Tab passes it by
    dimmed.definition = &moot;
    CHECK(nudge(dimmed, tabs) != index);
    MootHost applying;  // away from it: it takes the key
    applying.definition = &moot;
    applying.values[moot.index] = toHost(params::at(moot.index), params::at(moot.index).max) * 0.5 +
                                  hostDefault(params::at(moot.index)) * 0.5;
    CHECK(nudge(applying, tabs) == index);
}

TEST(the_automatic_editor_leaves_out_a_hidden_property) {
    // A text field makes room for itself; a hidden property's makes none.
    struct PropertyHost final : MockHost {
        std::unique_ptr<Definition> definition;
        const Definition& pluginDefinition() const override { return *definition; }
    };
    const Definition& product = *productDefinition();
    auto size = [&](std::vector<PropertyDef> properties) {
        PropertyHost host;
        host.definition = std::make_unique<Definition>(
            product.info(), ParamTable(product::kParams, params::kCount), PropertyList(std::move(properties)));
        std::unique_ptr<EditorBase> editor = product::createEditor(host);
        return std::pair(editor->width(), editor->height());
    };
    const auto none = size({});
    CHECK(size({property(900, "Notes")}) != none);
    CHECK(size({property(900, "Notes").hidden()}) == none);
}
#endif

TEST(rebuilding_after_a_window_draw_keeps_the_displayed_editor_alive) {
    MockHost host;
    const Theme theme{};
    bool alive[4]{};
    int made = 0;
    struct TrackedEditor final : EditorBase {
        bool& alive;
        TrackedEditor(EditorHost& h, const Theme& t, bool& a) : EditorBase(h, t, 160.0f, 120.0f), alive(a) {
            alive = true;
        }
        ~TrackedEditor() override { alive = false; }
        void paint(NVGcontext*) override {}
    };
    Gui gui(host, "supple-rebuild-test", true, [&](EditorHost& h) {
        return std::make_unique<TrackedEditor>(h, theme, alive[made++]);
    });
    if (!gui.create() || !gui.setParent(0) || !gui.show()) {
        std::printf("    (no window available: rebuild check skipped)\n");
        return;
    }
    gui.rebuild(host);
    // A native/fallback update may adopt the replacement without Gui::tick
    // having collected the retired editor yet.
    for (int n = 0; n < 100 && gui.window().replacing(); ++n) {
        gui.window().tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(!gui.window().replacing());
    if (gui.window().replacing()) return;
    gui.rebuild(host);
    CHECK(alive[1]);  // still attached until the next draw
    CHECK(alive[2]);  // waiting to replace it
    // Another rebuild before drawing replaces only the waiting editor.
    gui.rebuild(host);
    CHECK(alive[1] && !alive[2] && alive[3]);
    gui.tick();
}

TEST(the_editor_draws_without_a_window_at_any_scale) {
    MockHost host;
    std::unique_ptr<EditorBase> editor = product::createEditor(host);
    Picture picture;
    std::string why;
    if (!drawOffscreen(*editor, 1.5, picture, &why)) {
        CHECK_MSG(!std::getenv("SUPPLE_REQUIRE_OFFSCREEN"), "%s", why.c_str());
        std::printf("    (%s: skipped)\n", why.c_str());
        CHECK(!why.empty() && picture.rgba.empty());
        return;
    }
    CHECK(picture.width == static_cast<int>(std::lround(editor->width() * 1.5)));
    CHECK(picture.height == static_cast<int>(std::lround(editor->height() * 1.5)));
    CHECK(picture.rgba.size() == static_cast<size_t>(picture.width) * static_cast<size_t>(picture.height) * 4);
    // Not one colour: the editor drew something on its ground.
    std::set<uint32_t> colours;
    for (int y = 0; y < picture.height; y += 7)
        for (int x = 0; x < picture.width; x += 7) colours.insert(picture.rgb(x, y));
    CHECK(colours.size() > 8);
    // Drawn again, the same picture, and the editor left as it was.
    Picture again;
    CHECK(drawOffscreen(*editor, 1.5, again) && again.rgba == picture.rgba);
}

// Fixed colours make channel order, row order, scaling and compositing
// measurable; a product's changing artwork cannot serve as those assertions.
TEST(offscreen_pixels_have_rgba_channels_top_rows_and_opaque_compositing) {
    MockHost host;
    const Theme theme{};
    struct Swatches final : EditorBase {
        Swatches(EditorHost& h, const Theme& t) : EditorBase(h, t, 64, 48) { usePeakMeters(false); }
        void paint(NVGcontext* vg) override {
            auto fill = [&](float y, NVGcolor colour) {
                nvgBeginPath(vg);
                nvgRect(vg, 0, y, 64, 24);
                nvgFillColor(vg, colour);
                nvgFill(vg);
            };
            fill(0, nvgRGBA(255, 0, 0, 255));
            fill(24, nvgRGBA(0, 0, 255, 255));
            nvgBeginPath(vg);
            nvgRect(vg, 32, 0, 32, 48);
            nvgFillColor(vg, nvgRGBA(0, 255, 0, 128));
            nvgFill(vg);
        }
    } editor(host, theme);
    for (double scale : {0.25, 1.0, 1.5, 2.0, 4.0}) {
        Picture picture;
        std::string why;
        if (!drawOffscreen(editor, scale, picture, &why)) {
            CHECK_MSG(!std::getenv("SUPPLE_REQUIRE_OFFSCREEN"), "%s", why.c_str());
            std::printf("    (%s: skipped)\n", why.c_str());
            return;
        }
        CHECK(picture.width == static_cast<int>(64 * scale));
        CHECK(picture.height == static_cast<int>(48 * scale));
        CHECK(picture.rgb(picture.width / 4, picture.height / 4) == 0xff0000);
        CHECK(picture.rgb(picture.width / 4, 3 * picture.height / 4) == 0x0000ff);
        const size_t mixed = (static_cast<size_t>(picture.height / 4) * static_cast<size_t>(picture.width) +
                              static_cast<size_t>(3 * picture.width / 4)) * 4;
        CHECK_NEAR(picture.rgba[mixed], 127, 1);
        CHECK_NEAR(picture.rgba[mixed + 1], 128, 1);
        CHECK(picture.rgba[mixed + 2] == 0);
        for (size_t a = 3; a < picture.rgba.size(); a += 4) CHECK(picture.rgba[a] == 255);
        Picture again;
        CHECK(drawOffscreen(editor, scale, again) && picture.rgba == again.rgba);
    }
}

TEST(an_editor_drawn_without_a_window_takes_pointer_input_first) {
    MockHost host;
    std::unique_ptr<EditorBase> editor = product::createEditor(host);
    int k = -1;
    for (int i = 0; i < editor->layout().count() && k < 0; ++i) {
        const Control& c = editor->layout().control(i);
        if (c.widget == Widget::Knob && !c.rule && editor->layout().interactive(i) &&
            isContinuous(*params::find(c.param)))
            k = i;
    }
    if (k < 0) return;
    const Control& c = editor->layout().control(k);
    const ParamDef& p = *params::find(c.param);
    host.values[MockHost::slot(c.param)] = 0.5 * (p.hostMin() + p.hostMax());
    const double before = host.value(c.param);
    using Kind = PointerInput::Kind;
    const float x = c.bounds.cx(), y = c.bounds.cy();
    const PointerInput drag[] = {{Kind::Move, x, y}, {Kind::Press, x, y}, {Kind::Move, x, y - 40.0f},
                                 {Kind::Release, x, y - 40.0f}};
    Picture picture;
    std::string why;
    if (!drawOffscreen(*editor, 1.0, drag, picture, &why)) {
        CHECK_MSG(!std::getenv("SUPPLE_REQUIRE_OFFSCREEN"), "%s", why.c_str());
        std::printf("    (%s: skipped)\n", why.c_str());
        CHECK(host.value(c.param) == before && host.begins == 0);
        return;
    }
    CHECK(host.value(c.param) > before && host.openCount == 0 && host.clean());
    // A wheel burst's gesture, still open as it is drawn, ends with the picture.
    const PointerInput turn[] = {{Kind::Wheel, x, y, -1.0f}};
    const double dragged = host.value(c.param);
    CHECK(drawOffscreen(*editor, 1.0, turn, picture));
    CHECK(host.value(c.param) < dragged && host.openCount == 0 && host.clean());
}

TEST(the_layout_is_inside_the_panel_and_controls_do_not_overlap) {
    ProductEditor p;
    const Ui& ui = p.editor->layout();
    CHECK(ui.count() > 0);
    for (int i = 0; i < ui.count(); ++i) {
        const Control& a = ui.control(i);
        if (a.widget == Widget::TextField || a.widget == Widget::File) {
            CHECK_MSG(properties::indexOf(a.param) >= 0, "text field %d names property %u, which is not in the table", i,
                      a.param);
            CHECK_MSG(a.widget != Widget::File || (properties::indexOf(a.param) >= 0 &&
                                                   properties::at(static_cast<size_t>(properties::indexOf(a.param))).isFile()),
                      "file slot %d names property %u, which is not a file()", i, a.param);
        } else if (a.widget == Widget::PresetBar) {
            CHECK_MSG(product::info::kPresets, "control %d is a preset bar, and presets are off", i);
        } else if (a.widget == Widget::Keyboard) {
            CHECK_MSG(product::info::kNoteInput, "control %d is a keyboard, and the product takes no notes", i);
        } else if (a.widget == Widget::Tabs || a.widget == Widget::Button || (a.widget == Widget::List && a.param == kNoParam)) {
            CHECK_MSG(a.widget != Widget::List || a.source, "list %d has nothing to list", i);
        } else {
            int bound = 0;
            for (int k = 0; k < Control::kSlots; ++k) {
                if (a.paramAt(k) == kNoParam) continue;
                ++bound;
                CHECK_MSG(params::find(a.paramAt(k)), "control %d names parameter %u, which is not in the table", i,
                          a.paramAt(k));
            }
            CHECK_MSG(bound > 0, "control %d is bound to no parameter", i);
        }
        CHECK_MSG(a.bounds.x >= 0 && a.bounds.y >= 0 && a.bounds.right() <= p.editor->width() &&
                      a.bounds.bottom() <= p.editor->height(),
                  "control %d lies outside the %gx%g panel", i, p.editor->width(), p.editor->height());
        for (int j = i + 1; j < ui.count(); ++j) {
            const Control& b = ui.control(j);
            // Pages of one tab bar take turns in the same place.
            if (a.tabs >= 0 && a.tabs == b.tabs && a.page != b.page) continue;
            CHECK_MSG(!a.bounds.overlaps(b.bounds), "controls %d and %d overlap: a click could reach the wrong one", i, j);
        }
    }
}

TEST(a_preset_file_dropped_on_the_editor_loads) {
    if (!product::info::kPresets) {
        std::printf("      (no presets in this product)\n");
        return;
    }
    ProductEditor p;
    size_t k = 0;
    while (k + 1 < params::kCount && (params::at(k).isOutput() || params::at(k).is(pflag::kHidden))) ++k;
    const double saved = fromNormalized(params::at(k), 0.8);
    p.host.values[k] = saved;
    const std::filesystem::path file = p.host.folder.parent_path() / ("supple-drop-" + std::to_string(std::random_device{}()) + ".preset");
    CHECK(p.host.presets->exportFile(file));
    p.host.values[k] = fromNormalized(params::at(k), 0.2);
    CHECK(p.editor->dropOver(10, 10));  // an editor with presets takes them anywhere
    p.editor->drop(10, 10, {PresetLibrary::toUtf8(file)});
    CHECK(p.host.values[k] == saved);
    p.editor->drop(10, 10, {"/nowhere/at/all.wav"});  // not a preset, and nobody asked for it
    CHECK(p.host.values[k] == saved);
    std::error_code ec;
    std::filesystem::remove(file, ec);
}

TEST(custom_pointer_hooks_preserve_gestures_and_respect_overlays) {
    const auto ids = continuousParams(1);
    if (ids.empty()) return;
    struct HookEditor : EditorBase {
        using EditorBase::ui;
        int moves = 0;
        explicit HookEditor(EditorHost& host, uint32_t id) : EditorBase(host, Theme{}, 800, 600) {
            ui().knob(id, {20, 20, 100, 120});
        }
        void paint(NVGcontext*) override {}
        bool onPointerDown(float, float, int, uint32_t, bool) override {
            ui().dragEdit(0, 0.25);
            return true;
        }
        bool onPointerMove(float, float, uint32_t) override { ++moves; return true; }
    };
    MockHost host;
    host.values[MockHost::slot(ids[0])] = 0.75;
    HookEditor editor(host, ids[0]);
    editor.pointerDown(400, 300, 0, 0, false);
    CHECK(host.openCount == 1 && host.ends == 0);
    editor.ui().endDragEdit();
    editor.ui().openTextEdit(0);
    editor.pointerMove(50, 50, 0);
    CHECK(editor.moves == 0);
    editor.key(key::kEscape, 0);
    editor.ui().openContextMenu(0, 50, 50);
    editor.pointerMove(60, 60, 0);
    CHECK(editor.moves == 0);
    editor.key(key::kEscape, 0);
    editor.pointerMove(400, 300, 0);
    CHECK(editor.moves == 1 && host.clean());
}

TEST(a_press_a_pointer_hook_takes_holds_the_pointer_until_its_release) {
    const auto ids = continuousParams(2);
    if (ids.size() < 2) return;
    struct DragEditor : EditorBase {
        using EditorBase::ui;
        bool held = false;
        int moves = 0, ups = 0;
        DragEditor(EditorHost& host, uint32_t a, uint32_t b) : EditorBase(host, Theme{}, 800, 600) {
            ui().knob(a, {20, 20, 100, 120});
            ui().knob(b, {200, 20, 100, 120});
        }
        void paint(NVGcontext*) override {}
        bool onPointerDown(float x, float, int, uint32_t, bool) override {
            if (x < 400.0f) return false;  // the knobs are Ui's
            held = true;
            ui().dragEdit(0, 0.25);
            return true;
        }
        bool onPointerMove(float x, float, uint32_t) override {
            ++moves;
            if (held) ui().dragEdit(0, x / 800.0);
            return held;
        }
        bool onPointerUp(float, float, int, uint32_t) override {
            ++ups;
            if (held) ui().endDragEdit();
            held = false;
            return true;
        }
    };
    MockHost host;
    host.values[MockHost::slot(ids[0])] = 0.75;
    const ParamDef& second = *params::find(ids[1]);
    host.values[MockHost::slot(ids[1])] = 0.5 * (second.hostMin() + second.hostMax());  // room to move either way
    DragEditor editor(host, ids[0], ids[1]);
    editor.ui().setFocus(1);
    editor.pointerDown(500, 300, 0, 0, false);
    CHECK(host.openCount == 1);
    // Neither the wheel over the other knob nor a key for the focused one
    // can end the drag's gesture.
    const double other = host.value(ids[1]);
    CHECK(editor.wheel(250, 80, 0, 1, 0));
    CHECK(editor.key(key::kUp, 0));
    CHECK(host.value(ids[1]) == other && host.openCount == 1 && host.ends == 0);
    // Moves over Ui's controls, and the release, still come to the hooks.
    editor.pointerMove(260, 80, 0);
    CHECK(editor.moves == 1 && host.openCount == 1);
    editor.pointerUp(260, 80, 0, 0);
    CHECK(editor.ups == 1 && host.openCount == 0 && host.clean());
    // Released, the wheel is Ui's again; a release the hooks never took a
    // press for does not reach them.
    CHECK(editor.wheel(250, 80, 0, 1, 0) && host.value(ids[1]) != other);
    editor.pointerDown(250, 80, 0, 0, false);
    editor.pointerUp(250, 80, 0, 0);
    CHECK(editor.ups == 1);
}

TEST(the_wheel_and_the_pointer_leaving_reach_a_products_hooks_first) {
    const auto ids = continuousParams(1);
    if (ids.empty()) return;
    struct WheelEditor : EditorBase {
        using EditorBase::ui;
        int wheels = 0, leaves = 0;
        WheelEditor(EditorHost& host, uint32_t id) : EditorBase(host, Theme{}, 800, 600) {
            ui().knob(id, {20, 20, 100, 120});
        }
        void paint(NVGcontext*) override {}
        bool onWheel(float x, float, float, float, uint32_t) override {
            ++wheels;
            return x >= 400.0f;  // the knob's side is Ui's
        }
        void onPointerLeave() override { ++leaves; }
    };
    MockHost host;
    const ParamDef& p = *params::find(ids[0]);
    host.values[MockHost::slot(ids[0])] = 0.5 * (p.hostMin() + p.hostMax());
    WheelEditor editor(host, ids[0]);
    const double before = host.value(ids[0]);
    CHECK(editor.wheel(500, 300, 0, 1, 0) && editor.wheels == 1 && host.value(ids[0]) == before);
    CHECK(editor.wheel(70, 80, 0, 1, 0) && editor.wheels == 2 && host.value(ids[0]) != before);
    // An open menu comes first.
    editor.ui().openContextMenu(0, 70, 80);
    editor.wheel(500, 300, 0, 1, 0);
    CHECK(editor.wheels == 2);
    editor.key(key::kEscape, 0);
    editor.pointerLeave();
    CHECK(editor.leaves == 1 && host.clean());
}

namespace {
template<class ProductTelemetry>
void checkGainHistory() {
    if constexpr (requires(ProductTelemetry t) { t.key; t.gain; }) {
        MockHost host;
        host.rate = 192000.0;
        auto editor = product::createEditor(host);
        uint64_t hash = 0;
        NVGcontext* vg = test::createHeadlessCanvas(&hash);
        CHECK(editor->attach(vg));
        auto draw = [&] {
            hash = 0;
            nvgBeginFrame(vg, editor->width(), editor->height(), 1);
            editor->render(vg);
            nvgEndFrame(vg);
            return hash;
        };
        TelemetryFrame frame{};
        ProductTelemetry first{};
        first.gain = 1.0f;
        frame.product = first;
        editor->onTelemetry(frame);
        const uint64_t before = draw();
        first.gain = 0.25f;
        frame.product = first;
        editor->onTelemetry(frame);
        CHECK(draw() != before);  // host output values are unchanged
        first.gain = 1.0f;
        frame.product = first;
        for (int i = 0; i < 60; ++i) editor->onTelemetry(frame);
        // At 192 kHz the audio history holds less than a second: reduction
        // from a second ago must have scrolled off the same graph.
        CHECK(draw() == before);
        editor->detach();
        nvgDeleteInternal(vg);
    }
}
}  // namespace

TEST(gain_history_uses_each_telemetry_frame) {
    checkGainHistory<decltype(TelemetryFrame{}.product)>();
}

TEST(the_last_meter_frame_repaints_before_the_editor_idles) {
    struct MeterEditor : EditorBase {
        explicit MeterEditor(EditorHost& host) : EditorBase(host, Theme{}, 100, 100) {}
        void paint(NVGcontext*) override {}
        bool moving() const { return outputMeters()[0].moving(); }
    };
    MockHost host;
    MeterEditor editor(host);
    NVGcontext* vg = test::createHeadlessCanvas();
    CHECK(editor.attach(vg));
    TelemetryFrame frame{};
    frame.outputPeak[0] = 0.5f;
    host.ring.push(frame);
    editor.idle(0.016);
    CHECK(editor.moving());
    for (int i = 0; i < 200 && editor.moving(); ++i) {
        nvgBeginFrame(vg, 100, 100, 1.0f);
        editor.render(vg);
        nvgEndFrame(vg);
        editor.idle(0.05);
        CHECK(editor.needsRepaint());  // includes the tick that reaches the floor
    }
    CHECK(!editor.moving());
    nvgBeginFrame(vg, 100, 100, 1.0f);
    editor.render(vg);
    nvgEndFrame(vg);
    editor.idle(0.05);
    CHECK(!editor.needsRepaint());
    editor.detach();
    nvgDeleteInternal(vg);
}

TEST(rms_meters_fall_and_repaint_when_the_host_stops) {
    if constexpr (!product::Processor::kAudioFeed) {
        std::printf("      (no audio feed in this product)\n");
        return;
    }
    struct FeedHost : MockHost {
        size_t pending = 0;
        size_t readAudio(FeedFrame* out, size_t max) override {
            const size_t n = std::min(pending, max);
            for (size_t k = 0; k < n; ++k) out[k] = {{0.5f, 0.5f}, {0.5f, 0.5f}};
            pending -= n;
            return n;
        }
    };
    struct RmsEditor : EditorBase {
        explicit RmsEditor(EditorHost& host) : EditorBase(host, Theme{}, 100, 100) {}
        void paint(NVGcontext*) override {}
        void onAudio(size_t) override {}  // only the meters ask to repaint
        float level() const { return outputRms()[0].levelDb(); }
    };
    FeedHost host;
    RmsEditor editor(host);
    NVGcontext* vg = test::createHeadlessCanvas();
    CHECK(editor.attach(vg));
    auto draw = [&] {
        nvgBeginFrame(vg, 100, 100, 1.0f);
        editor.render(vg);
        nvgEndFrame(vg);
    };
    host.pending = static_cast<size_t>(10 * host.sampleRate());
    editor.idle(0.016);
    CHECK_NEAR(editor.level(), -6.02, 0.05);  // 0.5 held: settled
    draw();
    editor.idle(0.1);  // a gap between blocks: nothing falls yet
    CHECK_NEAR(editor.level(), -6.02, 0.05);
    CHECK(!editor.needsRepaint());
    editor.idle(0.2);
    CHECK(editor.level() < -6.5);
    int ticks = 0;
    for (; ticks < 200 && editor.level() > RmsMeter::kFloorDb; ++ticks) {
        CHECK(editor.needsRepaint());  // including the tick that reaches the floor
        draw();
        editor.idle(0.05);
    }
    CHECK(ticks < 200 && editor.needsRepaint());
    draw();
    editor.idle(0.05);
    CHECK(!editor.needsRepaint());
    editor.detach();
    nvgDeleteInternal(vg);
}

TEST(host_sizes_become_pixels_without_wrapping) {
    CHECK(Window::pixels(800, 1.0) == 800 && Window::pixels(800, 2.0) == 1600);
    CHECK(Window::pixels(0x7FFF, 2.0) == 0xFFFE);
    CHECK(Window::pixels(0x8000, 2.0) == Window::kMaxSpan + 1);
    CHECK(Window::pixels(0x80000000u, 2.0) == Window::kMaxSpan + 1);  // 2^32 pixels: once 0, and accepted
    CHECK(Window::pixels(0xFFFFFFFFu, 1.0) == Window::kMaxSpan + 1);  // past a 32-bit long
}

TEST(an_idle_editor_repaints_when_the_host_changes_a_value) {
    ProductEditor p;
    p.editor->idle(0.016);
    p.draw();
    p.editor->idle(0.016);
    CHECK(!p.editor->needsRepaint());  // nothing moved: draw nothing
    // Automation, a generic host editor or a preset, with no audio running.
    size_t k = 0;
    while (k + 1 < params::kCount && params::at(k).isOutput()) ++k;
    const double moved = fromNormalized(params::at(k), 0.9);
    p.host.values[k] = moved != p.host.values[k] ? moved : fromNormalized(params::at(k), 0.1);
    p.editor->idle(0.016);
    CHECK(p.editor->needsRepaint());
    p.draw();
    CHECK(!p.editor->needsRepaint());
}

TEST(an_output_repaints_the_editor_only_where_it_is_shown) {
    for (size_t i = 0; i < params::kCount; ++i) {
        if (!params::at(i).isOutput()) continue;
        ProductEditor p;
        const Ui& ui = p.editor->layout();
        bool shown = false;
        for (int c = 0; c < ui.count(); ++c) shown |= ui.control(c).param == params::at(i).id && ui.shown(c);
        p.editor->idle(0.016);
        p.draw();
        p.host.values[i] = fromNormalized(params::at(i), 0.6);  // the processor reported something new
        p.editor->idle(0.016);
        // A product drawing it its own way watch()es it; that is not visible from here.
        if (shown) CHECK_MSG(p.editor->needsRepaint(), "%s is on screen and moved", params::at(i).name);
        std::printf("      %s: %s\n", params::at(i).name, shown ? "shown, repaints" : "not shown by a control");
    }
}

TEST(silence_does_not_repaint_and_signal_does) {
    if (!product::info::kAudioOutput) {
        std::printf("      (no audio in this product)\n");
        return;
    }
    // A genuinely quiet frame, product payload included, from a real engine.
    auto engine = std::make_unique<Engine>();
    engine->activate(48000.0, 512, product::info::kAudioInput ? 2 : 0, 2);
    engine->setMetering(true);
    std::vector<double> l(512), r(512);
    double* io[] = {l.data(), r.data()};
    for (int b = 0; b < 4; ++b) {
        // In place, so the input is re-zeroed: the last block's output is
        // not silence for a product with a noise floor.
        std::fill(l.begin(), l.end(), 0.0);
        std::fill(r.begin(), r.end(), 0.0);
        engine->process<double>(io, io, 512);
    }
    // The newest frame: steady state, not whatever the product does as it
    // starts up.
    TelemetryFrame quiet{}, next{};
    CHECK(engine->telemetry().pop(quiet));
    while (engine->telemetry().pop(next)) quiet = next;

    ProductEditor p;
    for (int k = 0; k < 3; ++k) p.host.ring.push(quiet);
    p.editor->idle(0.016);
    p.draw();
    for (int k = 0; k < 10; ++k) p.host.ring.push(quiet);
    p.editor->idle(0.016);
    CHECK(!p.editor->needsRepaint());
    // Signal, through the same engine: a product shows it somewhere.
    for (int b = 0; b < 4; ++b) {
        for (int k = 0; k < 512; ++k) l[static_cast<size_t>(k)] = r[static_cast<size_t>(k)] = 0.5 * std::sin(0.13 * (b * 512 + k));
        engine->process<double>(io, io, 512);
    }
    TelemetryFrame loud{};
    CHECK(engine->telemetry().pop(loud));
    while (engine->telemetry().pop(next)) loud = next;
    // An instrument's signal comes from notes, which this does not play.
    if (!product::info::kAudioInput) loud.outputPeak[0] = loud.outputPeak[1] = 0.5f;
    p.host.ring.push(loud);
    p.editor->idle(0.016);
    CHECK(p.editor->needsRepaint());
}

TEST(random_use_of_the_whole_editor_keeps_the_gesture_contract) {
    ProductEditor p;
    std::mt19937 rng(1234);
    const float w = p.editor->width(), h = p.editor->height();
    std::uniform_real_distribution<float> x(0.0f, w), y(0.0f, h), step(-40.0f, 40.0f);
    const uint32_t keys[] = {key::kTab, key::kUp, key::kDown, key::kLeft, key::kRight, key::kHome, key::kEnd,
                             key::kPageUp, key::kEnter, key::kEscape, key::kBackspace, '1', 'k'};
    float px = w * 0.5f, py = h * 0.5f;
    bool down = false;
    for (int k = 0; k < 20000; ++k) {
        const uint32_t mods = (rng() % 4 == 0 ? mod::kShift : 0u) | (rng() % 15 == 0 ? mod::kCtrl : 0u);
        switch (rng() % 10) {
            case 0:
                px = x(rng), py = y(rng);
                p.editor->pointerDown(px, py, rng() % 6 == 0 ? 1 : 0, mods, rng() % 5 == 0);
                down = true;
                break;
            case 1:
            case 2:
                px += step(rng), py += step(rng);
                p.editor->pointerMove(px, py, mods);
                break;
            case 3:
                if (down) p.editor->pointerUp(px, py, 0, mods);
                down = false;
                break;
            case 4: p.editor->wheel(x(rng), y(rng), 0.0f, step(rng) / 20.0f, mods); break;
            case 5: p.editor->key(keys[rng() % (sizeof(keys) / sizeof(keys[0]))], mods); break;
            case 6: {
                static const uint32_t typed[] = {'0', '1', '5', '.', '-', 'k', '%', ' ', 'a', 0xE9, 0x2713, 0x1F600};
                p.editor->text(typed[rng() % (sizeof(typed) / sizeof(typed[0]))]);
                break;
            }
            case 7: p.editor->idle(0.01 + (rng() % 30) / 100.0); break;
            case 8:
                if (rng() % 20 == 0) p.editor->focusLost();
                if (rng() % 7 == 0) p.editor->pointerLeave();
                break;
            default: p.draw(); break;
        }
        if (!p.host.clean()) break;
    }
    p.editor->focusLost();
    p.editor->idle(1.0);
    CHECK(!p.host.nested);
    CHECK(!p.host.strayEnd);
    CHECK(!p.host.valueOutsideGesture);
    CHECK_MSG(p.host.maxOpen <= 2, "%d parameters in gestures at once", p.host.maxOpen);
    CHECK_MSG(p.host.openCount == 0, "%d gestures left open", p.host.openCount);
    CHECK_MSG(!p.host.strayNoteOff && p.host.held.empty(), "%zu notes left sounding", p.host.held.size());
    CHECK(p.host.begins > 50);  // the run actually exercised something
}

SUPPLE_TEST_MAIN
