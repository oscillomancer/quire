// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/Ui.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <system_error>
#include <utility>

#include "core/Files.h"
#include "core/ParamTable.h"
#include "core/PresetLibrary.h"
#include "core/PropertyTable.h"

namespace supple::gui {
namespace {

constexpr double kKnobTravelPixels = 220.0;  // a full sweep of a knob's range
constexpr double kFine = 0.2;                // Shift: five times finer
constexpr double kWheelStep = 0.02;
constexpr double kKeyStep = 0.01;
constexpr double kPageStep = 0.1;
constexpr float kHandleReach = 11.0f;        // how near an envelope handle a press must land
constexpr float kPadInset = 8.0f;            // an XY pad's point stays this far inside its edge

bool dragWidget(Widget w) { return w == Widget::Knob || w == Widget::HSlider || w == Widget::VSlider; }

int optionCount(const ParamDef& p) {
    return static_cast<int>(std::lround(p.hostMax() - p.hostMin())) + 1;
}

const char* optionText(const ParamDef& p, int index, char* scratch, size_t size) {
    if (p.labels && index >= 0 && index < p.labelCount) return p.labels[index];
    std::snprintf(scratch, size, "%d", static_cast<int>(p.hostMin()) + index);
    return scratch;
}

// UTF-8: a continuation byte is 10xxxxxx.
bool continuation(char c) { return (static_cast<unsigned char>(c) & 0xC0) == 0x80; }

int previousChar(const char* text, int at) {
    if (at <= 0) return 0;
    --at;
    while (at > 0 && continuation(text[at])) --at;
    return at;
}

int nextChar(const char* text, int at, int length) {
    if (at >= length) return length;
    ++at;
    while (at < length && continuation(text[at])) ++at;
    return at;
}

// A typed character as the text field takes it, in UTF-8: 0 bytes when it
// is refused (a control character; anything but ASCII in a value, where a
// typographic minus becomes '-').
int acceptedChar(uint32_t codepoint, bool ascii, char bytes[4]) {
    if (ascii && codepoint == 0x2212) codepoint = '-';
    if (codepoint < 0x20 || codepoint == 0x7F || (codepoint >= 0x80 && codepoint < 0xA0)) return 0;
    if (codepoint > 0x7E && (ascii || codepoint > 0x10FFFF || (codepoint >= 0xD800 && codepoint < 0xE000)))
        return 0;
    int n = 0;
    if (codepoint < 0x80) {
        bytes[n++] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        bytes[n++] = static_cast<char>(0xC0 | codepoint >> 6);
        bytes[n++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        bytes[n++] = static_cast<char>(0xE0 | codepoint >> 12);
        bytes[n++] = static_cast<char>(0x80 | (codepoint >> 6 & 0x3F));
        bytes[n++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        bytes[n++] = static_cast<char>(0xF0 | codepoint >> 18);
        bytes[n++] = static_cast<char>(0x80 | (codepoint >> 12 & 0x3F));
        bytes[n++] = static_cast<char>(0x80 | (codepoint >> 6 & 0x3F));
        bytes[n++] = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    return n;
}

// The character at text[i], advancing i past it; a malformed byte is
// skipped and reads as 0, which acceptedChar() refuses.
uint32_t decodeChar(std::string_view text, size_t& i) {
    const auto lead = static_cast<unsigned char>(text[i++]);
    if (lead < 0x80) return lead;
    const int more = lead >= 0xF0 && lead < 0xF5 ? 3 : lead >= 0xE0 ? 2 : lead >= 0xC2 && lead < 0xE0 ? 1 : -1;
    if (more < 0 || i + static_cast<size_t>(more) > text.size()) return 0;
    uint32_t c = lead & (0x3Fu >> more);
    for (int k = 0; k < more; ++k) {
        if (!continuation(text[i])) return 0;
        c = c << 6 | (static_cast<unsigned char>(text[i++]) & 0x3Fu);
    }
    const uint32_t least = more == 1 ? 0x80 : more == 2 ? 0x800 : 0x10000;
    return c < least ? 0 : c;  // an overlong form
}

bool command(uint32_t mods) { return (mods & (mod::kCtrl | mod::kSuper)) && !(mods & mod::kAlt); }

// The longest prefix of `text` within `limit` bytes that ends on a whole
// character.
size_t wholePrefix(std::string_view text, size_t limit) {
    if (text.size() <= limit) return text.size();
    size_t n = limit;
    while (n > 0 && continuation(text[n])) --n;
    return n;
}

// A list's rows from functions: a parameter's options, the presets.
struct FunctionRows : ListSource {
    std::function<int()> count;
    std::function<std::string(int)> text;
    std::function<bool(int)> isHeader;
    std::function<int()> current;
    std::function<void(int)> pick;

    int size() const override { return count(); }
    std::string item(int row) const override { return text(row); }
    bool header(int row) const override { return isHeader && isHeader(row); }
    int selected() const override { return current(); }
    void choose(int row) override { pick(row); }
};

// The presets as list rows: each factory category's header and presets,
// then the user's.
struct PresetRow {
    std::string text;
    int entry = -1;  // -1 for a header
};
std::vector<PresetRow> presetRows(const PresetLibrary& library) {
    std::vector<PresetRow> rows;
    const auto& entries = library.entries();
    std::vector<std::string> categories;
    for (const auto& e : entries)
        if (!e.user() && std::find(categories.begin(), categories.end(), e.category) == categories.end())
            categories.push_back(e.category);
    for (const std::string& category : categories) {
        rows.push_back({category.empty() ? "Factory" : category, -1});
        for (size_t k = 0; k < entries.size(); ++k)
            if (!entries[k].user() && entries[k].category == category) rows.push_back({entries[k].name, static_cast<int>(k)});
    }
    bool user = false;
    for (size_t k = 0; k < entries.size(); ++k) {
        if (!entries[k].user()) continue;
        if (!user) rows.push_back({"User", -1});
        user = true;
        rows.push_back({entries[k].name, static_cast<int>(k)});
    }
    return rows;
}

// Those rows, built again only when the library's list has changed: a list
// asks for them for every row it draws.
struct PresetRowCache {
    const PresetLibrary* library = nullptr;
    uint64_t revision = 0;
    std::vector<PresetRow> rows;
    const std::vector<PresetRow>& of(const PresetLibrary& l) {
        if (library != &l || revision != l.revision()) {
            rows = presetRows(l);
            library = &l;
            revision = l.revision();
        }
        return rows;
    }
};

}  // namespace

Ui::Ui(EditorHost& host, float designWidth, float designHeight)
    : host_(host), designW_(designWidth), designH_(designHeight) {}

Ui::~Ui() { focusLost(); }

// --- building ------------------------------------------------------------------

Control& Ui::add(uint32_t param, Widget widget, Rect bounds) {
    Control c;
    c.param = param;
    c.widget = widget;
    c.bounds = bounds;
    if (building_ >= 0) {
        c.tabs = building_;
        c.page = buildingPage_;
    }
    controls_.push_back(std::move(c));
    dirty_ = true;
    const int i = count() - 1;
    if (widget == Widget::PresetBar) syncPresets(true);
    if (widget == Widget::List && param != kNoParam) {
        auto rows = std::make_unique<FunctionRows>();
        rows->count = [this, i] {
            const ParamDef* p = def(i);
            return p ? optionCount(*p) : 0;
        };
        rows->text = [this, i](int row) {
            char scratch[32];
            const ParamDef* p = def(i);
            return p ? std::string(optionText(*p, row, scratch, sizeof(scratch))) : std::string();
        };
        rows->current = [this, i] {
            const ParamDef* p = def(i);
            return p ? static_cast<int>(std::lround(currentValue(i) - p->hostMin())) : -1;
        };
        rows->pick = [this, i](int row) {
            if (const ParamDef* p = def(i)) oneShot(i, p->hostMin() + row);
        };
        controls_.back().source = rows.get();
        ownSources_.push_back(std::move(rows));
    }
    return controls_.back();
}

Control& Ui::presetList(Rect r) {
    auto rows = std::make_unique<FunctionRows>();
    auto cache = std::make_shared<PresetRowCache>();
    // No library, no rows.
    const auto rowsNow = [this, cache]() -> const std::vector<PresetRow>& {
        static const std::vector<PresetRow> none;
        const PresetLibrary* library = host_.presetLibrary();
        return library ? cache->of(*library) : none;
    };
    rows->count = [rowsNow] { return static_cast<int>(rowsNow().size()); };
    rows->text = [rowsNow](int row) {
        const auto& all = rowsNow();
        return row >= 0 && row < static_cast<int>(all.size()) ? all[static_cast<size_t>(row)].text : std::string();
    };
    rows->isHeader = [rowsNow](int row) {
        const auto& all = rowsNow();
        return row >= 0 && row < static_cast<int>(all.size()) && all[static_cast<size_t>(row)].entry < 0;
    };
    rows->current = [this, rowsNow] {
        const PresetLibrary* library = host_.presetLibrary();
        if (!library || library->current() < 0) return -1;
        const auto& all = rowsNow();
        for (size_t k = 0; k < all.size(); ++k)
            if (all[k].entry == library->current()) return static_cast<int>(k);
        return -1;
    };
    rows->pick = [this, rowsNow](int row) {
        PresetLibrary* library = host_.presetLibrary();
        if (!library) return;
        const auto& all = rowsNow();
        if (row >= 0 && row < static_cast<int>(all.size()) && all[static_cast<size_t>(row)].entry >= 0)
            library->load(all[static_cast<size_t>(row)].entry);
        syncPresets(true);
    };
    if (PresetLibrary* library = host_.presetLibrary()) library->refresh();
    Control& c = add(kNoParam, Widget::List, r);
    c.source = rows.get();
    ownSources_.push_back(std::move(rows));
    c.label = "Presets";
    return c;
}

Control& Ui::button(Rect r, const char* text, std::function<void()> action) {
    Control& c = add(kNoParam, Widget::Button, r);
    c.label = text;
    c.action = std::move(action);
    return c;
}

Control& Ui::xyPad(uint32_t across, uint32_t up, Rect r) {
    Control& c = add(across, Widget::XYPad, r);
    c.more[0] = up;
    return c;
}

Control& Ui::tabs(Rect r, std::vector<std::string> labels) {
    Control& c = add(kNoParam, Widget::Tabs, r);
    c.pages = std::move(labels);
    return c;
}

void Ui::onPage(int page) {
    for (int i = count() - 1; i >= 0; --i)
        if (controls_[static_cast<size_t>(i)].widget == Widget::Tabs) {
            building_ = i;
            buildingPage_ = page;
            return;
        }
}

int Ui::page(int control) const {
    return control >= 0 && control < count() ? controls_[static_cast<size_t>(control)].selected : 0;
}

void Ui::setPage(int control, int page) {
    if (control < 0 || control >= count()) return;
    Control& c = controls_[static_cast<size_t>(control)];
    if (c.widget != Widget::Tabs || c.pages.empty()) return;
    page = std::clamp(page, 0, static_cast<int>(c.pages.size()) - 1);
    if (page == c.selected) return;
    // Whatever was held on the page leaving the screen is let go.
    pointerUp(0.0f, 0.0f, 0, 0);
    endGesture();
    release();
    c.selected = page;
    cancelInactiveOverlays();
    if (focus_ >= 0 && focus_ != control && !shown(focus_)) focus_ = -1;
    hideTooltip();
    dirty_ = true;
}

Control& Ui::list(Rect r, ListSource& rows) {
    Control& c = add(kNoParam, Widget::List, r);
    c.source = &rows;
    return c;
}

Control& Ui::keyboard(Rect r, int lowKey, int highKey) {
    Control& c = add(kNoParam, Widget::Keyboard, r);
    c.lowKey = std::clamp(std::min(lowKey, highKey), 0, 127);
    c.highKey = std::clamp(std::max(lowKey, highKey), 0, 127);
    return c;
}

Control& Ui::envelope(Rect r, uint32_t attack, uint32_t decay, uint32_t sustain, uint32_t release) {
    Control& c = add(attack, Widget::Envelope, r);
    c.more[0] = decay;
    c.more[1] = sustain;
    c.more[2] = release;
    return c;
}

// --- lookup ---------------------------------------------------------------------

uint32_t Ui::paramOf(int i, int slot) const {
    if (i < 0 || i >= count()) return kNoParam;
    const Control& c = controls_[static_cast<size_t>(i)];
    return c.widget == Widget::TextField || c.widget == Widget::File ? kNoParam : c.paramAt(slot);
}

const ParamDef* Ui::def(int i, int slot) const {
    const uint32_t id = paramOf(i, slot);
    return id == kNoParam ? nullptr : params::find(id);
}

bool Ui::parameterless(int i) const {
    for (int k = 0; k < Control::kSlots; ++k)
        if (def(i, k)) return false;
    return true;
}

int Ui::slots(int i) const {
    int n = 0;
    for (int k = 0; k < Control::kSlots; ++k)
        if (def(i, k)) ++n;
    return n;
}

int Ui::find(uint32_t param) const {
    for (int i = 0; i < count(); ++i) {
        const Control& c = controls_[static_cast<size_t>(i)];
        if (c.widget == Widget::TextField || c.widget == Widget::File) continue;
        for (int k = 0; k < Control::kSlots; ++k)
            if (c.paramAt(k) == param && param != kNoParam) return i;
    }
    return -1;
}

double Ui::currentValue(int i, int slot) const { return host_.value(paramOf(i, slot)); }

bool Ui::shown(int i) const {
    if (i < 0 || i >= count()) return false;
    const Control& c = controls_[static_cast<size_t>(i)];
    if (!c.visible) return false;
    if (c.tabs < 0 || c.tabs == i) return true;
    return shown(c.tabs) && controls_[static_cast<size_t>(c.tabs)].selected == c.page;
}

ControlState Ui::state(int i) const {
    ControlState s;
    const Control& c = controls_[static_cast<size_t>(i)];
    s.hover = hover_ == i;
    s.focused = focus_ == i;
    s.editing = text_.open && text_.control == i;
    s.hoverIndex = hover_ == i ? hoverPart_ : -1;
    s.activeIndex = drag_.control == i ? drag_.handle : -1;
    for (int k = 0; k < Control::kSlots; ++k) {
        ControlState::Slot& slot = s.slots[k];
        slot.def = def(i, k);
        if (!slot.def) continue;
        const uint32_t id = c.paramAt(k);
        slot.value = host_.value(id);
        slot.normalized = toNormalized(*slot.def, slot.value);
        const double m = host_.modulation(id);
        slot.modulated = m != 0.0 ? toNormalized(*slot.def, sanitizeHost(*slot.def, slot.value + m)) : slot.normalized;
    }
    s.def = s.slots[0].def;
    s.value = s.slots[0].value;
    s.normalized = s.slots[0].normalized;
    s.modulated = s.slots[0].modulated;
    s.active = drag_.control == i || gesture_.involves(i) || (menu_.open && menu_.control == i) || s.editing;
    switch (c.widget) {
        case Widget::TextField: s.text = host_.property(c.param); break;
        case Widget::File: {
            s.text = host_.property(c.param);
            const FileStatus status = host_.fileStatus(c.param);
            s.file = status.state;
            s.detail = status.state == FileStatus::State::Failed ? status.error : status.summary;
            break;
        }
        case Widget::PresetBar:
            s.text = presetName_;
            s.modified = presetModified_;
            s.slot = presetSlot_;
            s.available = host_.presetLibrary() != nullptr;
            break;
        case Widget::Keyboard:
            s.keys[0] = keysHeld_[0];
            s.keys[1] = keysHeld_[1];
            if (playing_ >= 0 && drag_.control == i) s.keys[playing_ >> 6] |= uint64_t{1} << (playing_ & 63);
            break;
        case Widget::List: s.available = c.source != nullptr; break;
        default: break;
    }
    return s;
}

int Ui::controlAt(float x, float y) const {
    for (int i = 0; i < count(); ++i)
        if (interactive(i) && controls_[static_cast<size_t>(i)].bounds.contains(x, y)) return i;
    return -1;
}

int Ui::segmentAt(int i, float x) const {
    const ParamDef* p = def(i);
    if (!p) return -1;
    const Control& c = controls_[static_cast<size_t>(i)];
    const int n = optionCount(*p);
    const int s = static_cast<int>(std::floor((x - c.bounds.x) / (c.bounds.w / static_cast<float>(n))));
    return std::clamp(s, 0, n - 1);
}

int Ui::partAt(int i, float x, float y) const {
    if (i < 0) return -1;
    const Control& c = controls_[static_cast<size_t>(i)];
    switch (c.widget) {
        case Widget::Segmented: return segmentAt(i, x);
        case Widget::PresetBar: return presetPartAt(i, x);
        case Widget::Tabs: {
            const int n = static_cast<int>(c.pages.size());
            for (int k = 0; k < n; ++k)
                if (tab(c.bounds, k, n).contains(x, y)) return k;
            return -1;
        }
        case Widget::List: {
            // Whatever part of a row is drawn, including the last row cut
            // off by the bottom edge, takes the click.
            if (!c.source || !c.bounds.inset(2.0f, 2.0f).contains(x, y)) return -1;
            const int rows = listRows(c);
            for (int r = c.scroll; r < std::min(c.source->size(), c.scroll + rows + 1); ++r)
                if (listRow(c, r).contains(x, y)) return c.source->header(r) ? -1 : r;
            return -1;
        }
        case Widget::Keyboard: return keyAt(c, x, y);
        case Widget::Envelope: return envelopeHandleAt(i, x, y);
        default: return -1;
    }
}

bool Ui::interactive(int i) const {
    if (!shown(i)) return false;
    const Control& c = controls_[static_cast<size_t>(i)];
    if (!c.enabled) return false;
    switch (c.widget) {
        case Widget::TextField: return properties::indexOf(c.param) >= 0;
        case Widget::File: {
            const int k = properties::indexOf(c.param);
            return k >= 0 && properties::at(static_cast<size_t>(k)).isFile();
        }
        case Widget::PresetBar: return host_.presetLibrary() != nullptr;
        case Widget::Button: return static_cast<bool>(c.action);
        case Widget::Tabs: return !c.pages.empty();
        case Widget::Keyboard: return true;
        case Widget::Readout: return false;
        case Widget::List:
            if (c.param == kNoParam) return c.source != nullptr;
            break;
        default: break;
    }
    for (int k = 0; k < Control::kSlots; ++k)
        if (const ParamDef* p = def(i, k); p && !p->isOutput()) return true;
    return false;
}

bool Ui::textEditable(int i) const {
    const ParamDef* p = def(i);
    return p && dragWidget(controls_[static_cast<size_t>(i)].widget) &&
           (p->kind == ParamKind::Continuous || p->kind == ParamKind::Integer);
}

// --- geometry ------------------------------------------------------------------------

Rect Ui::tab(const Rect& bar, int k, int n) {
    const float w = bar.w / static_cast<float>(std::max(1, n));
    return {bar.x + w * static_cast<float>(k), bar.y, w, bar.h};
}

int Ui::listRows(const Control& c) { return std::max(1, static_cast<int>((c.bounds.h - 8.0f) / kListRow)); }

Rect Ui::listRow(const Control& c, int row) {
    return {c.bounds.x + 4.0f, c.bounds.y + 4.0f + kListRow * static_cast<float>(row - c.scroll), c.bounds.w - 8.0f,
            kListRow};
}

bool Ui::blackKey(int key) {
    const int n = ((key % 12) + 12) % 12;
    return n == 1 || n == 3 || n == 6 || n == 8 || n == 10;
}

namespace {
int whitesBefore(int low, int key) {
    int n = 0;
    for (int k = low; k < key; ++k)
        if (!Ui::blackKey(k)) ++n;
    return n;
}
}  // namespace

Rect Ui::keyRect(const Control& c, int key) {
    const int whites = std::max(1, whitesBefore(c.lowKey, c.highKey + 1));
    const float ww = c.bounds.w / static_cast<float>(whites);
    const float x = c.bounds.x + ww * static_cast<float>(whitesBefore(c.lowKey, key));
    if (!blackKey(key)) return {x, c.bounds.y, ww, c.bounds.h};
    const float bw = ww * 0.62f;
    return {x - bw * 0.5f, c.bounds.y, bw, c.bounds.h * 0.62f};
}

int Ui::keyAt(const Control& c, float x, float y) {
    if (!c.bounds.contains(x, y)) return -1;
    for (int k = c.lowKey; k <= c.highKey; ++k)
        if (blackKey(k) && keyRect(c, k).contains(x, y)) return k;
    for (int k = c.lowKey; k <= c.highKey; ++k)
        if (!blackKey(k) && keyRect(c, k).contains(x, y)) return k;
    return -1;
}

namespace {
struct EnvelopeArea {
    Rect area;
    float segment;  // the widest an attack, decay or release can draw
    float sustain;  // the sustain plateau's width
};
EnvelopeArea envelopeArea(const Control& c) {
    const Rect area = c.bounds.inset(10.0f, 10.0f);
    const float segment = area.w * 0.25f;
    return {area, segment, area.w - 3.0f * segment};
}
}  // namespace

Ui::EnvelopeShape Ui::envelopeShape(const Control& c, const ControlState& s) {
    const EnvelopeArea e = envelopeArea(c);
    const Rect& a = e.area;
    const bool hasA = s.slots[0].def, hasD = s.slots[1].def, hasS = s.slots[2].def, hasR = s.slots[3].def;
    const float na = hasA ? static_cast<float>(s.slots[0].normalized) : 0.0f;
    const float nd = hasD ? static_cast<float>(s.slots[1].normalized) : 0.0f;
    // No sustain: an AD envelope falls to nothing, an AR one holds the peak.
    const float ns = hasS ? static_cast<float>(s.slots[2].normalized) : hasD ? 0.0f : 1.0f;
    const float nr = hasR ? static_cast<float>(s.slots[3].normalized) : 0.0f;
    EnvelopeShape shape{};
    shape.x[0] = a.x;
    shape.y[0] = a.bottom();
    shape.x[1] = a.x + e.segment * na;
    shape.y[1] = a.y;
    shape.x[2] = shape.x[1] + e.segment * nd;
    shape.y[2] = a.bottom() - a.h * ns;
    shape.x[3] = shape.x[2] + e.sustain;
    shape.y[3] = shape.y[2];
    shape.x[4] = shape.x[3] + e.segment * nr;
    shape.y[4] = a.bottom();
    shape.has[0] = hasA;
    shape.has[1] = hasD || hasS;
    shape.has[2] = hasR;
    return shape;
}

int Ui::envelopeHandleAt(int i, float x, float y) const {
    const EnvelopeShape shape = envelopeShape(controls_[static_cast<size_t>(i)], state(i));
    int best = -1;
    float nearest = kHandleReach * kHandleReach;
    for (int h = 0; h < 3; ++h) {
        if (!shape.has[h]) continue;
        const int point = h == 2 ? 4 : h + 1;
        const float dx = x - shape.x[point], dy = y - shape.y[point];
        if (dx * dx + dy * dy <= nearest) {
            nearest = dx * dx + dy * dy;
            best = h;
        }
    }
    return best;
}

// --- gestures ------------------------------------------------------------------

void Ui::beginGesture(int c0, int s0, int c1, int s1, bool timed) {
    Gesture& g = gesture_;
    if (g.control[0] == c0 && g.slot[0] == s0 && g.control[1] == c1 && g.slot[1] == s1) {
        g.timed = timed;
        g.idle = 0.0;
        return;
    }
    endGesture();
    g.control[0] = c0;
    g.slot[0] = s0;
    g.control[1] = c1;
    g.slot[1] = s1;
    g.timed = timed;
    g.idle = 0.0;
}

void Ui::endGesture() {
    if (gesture_.control[0] < 0 && gesture_.control[1] < 0) return;
    for (int k = 0; k < 2; ++k)
        if (gesture_.open[k]) host_.endEdit(paramOf(gesture_.control[k], gesture_.slot[k]));
    gesture_ = Gesture{};
    wheelAccumulator_ = 0.0;
    wheelControl_ = -1;
    dirty_ = true;
}

// One of the open gesture's parameters to `host`: its begin goes with its
// first change.
void Ui::send(int entry, double host) {
    const int i = gesture_.control[entry];
    const int slot = gesture_.slot[entry];
    const ParamDef* p = def(i, slot);
    if (!p || p->isOutput()) return;
    const double v = sanitizeHost(*p, host);
    if (v == currentValue(i, slot)) return;
    const uint32_t id = paramOf(i, slot);
    const int other = 1 - entry;
    const bool alreadyOpen = gesture_.open[other] &&
        paramOf(gesture_.control[other], gesture_.slot[other]) == id;
    if (!gesture_.open[entry] && !alreadyOpen) {
        host_.beginEdit(id);
        gesture_.open[entry] = true;
    }
    host_.setValue(id, v);
    dirty_ = true;
}

void Ui::edit(int i, int slot, double host, bool timed) {
    const ParamDef* p = def(i, slot);
    if (!p || sanitizeHost(*p, host) == currentValue(i, slot)) return;
    beginGesture(i, slot, -1, 0, timed);
    send(0, host);
}

void Ui::oneShot(int i, int slot, double host) {
    endGesture();
    const ParamDef* p = def(i, slot);
    if (!p || p->isOutput()) return;
    const double v = sanitizeHost(*p, host);
    if (v == currentValue(i, slot)) return;
    const uint32_t id = paramOf(i, slot);
    host_.beginEdit(id);
    host_.setValue(id, v);
    host_.endEdit(id);
    dirty_ = true;
}

void Ui::resetAll(int i) {
    for (int k = 0; k < Control::kSlots; ++k)
        if (const ParamDef* p = def(i, k)) oneShot(i, k, hostDefault(*p));
}

void Ui::dragEdit(int i, int slot, double host) {
    const ParamDef* p = def(i, slot);
    if (!p || drag_.control >= 0 || sanitizeHost(*p, host) == currentValue(i, slot)) return;
    beginGesture(i, slot, -1, 0, false);
    send(0, host);
}

void Ui::dragEdit(int controlX, double hostX, int controlY, double hostY) {
    if (drag_.control >= 0 || !def(controlX) || !def(controlY)) return;
    beginGesture(controlX, 0, controlY, 0, false);
    send(0, hostX);
    send(1, hostY);
}

void Ui::step(int i, double normalDelta, int steps, int slot) {
    const ParamDef* p = def(i, slot);
    if (!p) return;
    const double now = currentValue(i, slot);
    edit(i, slot, p->stepped() ? now + steps : fromNormalized(*p, toNormalized(*p, now) + normalDelta), true);
}

// --- pointer ---------------------------------------------------------------------

bool Ui::pointerDown(float x, float y, int button, uint32_t mods, bool doubleClick) {
    pointerX_ = x;
    pointerY_ = y;
    hideTooltip();
    tooltipArmed_ = false;
    if (text_.open) {
        if (text_.bounds.contains(x, y)) {
            TextEdit& t = text_;
            t.caret = caretAt(x);
            if (doubleClick) {
                // The word under the pointer: the run between spaces.
                t.anchor = t.caret;
                while (t.anchor > 0 && t.buffer[t.anchor - 1] != ' ') --t.anchor;
                while (t.caret < t.length && t.buffer[t.caret] != ' ') ++t.caret;
            } else {
                if (!(mods & mod::kShift)) t.anchor = t.caret;
                t.selecting = button == 0;
            }
            textChanged();
        } else if (text_.commit) {
            cancelText();  // a prompt is abandoned by clicking away
        } else {
            commitText();
            if (text_.open) cancelText();  // an unparseable entry is abandoned, not kept
        }
        return true;
    }
    if (menu_.open) {
        if (!menu_.bounds.contains(x, y)) {
            closeMenu();
        } else {
            const int row = menuRowAt(x, y);
            if (selectableRow(row)) chooseMenu(row);
        }
        return true;
    }
    if (button == 1) {  // secondary: the context menu
        const int i = controlAt(x, y);
        if (i >= 0 && controls_[static_cast<size_t>(i)].widget == Widget::File && interactive(i)) {
            endGesture();
            setFocus(i);
            const uint32_t id = controls_[static_cast<size_t>(i)].param;
            const bool chosen = !host_.property(id).empty();
            popup(Rect{x, y, 0.0f, 0.0f},
                  {MenuItem::item("Choose\xE2\x80\xA6", false, files_ != nullptr), MenuItem::item("Clear", false, chosen)},
                  [this, i, id](int row) {
                      if (row == 0) chooseFile(i);
                      else host_.setProperty(id, "");
                  },
                  140.0f, i);
            return true;
        }
        if (i < 0 || parameterless(i) || controls_[static_cast<size_t>(i)].widget == Widget::List) return false;
        endGesture();
        setFocus(i);
        openContextMenu(i, x, y);
        return true;
    }
    if (button != 0) return false;

    const int i = controlAt(x, y);
    if (i < 0) {
        endGesture();
        if (focus_ >= 0) {
            focus_ = -1;
            dirty_ = true;
        }
        return false;
    }
    endGesture();
    setFocus(i);
    Control& c = controls_[static_cast<size_t>(i)];
    dirty_ = true;
    switch (c.widget) {
        case Widget::TextField: openTextEdit(i); return true;
        case Widget::File: chooseFile(i); return true;
        case Widget::PresetBar: presetAction(i, presetPartAt(i, x)); return true;
        case Widget::Button: {
            const std::function<void()> action = c.action;  // it may add controls
            action();
            return true;
        }
        case Widget::Tabs: {
            const int k = partAt(i, x, y);
            if (k >= 0) setPage(i, k);
            return true;
        }
        case Widget::List: {
            const int row = partAt(i, x, y);
            if (row >= 0) listChoose(i, row);
            return true;
        }
        case Widget::Keyboard:
            drag_ = Drag{};
            drag_.control = i;
            play(keyAt(c, x, y), y);
            drag_.handle = playing_;
            return true;
        case Widget::Momentary:
            if (const ParamDef* p = def(i)) {
                drag_ = Drag{};
                drag_.control = i;
                beginGesture(i, 0, -1, 0, false);
                send(0, p->hostMax());
            }
            return true;
        default: break;
    }
    const ParamDef* p = def(i);
    const double current = currentValue(i);

    switch (c.widget) {
        case Widget::Knob:
        case Widget::HSlider:
        case Widget::VSlider:
            if ((mods & (mod::kCtrl | mod::kSuper)) && textEditable(i)) {
                openTextEdit(i);
            } else if (doubleClick) {
                oneShot(i, hostDefault(*p));
            } else {
                drag_ = Drag{};
                drag_.control = i;
                drag_.anchorX = x;
                drag_.anchorY = y;
                drag_.anchorNormal = drag_.normal = toNormalized(*p, current);
                drag_.fine = (mods & mod::kShift) != 0;
            }
            break;
        case Widget::XYPad:
            if (doubleClick) {
                resetAll(i);
                break;
            }
            drag_ = Drag{};
            drag_.control = i;
            drag_.anchorX = x;
            drag_.anchorY = y;
            drag_.fine = (mods & mod::kShift) != 0;
            {
                const ControlState s = state(i);
                drag_.anchorNormal = drag_.normal = s.slots[0].normalized;
                drag_.anchorNormal2 = drag_.normal2 = s.slots[1].normalized;
            }
            if (!drag_.fine) xyDrag(i, x, y);  // the point jumps to the press
            break;
        case Widget::Envelope: {
            const int h = envelopeHandleAt(i, x, y);
            if (h < 0) break;
            if (doubleClick) {
                // The handle's own parameters, back to their defaults.
                for (int k : {h == 0 ? 0 : h == 1 ? 1 : 3, h == 1 ? 2 : -1})
                    if (const ParamDef* d = k >= 0 ? def(i, k) : nullptr) oneShot(i, k, hostDefault(*d));
                break;
            }
            drag_ = Drag{};
            drag_.control = i;
            drag_.handle = h;
            break;
        }
        case Widget::Toggle: oneShot(i, current + 1.0 > p->hostMax() ? p->hostMin() : current + 1.0); break;
        case Widget::Segmented: oneShot(i, p->hostMin() + segmentAt(i, x)); break;
        case Widget::Dropdown: openMenu(i); break;
        default: break;  // a readout is never interactive
    }
    return true;
}

void Ui::xyDrag(int i, float x, float y) {
    const Control& c = controls_[static_cast<size_t>(i)];
    const ParamDef* px = def(i, 0);
    const ParamDef* py = def(i, 1);
    const Rect area = c.bounds.inset(kPadInset);
    double nx, ny;
    if (drag_.fine || drag_.relative) {
        const double speed = drag_.fine ? kFine : 1.0;
        nx = drag_.anchorNormal + (x - drag_.anchorX) / std::max(1.0f, area.w) * speed;
        ny = drag_.anchorNormal2 - (y - drag_.anchorY) / std::max(1.0f, area.h) * speed;
    } else {
        nx = (x - area.x) / std::max(1.0f, area.w);
        ny = 1.0 - (y - area.y) / std::max(1.0f, area.h);
    }
    nx = std::clamp(nx, 0.0, 1.0);
    ny = std::clamp(ny, 0.0, 1.0);
    // Only an axis that moved is sent: a pad dragged sideways leaves the
    // vertical parameter (and its automation) alone.
    const bool movedX = nx != drag_.normal || !drag_.fine, movedY = ny != drag_.normal2 || !drag_.fine;
    drag_.normal = nx;
    drag_.normal2 = ny;
    beginGesture(i, 0, py ? i : -1, 1, false);
    if (px && movedX) send(0, fromNormalized(*px, nx));
    if (py && movedY) send(1, fromNormalized(*py, ny));
}

void Ui::envelopeDrag(int i, float x, float y) {
    const Control& c = controls_[static_cast<size_t>(i)];
    const EnvelopeArea e = envelopeArea(c);
    const EnvelopeShape shape = envelopeShape(c, state(i));
    const float segment = std::max(1.0f, e.segment);
    auto to = [&](int slot, double normal) {
        const ParamDef* p = def(i, slot);
        return p ? fromNormalized(*p, std::clamp(normal, 0.0, 1.0)) : 0.0;
    };
    switch (drag_.handle) {
        case 0:
            beginGesture(i, 0, -1, 0, false);
            send(0, to(0, (x - shape.x[0]) / segment));
            break;
        case 1: {
            const bool d = def(i, 1) != nullptr, s = def(i, 2) != nullptr;
            beginGesture(d ? i : -1, 1, s ? i : -1, 2, false);
            if (d) send(0, to(1, (x - shape.x[1]) / segment));
            if (s) send(1, to(2, (e.area.bottom() - y) / std::max(1.0f, e.area.h)));
            break;
        }
        case 2:
            beginGesture(i, 3, -1, 0, false);
            send(0, to(3, (x - shape.x[3]) / segment));
            break;
        default: break;
    }
}

bool Ui::pointerMove(float x, float y, uint32_t mods) {
    pointerX_ = x;
    pointerY_ = y;
    if (text_.open && text_.selecting) {
        const int at = caretAt(x);
        if (at != text_.caret) {
            text_.caret = at;
            textChanged();
        }
        return true;
    }
    if (drag_.control >= 0) {
        const int i = drag_.control;
        const Control& c = controls_[static_cast<size_t>(i)];
        const bool fine = (mods & mod::kShift) != 0;
        switch (c.widget) {
            case Widget::Keyboard: {
                const int k = keyAt(c, x, y);
                if (k != playing_) {
                    play(k, y);
                    drag_.handle = playing_;
                }
                return true;
            }
            case Widget::Momentary: return true;
            case Widget::Envelope: envelopeDrag(i, x, y); return true;
            case Widget::XYPad:
                if (fine != drag_.fine) {  // re-anchor: switching speed must not jump
                    drag_.anchorX = x;
                    drag_.anchorY = y;
                    drag_.anchorNormal = drag_.normal;
                    drag_.anchorNormal2 = drag_.normal2;
                    drag_.fine = fine;
                    drag_.relative = true;
                }
                xyDrag(i, x, y);
                return true;
            default: break;
        }
        const ParamDef& p = *def(i);
        if (fine != drag_.fine) {  // re-anchor: switching speed must not jump
            drag_.anchorX = x;
            drag_.anchorY = y;
            drag_.anchorNormal = drag_.normal;
            drag_.fine = fine;
        }
        double travel = kKnobTravelPixels, delta = drag_.anchorY - y;
        if (c.widget == Widget::HSlider) {
            travel = std::max(40.0f, c.bounds.w);
            delta = x - drag_.anchorX;
        } else if (c.widget == Widget::VSlider) {
            travel = std::max(40.0f, c.bounds.h);
        }
        double normal = drag_.anchorNormal + delta / travel * (fine ? kFine : 1.0);
        if (normal < 0.0 || normal > 1.0) {
            // Pinned at a limit: re-anchor so reversing responds at once.
            normal = std::clamp(normal, 0.0, 1.0);
            drag_.anchorX = x;
            drag_.anchorY = y;
            drag_.anchorNormal = normal;
        }
        drag_.normal = normal;
        const double v = fromNormalized(p, normal);
        if (v != currentValue(i)) {
            beginGesture(i, 0, -1, 0, false);
            send(0, v);
        }
        return true;
    }
    if (menu_.open) {
        const int row = menuRowAt(x, y);
        if (selectableRow(row) && row != menu_.hover) {
            menu_.hover = row;
            dirty_ = true;
        }
        return true;
    }
    const int h = controlAt(x, y);
    const int part = partAt(h, x, y);
    if (h != hover_) {
        hideTooltip();
        const Control* c = h >= 0 ? &controls_[static_cast<size_t>(h)] : nullptr;
        const ParamDef* p = h >= 0 ? def(h) : nullptr;
        tooltipArmed_ = c && (c->tooltip || c->widget == Widget::File || (p && p->description));
        tooltipWait_ = 0.0;
    }
    if (h != hover_ || part != hoverPart_) {
        hover_ = h;
        hoverPart_ = part;
        dirty_ = true;
        return true;
    }
    return false;
}

bool Ui::pointerUp(float, float, int button, uint32_t) {
    if (button == 0 && text_.selecting) {
        text_.selecting = false;
        return true;
    }
    if (button != 0 || drag_.control < 0) return false;
    const int i = drag_.control;
    const Widget w = controls_[static_cast<size_t>(i)].widget;
    if (w == Widget::Keyboard) {
        release();
    } else if (w == Widget::Momentary) {
        if (const ParamDef* p = def(i)) {
            beginGesture(i, 0, -1, 0, false);
            send(0, p->hostMin());
        }
        endGesture();
    } else if (gesture_.involves(i)) {
        endGesture();
    }
    drag_ = Drag{};
    dirty_ = true;
    return true;
}

void Ui::pointerLeave() {
    // A drag survives leaving the window: every platform keeps delivering
    // motion and the release to the view that took the press.
    if (drag_.control >= 0) return;
    if (hover_ >= 0 || hoverPart_ >= 0) dirty_ = true;
    hover_ = -1;
    hoverPart_ = -1;
    hideTooltip();
    tooltipArmed_ = false;
    pointerX_ = pointerY_ = -1.0f;
}

bool Ui::wheel(float x, float y, float dx, float dy, uint32_t mods) {
    hideTooltip();
    tooltipArmed_ = false;
    if (drag_.control >= 0 || text_.open) return true;
    // macOS turns Shift+wheel into horizontal scrolling.
    const double d = dy != 0.0f ? dy : dx;
    if (menu_.open) {
        if (d != 0.0 && menu_.visible < menu_.count()) {
            menu_.first = std::clamp(menu_.first + (d > 0.0 ? -1 : 1), 0, menu_.count() - menu_.visible);
            const int row = menuRowAt(x, y);
            menu_.hover = selectableRow(row) ? row : -1;
            dirty_ = true;
        }
        return true;
    }
    const int i = controlAt(x, y);
    if (i < 0 || d == 0.0) return false;
    Control& c = controls_[static_cast<size_t>(i)];
    if (c.widget == Widget::List) {
        if (wheelControl_ != i) {
            endGesture();
            wheelControl_ = i;
            wheelAccumulator_ = 0.0;
        }
        wheelAccumulator_ += d;
        const double rows = std::trunc(wheelAccumulator_);
        wheelAccumulator_ -= rows;
        const int most = std::max(0, (c.source ? c.source->size() : 0) - listRows(c));
        const int scroll = std::clamp(c.scroll - static_cast<int>(rows), 0, most);
        if (scroll != c.scroll) {
            c.scroll = scroll;
            hoverPart_ = partAt(i, x, y);
            dirty_ = true;
        }
        return true;
    }
    // A pad's wheel moves it up and down.
    const int slot = c.widget == Widget::XYPad ? 1 : 0;
    const ParamDef* pp = def(i, slot);
    if (!pp || c.widget == Widget::Toggle || c.widget == Widget::Momentary || c.widget == Widget::Envelope)
        return false;
    const ParamDef& p = *pp;
    if (p.stepped()) {
        if (wheelControl_ != i) {
            wheelControl_ = i;
            wheelAccumulator_ = 0.0;
        }
        wheelAccumulator_ += d;
        const double steps = std::trunc(wheelAccumulator_);
        if (steps == 0.0) return true;
        wheelAccumulator_ -= steps;
        edit(i, slot, currentValue(i, slot) + steps, true);
        wheelControl_ = i;  // edit may have closed an older gesture
        return true;
    }
    const double scale = (mods & mod::kShift) ? kWheelStep * kFine : kWheelStep;
    edit(i, slot, fromNormalized(p, toNormalized(p, currentValue(i, slot)) + d * scale), true);
    return true;
}

// --- keyboard --------------------------------------------------------------------

bool Ui::key(uint32_t code, uint32_t mods) {
    hideTooltip();
    tooltipArmed_ = false;
    if (text_.open) return textKey(code, mods);
    if (menu_.open) return menuKey(code);
    if (drag_.control >= 0) return true;
    const bool shift = (mods & mod::kShift) != 0;

    if (code == key::kTab) {
        endGesture();
        moveFocus(shift ? -1 : 1);
        return true;
    }
    if (code == key::kEscape) {
        if (focus_ < 0) return false;
        endGesture();
        focus_ = -1;
        dirty_ = true;
        return true;
    }
    if (focus_ < 0 || !interactive(focus_)) return false;
    const int i = focus_;
    Control& c = controls_[static_cast<size_t>(i)];
    const bool enter = code == key::kEnter || code == key::kPadEnter;
    switch (c.widget) {
        case Widget::TextField:
            if (enter) openTextEdit(i);
            return enter;
        case Widget::File:
            if (enter) chooseFile(i);
            else if (code == key::kDelete || code == key::kBackspace) host_.setProperty(c.param, "");
            else return false;
            return true;
        case Widget::PresetBar:
            if (enter) presetAction(i, presetbar::kName);
            else if (code == key::kLeft || code == key::kUp) presetAction(i, presetbar::kPrevious);
            else if (code == key::kRight || code == key::kDown) presetAction(i, presetbar::kNext);
            else return false;
            return true;
        case Widget::Button:
            if (enter) {
                const std::function<void()> action = c.action;
                action();
            }
            return enter;
        case Widget::Tabs: {
            const int n = static_cast<int>(c.pages.size());
            if (code == key::kLeft || code == key::kUp) setPage(i, c.selected - 1);
            else if (code == key::kRight || code == key::kDown) setPage(i, c.selected + 1);
            else if (code == key::kHome) setPage(i, 0);
            else if (code == key::kEnd) setPage(i, n - 1);
            else return false;
            return true;
        }
        case Widget::List:
            if (code == key::kUp || code == key::kLeft) listStep(i, -1);
            else if (code == key::kDown || code == key::kRight) listStep(i, 1);
            else if (code == key::kHome || code == key::kEnd) {
                const int n = c.source ? c.source->size() : 0;
                const bool home = code == key::kHome;
                for (int row = home ? 0 : n - 1; row >= 0 && row < n; row += home ? 1 : -1)
                    if (!c.source->header(row)) {
                        listChoose(i, row);
                        break;
                    }
            } else return false;
            return true;
        case Widget::Keyboard:
        case Widget::Momentary: return false;
        case Widget::Envelope:
            if (code != key::kBackspace && code != key::kDelete) return false;
            resetAll(i);
            return true;
        case Widget::XYPad: {
            const double fineStep = shift ? kKeyStep * kFine : kKeyStep;
            switch (code) {
                case key::kRight: step(i, fineStep, 1, 0); return true;
                case key::kLeft: step(i, -fineStep, -1, 0); return true;
                case key::kUp: step(i, fineStep, 1, 1); return true;
                case key::kDown: step(i, -fineStep, -1, 1); return true;
                case key::kBackspace:
                case key::kDelete: resetAll(i); return true;
                default: return false;
            }
        }
        default: break;
    }
    const ParamDef* pp = def(i);
    if (!pp) return false;
    const ParamDef& p = *pp;
    const Widget w = c.widget;
    const int pageSteps = std::max(1, optionCount(p) / 10);
    const double current = currentValue(i);

    switch (code) {
        case key::kEnter:
        case key::kPadEnter:
            if (w == Widget::Dropdown) openMenu(i);
            else if (w == Widget::Toggle || w == Widget::Segmented)
                oneShot(i, current + 1.0 > p.hostMax() ? p.hostMin() : current + 1.0);
            else if (textEditable(i)) openTextEdit(i);
            return true;
        case key::kUp:
        case key::kRight: step(i, shift ? kKeyStep * kFine : kKeyStep, 1); return true;
        case key::kDown:
        case key::kLeft: step(i, -(shift ? kKeyStep * kFine : kKeyStep), -1); return true;
        case key::kPageUp: step(i, kPageStep, pageSteps); return true;
        case key::kPageDown: step(i, -kPageStep, -pageSteps); return true;
        case key::kHome: timedEdit(i, p.hostMin()); return true;
        case key::kEnd: timedEdit(i, p.hostMax()); return true;
        case key::kBackspace:
        case key::kDelete: oneShot(i, hostDefault(p)); return true;
        default: return false;
    }
}

void Ui::moveFocus(int direction) {
    const int n = count();
    if (n == 0) return;
    int i = focus_ < 0 ? (direction > 0 ? -1 : n) : focus_;
    for (int k = 0; k < n; ++k) {
        i = ((i + direction) % n + n) % n;
        // A keyboard is played with the pointer; Tab passes it by.
        if (interactive(i) && controls_[static_cast<size_t>(i)].widget != Widget::Keyboard) {
            setFocus(i);
            return;
        }
    }
}

void Ui::setFocus(int control) {
    if (control != focus_) dirty_ = true;
    focus_ = control;
    if (control >= 0 && controls_[static_cast<size_t>(control)].widget == Widget::List) listReveal(control);
}

bool Ui::text(uint32_t codepoint) {
    if (!text_.open) return false;
    char bytes[4];
    const int n = acceptedChar(codepoint, text_.ascii, bytes);
    if (n > 0) insertText(std::string_view(bytes, static_cast<size_t>(n)));
    return true;
}

// Whole characters of `bytes` (valid UTF-8) in place of the selection, as
// many as fit.
void Ui::insertText(std::string_view bytes) {
    TextEdit& t = text_;
    eraseSelection();
    const int room = std::min(t.limit, TextEdit::kCapacity - 1) - t.length;
    const int n = static_cast<int>(wholePrefix(bytes, static_cast<size_t>(std::max(0, room))));
    std::memmove(t.buffer + t.caret + n, t.buffer + t.caret, static_cast<size_t>(t.length - t.caret));
    std::memcpy(t.buffer + t.caret, bytes.data(), static_cast<size_t>(n));
    t.caret += n;
    t.length += n;
    t.buffer[t.length] = '\0';
    t.anchor = t.caret;
    textChanged();
}

void Ui::eraseSelection() {
    TextEdit& t = text_;
    if (!t.selected()) return;
    const int from = t.from(), to = t.to();
    std::memmove(t.buffer + from, t.buffer + to, static_cast<size_t>(t.length - to));
    t.length -= to - from;
    t.buffer[t.length] = '\0';
    t.caret = t.anchor = from;
}

// Clipboard text, as if typed: line breaks and other control characters
// go, and a value keeps only what it would take from the keyboard.
void Ui::pasteText(std::string_view text) {
    std::string accepted;
    for (size_t i = 0; i < text.size();) {
        char bytes[4];
        const int n = acceptedChar(decodeChar(text, i), text_.ascii, bytes);
        accepted.append(bytes, static_cast<size_t>(n));
    }
    insertText(accepted);
}

float Ui::measureText(int bytes) const {
    const std::string_view s(text_.buffer, static_cast<size_t>(bytes));
    if (textWidth_) return textWidth_(s, text_.ascii);
    float width = 0.0f;
    for (char c : s) width += continuation(c) ? 0.0f : 7.0f;
    return width;
}

float Ui::textStart(const TextEdit& e, float width) {
    constexpr float kPad = 8.0f;
    return e.ascii ? e.bounds.cx() - width * 0.5f : e.bounds.x + kPad - e.scroll;
}

// The character boundary nearest x.
int Ui::caretAt(float x) const {
    const TextEdit& t = text_;
    const float start = textStart(t, measureText(t.length));
    int best = 0;
    float nearest = std::fabs(x - start);
    for (int at = 0; at < t.length;) {
        at = nextChar(t.buffer, at, t.length);
        const float d = std::fabs(x - (start + measureText(at)));
        if (d >= nearest) break;  // widths only grow: it is only further from here
        nearest = d;
        best = at;
    }
    return best;
}

// Text slides so the caret stays inside the field, and no further.
void Ui::revealCaret() {
    TextEdit& t = text_;
    if (t.ascii) {
        t.scroll = 0.0f;
        return;
    }
    const float inner = t.bounds.w - 16.0f;
    const float caret = measureText(t.caret);
    t.scroll = std::min(std::max(t.scroll, caret - inner), caret);
    t.scroll = std::max(0.0f, std::min(t.scroll, measureText(t.length) - inner));
}

void Ui::textChanged() {
    revealCaret();
    text_.rejected = false;
    text_.caretOn = true;
    text_.blink = 0.0;
    dirty_ = true;
}

bool Ui::textKey(uint32_t code, uint32_t mods) {
    TextEdit& t = text_;
    const bool shift = (mods & mod::kShift) != 0;
    if (command(mods)) {
        // pugl gives a letter key as its unshifted character.
        const std::string_view selection(t.buffer + t.from(), static_cast<size_t>(t.to() - t.from()));
        switch (code) {
            case 'a':
                t.anchor = 0;
                t.caret = t.length;
                break;
            case 'c':
                if (clipboard_ && t.selected()) clipboard_->copy(selection);
                return true;
            case 'x':
                if (!clipboard_ || !t.selected() || !clipboard_->copy(selection)) return true;
                eraseSelection();
                break;
            case 'v':
                if (clipboard_) {
                    const unsigned serial = textSerial_;
                    clipboard_->paste([this, serial](std::string_view text) {
                        if (text_.open && textSerial_ == serial) pasteText(text);
                    });
                }
                return true;
            default: break;
        }
        if (code == 'a' || code == 'x') {
            textChanged();
            return true;
        }
    }
    // Moving the caret without Shift drops the selection, from its own end.
    const auto move = [&](int to) {
        t.caret = to;
        if (!shift) t.anchor = t.caret;
    };
    switch (code) {
        case key::kEnter:
        case key::kPadEnter: commitText(); return true;
        case key::kEscape: cancelText(); return true;
        case key::kTab:
            commitText();
            if (t.open) cancelText();
            moveFocus(shift ? -1 : 1);
            return true;
        case key::kBackspace:
        case key::kDelete:
            if (!t.selected()) {
                if (code == key::kBackspace) t.anchor = previousChar(t.buffer, t.caret);
                else t.anchor = nextChar(t.buffer, t.caret, t.length);
            }
            eraseSelection();
            break;
        case key::kLeft:
            move(t.selected() && !shift ? t.from() : previousChar(t.buffer, t.caret));
            break;
        case key::kRight:
            move(t.selected() && !shift ? t.to() : nextChar(t.buffer, t.caret, t.length));
            break;
        case key::kHome:
        case key::kUp: move(0); break;
        case key::kEnd:
        case key::kDown: move(t.length); break;
        default: return true;  // printable keys arrive through text()
    }
    textChanged();
    return true;
}

bool Ui::menuKey(uint32_t code) {
    switch (code) {
        case key::kUp: moveMenuHover(-1); break;
        case key::kDown: moveMenuHover(1); break;
        case key::kHome:
            menu_.hover = -1;
            moveMenuHover(1);
            break;
        case key::kEnd:
            menu_.hover = menu_.count();
            moveMenuHover(-1);
            break;
        case key::kEnter:
        case key::kPadEnter:
            if (selectableRow(menu_.hover)) chooseMenu(menu_.hover);
            return true;
        case key::kEscape:
        case key::kTab: closeMenu(); return true;
        default: return true;
    }
    dirty_ = true;
    return true;
}

bool Ui::selectableRow(int row) const {
    return row >= 0 && row < menu_.count() && menu_.items[static_cast<size_t>(row)].selectable();
}

void Ui::moveMenuHover(int direction) {
    for (int row = menu_.hover + direction; row >= 0 && row < menu_.count(); row += direction) {
        if (selectableRow(row)) {
            menu_.hover = row;
            revealMenuHover();
            return;
        }
    }
}

void Ui::revealMenuHover() {
    if (menu_.hover < 0) return;
    if (menu_.hover < menu_.first) menu_.first = menu_.hover;
    if (menu_.hover >= menu_.first + menu_.visible) menu_.first = menu_.hover - menu_.visible + 1;
}

int Ui::menuRowAt(float x, float y) const {
    if (!menu_.open || !menu_.bounds.contains(x, y)) return -1;
    const int row = menu_.first + static_cast<int>(std::floor((y - menu_.bounds.y - 4.0f) / menu_.rowHeight));
    return row >= menu_.first && row < menu_.first + menu_.visible && row < menu_.count() ? row : -1;
}

void Ui::chooseMenu(int row) {
    std::function<void(int)> chosen = std::move(menu_.chosen);
    closeMenu();
    if (chosen) chosen(row);
}

// --- lists and the keyboard -------------------------------------------------------------

void Ui::listChoose(int i, int row) {
    Control& c = controls_[static_cast<size_t>(i)];
    if (!c.source || row < 0 || row >= c.source->size() || c.source->header(row)) return;
    c.source->choose(row);
    listReveal(i);
    dirty_ = true;
}

void Ui::listReveal(int i) {
    Control& c = controls_[static_cast<size_t>(i)];
    if (!c.source) return;
    const int rows = listRows(c), n = c.source->size(), selected = c.source->selected();
    if (selected >= 0) {
        if (selected < c.scroll) c.scroll = selected;
        if (selected >= c.scroll + rows) c.scroll = selected - rows + 1;
        // A header just above the first row shown comes into view with it.
        if (c.scroll > 0 && c.scroll == selected && c.source->header(c.scroll - 1)) --c.scroll;
    }
    c.scroll = std::clamp(c.scroll, 0, std::max(0, n - rows));
}

void Ui::listStep(int i, int direction) {
    Control& c = controls_[static_cast<size_t>(i)];
    if (!c.source) return;
    const int n = c.source->size();
    int from = c.source->selected();
    if (from < 0) from = direction > 0 ? -1 : n;
    for (int row = from + direction; row >= 0 && row < n; row += direction)
        if (!c.source->header(row)) {
            listChoose(i, row);
            return;
        }
}

void Ui::play(int key, float y) {
    if (key == playing_) return;
    release();
    if (key < 0 || drag_.control < 0) return;
    // Softer at the top of a key, harder towards the player, as on a piano.
    const Rect r = keyRect(controls_[static_cast<size_t>(drag_.control)], key);
    const double velocity = std::clamp(0.25 + 0.75 * (y - r.y) / std::max(1.0f, r.h), 0.05, 1.0);
    host_.playNote(key, velocity, true);
    playing_ = key;
    dirty_ = true;
}

void Ui::release() {
    if (playing_ < 0) return;
    host_.playNote(playing_, 0.0, false);
    playing_ = -1;
    dirty_ = true;
}

void Ui::setKeysHeld(const uint64_t keys[2]) {
    if (keys[0] == keysHeld_[0] && keys[1] == keysHeld_[1]) return;
    keysHeld_[0] = keys[0];
    keysHeld_[1] = keys[1];
    for (int i = 0; i < count(); ++i)
        if (controls_[static_cast<size_t>(i)].widget == Widget::Keyboard && shown(i)) dirty_ = true;
}

// --- overlays ----------------------------------------------------------------------

void Ui::openMenu(int i) {
    const ParamDef* p = def(i);
    if (!p) return;
    const int n = optionCount(*p);
    const int current = static_cast<int>(std::lround(currentValue(i) - p->hostMin()));
    std::vector<MenuItem> items;
    items.reserve(static_cast<size_t>(n));
    for (int k = 0; k < n; ++k) {
        char scratch[32];
        items.push_back(MenuItem::item(optionText(*p, k, scratch, sizeof(scratch)), k == current));
    }
    const double base = p->hostMin();
    const Control& c = controls_[static_cast<size_t>(i)];
    popup(c.bounds, std::move(items), [this, i, base](int row) { oneShot(i, base + row); },
          std::max(c.bounds.w, 120.0f), i);
}

void Ui::popup(Rect anchor, std::vector<MenuItem> items, std::function<void(int)> chosen, float minWidth,
               int control) {
    endGesture();
    cancelText();
    hideTooltip();
    menu_ = Menu{};
    menu_.open = true;
    menu_.control = control;
    menu_.items = std::move(items);
    menu_.chosen = std::move(chosen);
    const int n = menu_.count();
    const int fit = std::max(1, static_cast<int>((designH_ - 16.0f) / menu_.rowHeight));
    menu_.visible = std::min(n, fit);
    const float height = menu_.rowHeight * static_cast<float>(menu_.visible) + 8.0f;
    const float width = std::min(std::max(anchor.w, minWidth), designW_ - 8.0f);
    float y = anchor.bottom() + 4.0f;
    if (y + height > designH_ - 4.0f) y = anchor.y - 4.0f - height;
    y = std::clamp(y, 4.0f, std::max(4.0f, designH_ - 4.0f - height));
    const float x = std::clamp(anchor.x, 4.0f, std::max(4.0f, designW_ - width - 4.0f));
    menu_.bounds = {x, y, width, height};
    menu_.hover = -1;
    for (int r = 0; r < n && menu_.hover < 0; ++r)
        if (selectableRow(r) && menu_.items[static_cast<size_t>(r)].checked) menu_.hover = r;
    revealMenuHover();
    if (control >= 0) focus_ = control;
    dirty_ = true;
}

void Ui::openContextMenu(int i, float x, float y) {
    // Each of the control's parameters: one, or a pad's two, an envelope's four.
    std::vector<int> owned;
    for (int k = 0; k < Control::kSlots; ++k)
        if (const ParamDef* p = def(i, k); p && !p->isOutput()) owned.push_back(k);
    if (owned.empty()) return;
    const bool several = owned.size() > 1;
    // A host that can show its own menu does, with ours in it: for a
    // control with one parameter, since it can show only one's.
    if (!several && host_.hostPopup(paramOf(i, owned[0]), x, y)) return;

    // Each row's action: the framework's own, or one of the host's.
    struct Action {
        enum Kind : uint8_t { None, Reset, Type, Host } kind = None;
        int slot = 0;
        uint32_t hostAction = 0;
    };
    std::vector<MenuItem> items;
    std::vector<Action> actions;
    for (int k : owned) {
        const ParamDef& p = *def(i, k);
        if (several) {
            if (!items.empty()) {
                items.push_back(MenuItem::separator());
                actions.push_back({});
            }
            items.push_back(MenuItem::header(p.name));
            actions.push_back({});
        }
        char text[64];
        formatValue(p, hostDefault(p), text, sizeof(text));
        items.push_back(MenuItem::item(std::string("Reset to ") + text, false, currentValue(i, k) != hostDefault(p)));
        actions.push_back({Action::Reset, k, 0});
        if (!several && textEditable(i)) {
            items.push_back(MenuItem::item("Enter Value\xE2\x80\xA6"));
            actions.push_back({Action::Type, k, 0});
        }
        HostMenu fromHost;
        if (!host_.hostMenu(paramOf(i, k), fromHost)) continue;
        bool separated = false;
        for (size_t h = 0; h < fromHost.items.size(); ++h) {
            const MenuItem& item = fromHost.items[h];
            // No separator leads, follows another, or ends the menu.
            if (item.kind == MenuItem::Kind::Separator &&
                (!separated || items.back().kind == MenuItem::Kind::Separator))
                continue;
            if (!separated) {
                items.push_back(MenuItem::separator());
                actions.push_back({});
                separated = true;
            }
            items.push_back(item);
            const bool performable = item.kind == MenuItem::Kind::Item && h < fromHost.actions.size();
            actions.push_back(performable ? Action{Action::Host, k, fromHost.actions[h]} : Action{});
        }
        if (items.back().kind == MenuItem::Kind::Separator) {
            items.pop_back();
            actions.pop_back();
        }
    }
    popup(
        Rect{x, y, 0.0f, 0.0f}, std::move(items),
        [this, i, actions](int row) {
            const Action& a = actions[static_cast<size_t>(row)];
            if (a.kind == Action::Reset) {
                if (const ParamDef* d = def(i, a.slot)) oneShot(i, a.slot, hostDefault(*d));
            } else if (a.kind == Action::Type) {
                openTextEdit(i);
            } else if (a.kind == Action::Host) {
                host_.performHostMenu(paramOf(i, a.slot), a.hostAction);
            }
        },
        180.0f, i);
    // Nothing is preselected: a checked host item is a state, not a choice.
    menu_.hover = -1;
}

void Ui::closeMenu() {
    if (!menu_.open) return;
    menu_.open = false;
    menu_.chosen = nullptr;
    dirty_ = true;
}

Rect Ui::valueBox(const Control& c) {
    const float w = std::clamp(c.bounds.w * 0.8f, 72.0f, 120.0f);
    return {c.bounds.cx() - w * 0.5f, c.bounds.cy() - 12.0f, w, 24.0f};
}

void Ui::openTextEdit(int i) {
    if (i < 0 || i >= count()) return;
    const Control& c = controls_[static_cast<size_t>(i)];
    if (c.widget == Widget::TextField) {
        const int k = properties::indexOf(c.param);
        if (k < 0) return;
        const uint32_t id = c.param;
        const int limit = static_cast<int>(std::min<uint32_t>(properties::at(static_cast<size_t>(k)).limit,
                                                              TextEdit::kCapacity - 1));
        prompt(c.bounds, host_.property(id), limit,
               [this, id](std::string_view text) { return host_.setProperty(id, text); }, i);
        return;
    }
    const ParamDef* p = def(i);
    if (!p) return;
    endGesture();
    closeMenu();
    hideTooltip();
    text_ = TextEdit{};
    text_.open = true;
    text_.control = i;
    text_.bounds = valueBox(c);
    text_.limit = 47;
    formatValue(*p, currentValue(i), text_.buffer, static_cast<size_t>(text_.limit) + 1);
    text_.length = text_.caret = static_cast<int>(std::strlen(text_.buffer));
    text_.anchor = 0;  // all of it: typing replaces it
    ++textSerial_;
    focus_ = i;
    textChanged();
}

void Ui::prompt(Rect box, std::string_view initial, int limit, std::function<bool(std::string_view)> commit,
                int control) {
    endGesture();
    closeMenu();
    hideTooltip();
    text_ = TextEdit{};
    text_.open = true;
    text_.control = control;
    text_.bounds = box;
    text_.ascii = false;
    text_.limit = std::clamp(limit, 1, TextEdit::kCapacity - 1);
    const size_t n = wholePrefix(initial, static_cast<size_t>(text_.limit));
    std::memcpy(text_.buffer, initial.data(), n);
    text_.buffer[n] = '\0';
    text_.length = text_.caret = static_cast<int>(n);
    text_.anchor = 0;
    text_.commit = std::move(commit);
    ++textSerial_;
    if (control >= 0) focus_ = control;
    textChanged();
}

void Ui::commitText() {
    if (!text_.open) return;
    if (text_.commit) {
        // The prompt closes first: what it runs may open the next one (a
        // confirmation), which must survive.
        TextEdit was = text_;
        text_.open = false;
        text_.selecting = false;
        text_.commit = nullptr;
        dirty_ = true;
        if (!was.commit(std::string_view(was.buffer, static_cast<size_t>(was.length))) && !text_.open &&
            !menu_.open) {
            text_ = std::move(was);
            text_.rejected = true;
        }
        return;
    }
    const int i = text_.control;
    const ParamDef* p = def(i);
    double v = 0.0;
    if (!p || !parseValue(*p, text_.buffer, v)) {
        text_.rejected = true;  // stay open so the entry can be fixed
        dirty_ = true;
        return;
    }
    text_.open = false;
    text_.selecting = false;
    oneShot(i, v);
    dirty_ = true;
}

void Ui::cancelText() {
    if (!text_.open) return;
    text_.open = false;
    text_.selecting = false;
    text_.commit = nullptr;
    dirty_ = true;
}

void Ui::hideTooltip() {
    if (!tooltip_.open) return;
    tooltip_.open = false;
    dirty_ = true;
}

void Ui::openTooltip() {
    const int i = hover_;
    if (i < 0) return;
    const Control& c = controls_[static_cast<size_t>(i)];
    if (c.widget == Widget::File && !c.tooltip) {
        // The whole path, which the slot shortens to its name.
        const std::string_view path = host_.property(c.param);
        if (path.empty()) return;
        tooltip_.open = true;
        tooltip_.control = i;
        tooltip_.anchor = c.bounds;
        tooltip_.title = "";
        tooltip_.text = std::string(path);
        dirty_ = true;
        return;
    }
    const ParamDef* p = def(i);
    const char* text = c.tooltip ? c.tooltip : p ? p->description : nullptr;
    if (!text) return;
    tooltip_.open = true;
    tooltip_.control = i;
    tooltip_.anchor = c.bounds;
    tooltip_.title = p && !c.tooltip ? p->name : "";
    tooltip_.text = text;
    dirty_ = true;
}

// --- file slots ---------------------------------------------------------------------

namespace {
const PropertyDef* fileProperty(uint32_t id) {
    const int k = properties::indexOf(id);
    return k >= 0 && properties::at(static_cast<size_t>(k)).isFile() ? &properties::at(static_cast<size_t>(k)) : nullptr;
}
}  // namespace

int Ui::fileSlotAt(float x, float y, const std::vector<std::string>& paths) const {
    const int i = controlAt(x, y);
    if (i < 0 || controls_[static_cast<size_t>(i)].widget != Widget::File || !interactive(i)) return -1;
    const PropertyDef* p = fileProperty(controls_[static_cast<size_t>(i)].param);
    if (paths.empty()) return i;  // still being dragged: what it holds is not known yet
    for (const std::string& path : paths)
        if (files::hasExtension(path, p->extensions)) return i;
    return -1;
}

bool Ui::dropFiles(float x, float y, const std::vector<std::string>& paths) {
    if (paths.empty()) return false;
    const int i = fileSlotAt(x, y, paths);
    if (i < 0) return false;
    const Control& c = controls_[static_cast<size_t>(i)];
    for (const std::string& path : paths) {
        if (!files::hasExtension(path, fileProperty(c.param)->extensions)) continue;
        setFocus(i);
        host_.setProperty(c.param, path);
        dirty_ = true;
        return true;
    }
    return false;
}

void Ui::chooseFile(int i) {
    if (i < 0 || i >= count() || !files_ || !interactive(i)) return;
    const Control& c = controls_[static_cast<size_t>(i)];
    const PropertyDef* p = fileProperty(c.param);
    if (!p) return;
    FileDialogs::Options o;
    o.title = std::string("Choose ") + (c.label ? c.label : p->name);
    for (std::string_view types = p->extensions; !types.empty();) {
        const size_t space = types.find(' ');
        const std::string_view type = types.substr(0, space);
        if (!type.empty()) o.extensions.push_back("." + std::string(type));
        types = space == std::string_view::npos ? std::string_view() : types.substr(space + 1);
    }
    // Where the present file is, if it is anywhere.
    const std::string current(host_.property(c.param));
    if (!current.empty()) {
        const std::string folder = files::folderOf(current);
        std::error_code ec;
        if (std::filesystem::is_directory(files::fromUtf8(folder), ec)) o.folder = folder;
    }
    const uint32_t id = c.param;
    files_->open(o, [this, id](const std::vector<std::string>& paths) {
        if (!paths.empty()) host_.setProperty(id, paths.front());
        dirty_ = true;
    });
}

// --- presets -----------------------------------------------------------------------

Rect Ui::presetPart(const Rect& bar, int part) {
    const float arrow = std::min(26.0f, bar.h + 2.0f), compare = 48.0f, gap = 6.0f;
    const Rect previous{bar.x, bar.y, arrow, bar.h};
    const Rect ab{bar.right() - compare, bar.y, compare, bar.h};
    const Rect next{ab.x - gap - arrow, bar.y, arrow, bar.h};
    switch (part) {
        case presetbar::kPrevious: return previous;
        case presetbar::kNext: return next;
        case presetbar::kCompare: return ab;
        default: return {previous.right() + 2.0f, bar.y, next.x - 2.0f - (previous.right() + 2.0f), bar.h};
    }
}

int Ui::presetPartAt(int i, float x) const {
    const Rect& bar = controls_[static_cast<size_t>(i)].bounds;
    for (int part : {presetbar::kPrevious, presetbar::kNext, presetbar::kCompare})
        if (x >= presetPart(bar, part).x && x < presetPart(bar, part).right()) return part;
    return presetbar::kName;
}

void Ui::syncPresets(bool force) {
    PresetLibrary* library = host_.presetLibrary();
    if (!library) return;
    bool shown = false;
    for (int i = 0; i < count(); ++i) {
        const Control& c = controls_[static_cast<size_t>(i)];
        shown = shown || ((c.widget == Widget::PresetBar || (c.widget == Widget::List && c.param == kNoParam)) &&
                          this->shown(i));
    }
    if (!shown && !force) return;
    presetPoll_ = 0.0;
    const bool modified = library->modified();
    if (library->currentName() != presetName_ || modified != presetModified_ || library->slot() != presetSlot_ ||
        library->revision() != presetRevision_) {
        presetName_ = library->currentName();
        presetModified_ = modified;
        presetSlot_ = library->slot();
        presetRevision_ = library->revision();
        // A preset list follows the preset that loaded, wherever it came from.
        for (int i = 0; i < count(); ++i)
            if (controls_[static_cast<size_t>(i)].widget == Widget::List) listReveal(i);
        dirty_ = true;
    }
}

void Ui::presetAction(int i, int part) {
    PresetLibrary* library = host_.presetLibrary();
    if (!library) return;
    switch (part) {
        case presetbar::kPrevious: library->step(-1); break;
        case presetbar::kNext: library->step(1); break;
        case presetbar::kCompare: library->compare(1 - library->slot()); break;
        default: openPresetMenu(i); return;
    }
    syncPresets(true);
}

void Ui::openPresetMenu(int i) {
    PresetLibrary* library = host_.presetLibrary();
    if (!library) return;
    library->refresh();
    const Rect name = presetPart(controls_[static_cast<size_t>(i)].bounds, presetbar::kName);
    const auto& entries = library->entries();
    const int current = library->current();
    const bool user = current >= 0 && entries[static_cast<size_t>(current)].user();

    std::vector<MenuItem> items;
    std::vector<std::function<void()>> actions;
    auto add = [&](MenuItem item, std::function<void()> action) {
        items.push_back(std::move(item));
        actions.push_back(std::move(action));
    };
    // Factory presets under their categories, in table order; then the user's.
    for (const PresetRow& row : presetRows(*library)) {
        if (row.entry < 0) {
            add(MenuItem::header(row.text), nullptr);
        } else {
            const int k = row.entry;
            add(MenuItem::item(row.text, k == current), [library, k] { library->load(k); });
        }
    }
    if (!items.empty()) add(MenuItem::separator(), nullptr);

    const std::string currentName = library->currentName();
    add(MenuItem::item("Save", false, user && library->modified()),
        [library, currentName] { library->save(currentName, true); });
    add(MenuItem::item("Save As…", false, library->canSave()), [this, i, library, name, currentName] {
        prompt(name, currentName, static_cast<int>(PresetLibrary::kMaxName),
               [this, i, library, name](std::string_view text) {
                   const PresetLibrary::SaveResult result = library->save(text, false);
                   if (result == PresetLibrary::SaveResult::Exists) {
                       const std::string chosen(text);
                       popup(name,
                             {MenuItem::header("Replace “" + chosen + "”?"), MenuItem::item("Replace"),
                              MenuItem::item("Cancel")},
                             [this, library, chosen](int row) {
                                 if (row == 1) library->save(chosen, true);
                                 syncPresets(true);
                             },
                             220.0f, i);
                       return true;
                   }
                   syncPresets(true);
                   return result == PresetLibrary::SaveResult::Ok;
               },
               i);
    });
    add(MenuItem::item("Rename…", false, user), [this, i, library, name, current, currentName] {
        prompt(name, currentName, static_cast<int>(PresetLibrary::kMaxName),
               [this, library, current](std::string_view text) {
                   const bool ok = library->rename(current, text);
                   syncPresets(true);
                   return ok;
               },
               i);
    });
    add(MenuItem::item("Delete…", false, user), [this, i, library, name, current, currentName] {
        popup(name,
              {MenuItem::header("Delete “" + currentName + "”?"), MenuItem::item("Delete"),
               MenuItem::item("Cancel")},
              [this, library, current](int row) {
                  if (row == 1) library->remove(current);
                  syncPresets(true);
              },
              220.0f, i);
    });
    add(MenuItem::separator(), nullptr);
    // Files anywhere, through the system's own dialogs.
    FileDialogs* files = files_;
    add(MenuItem::item("Import…", false, files != nullptr), [this, files, library] {
        FileDialogs::Options options;
        options.title = "Import Preset";
        options.extensions = {PresetLibrary::kExtension};
        files->open(options, [this, library](const std::vector<std::string>& paths) {
            if (!paths.empty()) library->loadFile(PresetLibrary::fromUtf8(paths.front()));
            syncPresets(true);
        });
    });
    add(MenuItem::item("Export…", false, files != nullptr), [files, library, currentName] {
        FileDialogs::Options options;
        options.title = "Export Preset";
        options.extensions = {PresetLibrary::kExtension};
        options.name = (currentName.empty() ? std::string("Preset") : currentName) + PresetLibrary::kExtension;
        files->save(options, [library](const std::vector<std::string>& paths) {
            if (!paths.empty()) library->exportFile(PresetLibrary::fromUtf8(paths.front()));
        });
    });
    add(MenuItem::item("Copy Settings", false, clipboard_ != nullptr),
        [this, library] { clipboard_->copy(library->copyText()); });
    add(MenuItem::item("Paste Settings", false, clipboard_ != nullptr), [this, library] {
        clipboard_->paste([this, library](std::string_view text) {
            library->pasteText(text);
            syncPresets(true);
        });
    });
    add(MenuItem::item(library->slot() == 0 ? "Copy A to B" : "Copy B to A"), [library] { library->copyToOther(); });
    add(MenuItem::item("Default Settings"), [library] { library->loadDefaults(); });

    popup(name, std::move(items),
          [this, actions = std::move(actions)](int row) {
              if (actions[static_cast<size_t>(row)]) actions[static_cast<size_t>(row)]();
              syncPresets(true);
          },
          220.0f, i);
}

void Ui::focusLost() {
    pointerUp(0.0f, 0.0f, 0, 0);
    endGesture();
    release();
    drag_ = Drag{};
    closeMenu();
    cancelText();
    hideTooltip();
    tooltipArmed_ = false;
    if (focus_ >= 0 || hover_ >= 0) dirty_ = true;
    focus_ = -1;
    hover_ = -1;
    hoverPart_ = -1;
}

// --- time ----------------------------------------------------------------------------

void Ui::cancelInactiveOverlays() {
    if (menu_.open && menu_.control >= 0 && !interactive(menu_.control)) closeMenu();
    if (text_.open && text_.control >= 0 && !interactive(text_.control)) cancelText();
}

void Ui::applyRules() {
    for (Control& c : controls_) {
        if (!c.rule) continue;
        const bool enabled = c.rule();
        if (enabled != c.enabled) {
            c.enabled = enabled;
            dirty_ = true;
        }
    }
    cancelInactiveOverlays();
    // A control that just turned off lets go of what it held.
    if (drag_.control >= 0 && !interactive(drag_.control)) pointerUp(0.0f, 0.0f, 0, 0);
    if (focus_ >= 0 && !interactive(focus_)) {
        if (gesture_.involves(focus_)) endGesture();
        focus_ = -1;
        dirty_ = true;
    }
}

void Ui::idle(double seconds) {
    applyRules();
    // A file loads, or goes missing, without any property changing.
    fileStates_.resize(controls_.size(), 0xFF);
    for (size_t i = 0; i < controls_.size(); ++i) {
        if (controls_[i].widget != Widget::File) continue;
        const auto now = static_cast<uint8_t>(host_.fileStatus(controls_[i].param).state);
        if (now != fileStates_[i]) dirty_ = true;
        fileStates_[i] = now;
    }
    presetPoll_ += seconds;
    if (presetPoll_ >= 0.25) syncPresets(false);
    if ((gesture_.control[0] >= 0 || gesture_.control[1] >= 0) && gesture_.timed) {
        gesture_.idle += seconds;
        if (gesture_.idle >= kGestureIdleSeconds) endGesture();
    }
    if (text_.open) {
        text_.blink += seconds;
        if (text_.blink >= 0.53) {
            text_.blink = 0.0;
            text_.caretOn = !text_.caretOn;
            dirty_ = true;
        }
    }
    if (tooltipArmed_ && !tooltip_.open && drag_.control < 0 && !menu_.open && !text_.open) {
        tooltipWait_ += seconds;
        if (tooltipWait_ >= kTooltipSeconds) {
            tooltipArmed_ = false;
            openTooltip();
        }
    }
}

bool Ui::takeDirty() {
    const bool d = dirty_;
    dirty_ = false;
    return d;
}

bool Ui::animating() const {
    return ((gesture_.control[0] >= 0 || gesture_.control[1] >= 0) && gesture_.timed) || text_.open ||
           (tooltipArmed_ && !tooltip_.open);
}

Cursor Ui::cursor() const {
    if (text_.open) return text_.bounds.contains(pointerX_, pointerY_) ? Cursor::Caret : Cursor::Arrow;
    if (menu_.open) return menu_.bounds.contains(pointerX_, pointerY_) ? Cursor::Hand : Cursor::Arrow;
    const int i = drag_.control >= 0 ? drag_.control : hover_;
    if (i < 0) return Cursor::Arrow;
    switch (controls_[static_cast<size_t>(i)].widget) {
        case Widget::Knob:
        case Widget::VSlider: return Cursor::UpDown;
        case Widget::HSlider: return Cursor::LeftRight;
        case Widget::TextField: return Cursor::Caret;
        case Widget::XYPad: return Cursor::Move;
        case Widget::Envelope:
            return drag_.control == i || hoverPart_ >= 0 ? Cursor::Move : Cursor::Arrow;
        default: return Cursor::Hand;
    }
}

}  // namespace supple::gui
