// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "Editor.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "Widgets.h"
#include "orrery/edits.h"
#include "orrery/params.h"

extern "C" {
extern const unsigned char orrery_font_mono[];
extern const unsigned int orrery_font_mono_size;
extern const unsigned char orrery_font_mono_medium[];
extern const unsigned int orrery_font_mono_medium_size;
extern const unsigned char orrery_font_display[];
extern const unsigned int orrery_font_display_size;
extern const unsigned char orrery_font_ui[];
extern const unsigned int orrery_font_ui_size;
extern const unsigned char orrery_font_ui_semibold[];
extern const unsigned int orrery_font_ui_semibold_size;
}

namespace orrery::gui {

namespace so = supple::orrery;
namespace edits = supple::orrery::edits;
namespace params = supple::params;
using supple::json::Value;
using Kind = Editor::Hit::Kind;

namespace {

constexpr float kWheelGestureSeconds = 0.25f;
constexpr int kMenuRows = 22;
constexpr float kMenuRowH = 20.0f;
constexpr float kRouteRowH = 22.0f;

const params::ParamSpec& spec(std::size_t i) { return so::table()[i]; }

bool isChoice(const params::ParamSpec& p) {
    return p.type == params::Type::Enum || p.type == params::Type::Bool;
}

std::string upper(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    return out;
}

// "osc1" → "OSC 1", "env2" → "ENV 2", "amp" (the envelope) → "AMP ENV".
std::string tileTitle(so::Slot s) {
    if (s == so::kEnvAmp) return "AMP ENV";
    std::string n = upper(so::slotInfo(s).name);
    if (n.rfind("FX.", 0) == 0) n = n.substr(3);
    if (!n.empty() && n.back() >= '0' && n.back() <= '9') n.insert(n.size() - 1, " ");
    return n;
}

std::string choiceLabel(const params::ParamSpec& p, double v) {
    if (p.type == params::Type::Bool) return v >= 0.5 ? "On" : "Off";
    const auto k = static_cast<std::size_t>(std::lround(v));
    return k < p.choices.size() ? std::string(p.choices[k].label) : std::string("?");
}

std::string targetName(so::OscTarget t) {
    constexpr const char* kNames[] = {"→ filter 1", "→ filter 2", "→ both", "→ direct", "mod only"};
    return kNames[static_cast<std::size_t>(t)];
}

std::string fmtAmount(const so::Route& r) {
    char buf[48];
    const auto unit = so::modUnit(r.dest);
    const std::string_view u = supple::units::unitName(unit);
    std::snprintf(buf, sizeof(buf), "%+.2f %.*s", r.amount, static_cast<int>(u.size()), u.data());
    return buf;
}

double amountStep(const so::Dest& d) {
    switch (so::modUnit(d)) {
        case supple::units::Unit::oct:
            return 0.01;
        case supple::units::Unit::st:
            return 0.05;
        case supple::units::Unit::dB:
            return 0.1;
        case supple::units::Unit::x:
            return 0.02;
        default:
            return 0.25;
    }
}

double amountLimit(const so::Dest& d) {
    switch (so::modUnit(d)) {
        case supple::units::Unit::oct:
            return 10.0;
        case supple::units::Unit::st:
            return 48.0;
        case supple::units::Unit::dB:
            return 48.0;
        case supple::units::Unit::x:
            return 20.0;
        default:
            return 100.0;
    }
}

// The slots of each part of the diagram.
constexpr so::Slot kEnvTiles[] = {so::kEnvAmp, so::kEnv2, so::kEnv3, so::kEnv4};
constexpr so::Slot kLfoTiles[] = {so::kLfo1, so::kLfo2, so::kLfo3, so::kLfo4};
constexpr so::FxKind kFxKinds[] = {so::FxKind::Chorus, so::FxKind::Phaser, so::FxKind::Delay,
                                   so::FxKind::Reverb, so::FxKind::Eq};

so::Slot fxSlot(so::FxKind k) {
    switch (k) {
        case so::FxKind::Chorus:
            return so::kChorus;
        case so::FxKind::Phaser:
            return so::kPhaser;
        case so::FxKind::Delay:
            return so::kDelay;
        case so::FxKind::Reverb:
            return so::kReverb;
        case so::FxKind::Eq:
            return so::kEq;
    }
    return so::kChorus;
}

}  // namespace

Editor::Editor(EditorHost& host) : host_(host) { host_.setTelemetryEnabled(true); }

void Editor::setVisible(bool on) { host_.setTelemetryEnabled(on); }

bool Editor::attach(NVGcontext* vg) {
    if (fontsReady_) return true;
    auto font = [&](const char* name, const unsigned char* data, unsigned int size) {
        return nvgCreateFontMem(vg, name, const_cast<unsigned char*>(data), static_cast<int>(size),
                                0) >= 0;
    };
    fontsReady_ = font(kFontMono, orrery_font_mono, orrery_font_mono_size) &&
                  font(kFontMonoMed, orrery_font_mono_medium, orrery_font_mono_medium_size) &&
                  font(kFontDisplay, orrery_font_display, orrery_font_display_size) &&
                  font(kFontUi, orrery_font_ui, orrery_font_ui_size) &&
                  font(kFontUiSemi, orrery_font_ui_semibold, orrery_font_ui_semibold_size);
    return fontsReady_;
}

// --- time ------------------------------------------------------------------------------------

void Editor::pump(double elapsedSeconds) {
    const float dt = static_cast<float>(std::clamp(elapsedSeconds, 0.0, 0.25));
    const so::Telemetry t = host_.telemetry();
    const float decay = std::exp(-dt * 6.0f);
    const float l = std::max(t.peakL, peakL_ * decay), r = std::max(t.peakR, peakR_ * decay);
    if (std::fabs(l - peakL_) > 1e-4f || std::fabs(r - peakR_) > 1e-4f || t.voices != voices_)
        dirty_ = true;
    peakL_ = l;
    peakR_ = r;
    voices_ = t.voices;
    if (wheelParam_ >= 0) {
        wheelIdle_ += dt;
        if (wheelIdle_ >= kWheelGestureSeconds) closeWheelGesture();
    }
    if (!status_.empty()) {
        statusAge_ += dt;
        if (!statusError_ && statusAge_ > 4.0f) {
            status_.clear();
            dirty_ = true;
        }
    }
    if (host_.revision() != seenRevision_) dirty_ = true;
}

bool Editor::wantsRepaint() const {
    return dirty_ || wheelParam_ >= 0 || peakL_ > 1e-3f || peakR_ > 1e-3f;
}

// --- layout ----------------------------------------------------------------------------------

std::vector<std::size_t> Editor::inspectorParams() const {
    const so::Structure& st = host_.structure();
    std::vector<std::size_t> out;
    const so::Block block = so::slotInfo(selected_).block;
    if (block == so::Block::Macro) {
        for (int m = 0; m < so::kMacroCount; ++m)
            out.push_back(so::index(so::macro(m), so::MacroF::value));
        return out;
    }
    auto add = [&](so::Slot s) {
        const auto n = so::fieldCount(so::slotInfo(s).block);
        for (std::uint16_t f = 0; f < n; ++f) {
            const std::size_t i = so::index(s, f);
            if ((spec(i).flags & params::kHidden) || !so::paramActive(st, i)) continue;
            out.push_back(i);
        }
    };
    add(selected_);
    if (block == so::Block::Filter) add(so::kFilters);  // the parallel balance
    return out;
}

void Editor::layout() {
    hits_.clear();
    const so::Structure& st = host_.structure();
    if (!st.has(selected_) && selected_ != so::kMacro1) selected_ = so::kOsc1;
    auto push = [&](Kind k, float x, float y, float w, float h, int a = -1) {
        hits_.push_back({k, x, y, w, h, a});
    };

    // Signal flow.
    const float ox = kFlowX + 14, fx = ox + 150, dx = fx + 150, xx = dx + 112;
    for (int i = 0; i < so::kOscCount; ++i) {
        const so::Slot s = so::osc(i);
        push(st.has(s) ? Kind::Tile : Kind::AddTile, ox, kFlowY + 40 + static_cast<float>(i) * 72,
             118, 58, s);
    }
    for (int i = 0; i < so::kFilterCount; ++i) {
        const so::Slot s = so::filter(i);
        push(st.has(s) ? Kind::Tile : Kind::AddTile, fx, kFlowY + 62 + static_cast<float>(i) * 120,
             118, 84, s);
    }
    if (st.filter[0] && st.filter[1]) push(Kind::Routing, fx + 14, kFlowY + 154, 90, 20);
    push(st.has(so::kDrive) ? Kind::Tile : Kind::AddTile, dx, kFlowY + 62, 88, 58, so::kDrive);
    push(Kind::Tile, dx, kFlowY + 138, 88, 58, so::kAmp);
    push(Kind::Tile, dx, kFlowY + 214, 88, 44, so::kOutput);
    for (std::size_t k = 0; k < st.chain.size(); ++k)
        push(Kind::Tile, xx, kFlowY + 62 + static_cast<float>(k) * 44, 84, 36, fxSlot(st.chain[k]));
    if (st.chain.size() < std::size(kFxKinds))
        push(Kind::AddFx, xx, kFlowY + 62 + static_cast<float>(st.chain.size()) * 44, 84, 28);

    // Modulators.
    for (int i = 0; i < 4; ++i) {
        const float x = kModX + 14 + static_cast<float>(i) * 124;
        push(st.has(kEnvTiles[i]) ? Kind::Tile : Kind::AddTile, x, kModY + 38, 116, 50,
             kEnvTiles[i]);
        push(st.has(kLfoTiles[i]) ? Kind::Tile : Kind::AddTile, x, kModY + 100, 116, 50,
             kLfoTiles[i]);
    }
    for (int i = 0; i < so::kStepCount; ++i)
        push(st.has(so::step(i)) ? Kind::Tile : Kind::AddTile,
             kModX + 14 + static_cast<float>(i) * 124, kModY + 162, 116, 50, so::step(i));
    push(Kind::Tile, kModX + 14 + 2 * 124, kModY + 162, 116, 50, so::kMacro1);
    push(Kind::Tile, kModX + 14 + 3 * 124, kModY + 162, 116, 50, so::kVoice);

    // Header: quality.
    for (int q = 0; q < 3; ++q)
        push(Kind::Quality, kDesignW - 420 + static_cast<float>(q) * 62, 20, 56, 20, q);

    // Inspector.
    const so::Block block = so::slotInfo(selected_).block;
    float hx = kInspX + kInspW - 14;
    if (edits::removable(selected_) && st.has(selected_)) {
        hx -= 72;
        push(Kind::Remove, hx, kInspY + 8, 72, 20, selected_);
    }
    if (block == so::Block::Osc || block == so::Block::Filter || block == so::Block::Drive) {
        hx -= 104;
        push(Kind::TypePill, hx, kInspY + 8, 96, 20, selected_);
    }
    if (block == so::Block::Osc) {
        hx -= 104;
        push(Kind::TargetPill, hx, kInspY + 8, 96, 20, selected_ - so::kOsc1);
    }
    const auto ps = inspectorParams();
    const int cols = ps.size() > 18 ? 7 : 6;
    const float cw = (kInspW - 28) / static_cast<float>(cols), ch = ps.size() > 18 ? 70.0f : 88.0f;
    for (std::size_t k = 0; k < ps.size(); ++k) {
        const float x = kInspX + 14 + static_cast<float>(k % static_cast<std::size_t>(cols)) * cw;
        const float y = kInspY + 44 + static_cast<float>(k / static_cast<std::size_t>(cols)) * ch;
        push(isChoice(spec(ps[k])) ? Kind::Choice
                                   : (block == so::Block::Macro ? Kind::Macro : Kind::Knob),
             x, y, cw, ch, static_cast<int>(ps[k]));
    }

    // Routes: the selected module's first.
    push(Kind::AddRoute, kRouteX + kRouteW - 96, kRouteY + 8, 82, 20);
    std::vector<int> order;
    for (std::size_t i = 0; i < st.routes.size(); ++i) order.push_back(static_cast<int>(i));
    auto mine = [&](int i) {
        const so::Route& r = st.routes[static_cast<std::size_t>(i)];
        const auto src = edits::sourceSlot(r.source);
        return (src && *src == selected_) || edits::destSlot(r.dest) == selected_;
    };
    std::stable_partition(order.begin(), order.end(), mine);
    const int rows = static_cast<int>((kRouteH - 44) / kRouteRowH);
    for (int k = 0; k < std::min<int>(rows, static_cast<int>(order.size())); ++k) {
        const float y = kRouteY + 38 + static_cast<float>(k) * kRouteRowH;
        push(Kind::RouteAmount, kRouteX + 330, y, 150, kRouteRowH - 2,
             order[static_cast<std::size_t>(k)]);
        push(Kind::RouteRemove, kRouteX + kRouteW - 34, y, 20, kRouteRowH - 2,
             order[static_cast<std::size_t>(k)]);
    }

    // The menu is topmost (hit-tested last to first).
    if (menu_.open) {
        const int n = static_cast<int>(menu_.items.size());
        const int cols = std::max(1, (n + kMenuRows - 1) / kMenuRows);
        const int rowsN = std::min(n, kMenuRows);
        const float w = menu_.w * static_cast<float>(cols);
        const float h = 26 + static_cast<float>(rowsN) * kMenuRowH + 6;
        menu_.x = std::clamp(menu_.x, 4.0f, kDesignW - w - 4);
        menu_.y = std::clamp(menu_.y, 4.0f, kDesignH - h - 4);
        for (int i = 0; i < n; ++i)
            push(Kind::MenuItem, menu_.x + static_cast<float>(i / kMenuRows) * menu_.w,
                 menu_.y + 26 + static_cast<float>(i % kMenuRows) * kMenuRowH, menu_.w, kMenuRowH,
                 i);
    }
}

const Editor::Hit* Editor::hitAt(float x, float y) const {
    if (menu_.open) {  // a modal menu: only its items
        for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
            if (it->kind == Kind::MenuItem && it->contains(x, y)) return &*it;
        return nullptr;
    }
    for (auto it = hits_.rbegin(); it != hits_.rend(); ++it)
        if (it->contains(x, y)) return &*it;
    return nullptr;
}

// --- actions ---------------------------------------------------------------------------------

void Editor::apply(const Value& patch) {
    const auto r = host_.merge(patch);
    if (r) {
        status_.clear();
        statusError_ = false;
    } else {
        const auto& e = r.error();
        status_ = e.message + (e.hint.empty() ? "" : " — " + e.hint);
        statusError_ = true;
    }
    statusAge_ = 0.0f;
    dirty_ = true;
}

void Editor::select(so::Slot s) {
    selected_ = s;
    focus_ = -1;
    dirty_ = true;
}

void Editor::setParamNormalized(std::size_t index, double normal) {
    const params::ParamSpec& p = spec(index);
    double v = params::fromNormalized(p, std::clamp(normal, 0.0, 1.0));
    if (p.type != params::Type::Float) v = std::round(v);
    host_.edit(index, v);
    dirty_ = true;
}

void Editor::editOnce(std::size_t index, double physical) {
    host_.beginEdit(index);
    host_.edit(index, physical);
    host_.endEdit(index);
    dirty_ = true;
}

void Editor::closeWheelGesture() {
    if (wheelParam_ >= 0) host_.endEdit(static_cast<std::size_t>(wheelParam_));
    wheelParam_ = -1;
    wheelIdle_ = 0.0f;
}

void Editor::openMenu(float x, float y, std::string title, std::vector<std::string> items,
                      std::function<void(int)> pick) {
    endDrag();  // a menu is modal: no parameter keeps moving under it
    menu_.open = true;
    menu_.x = x;
    menu_.y = y;
    menu_.title = std::move(title);
    menu_.items = std::move(items);
    menu_.pick = std::move(pick);
    menu_.hover = -1;
    dirty_ = true;
}

void Editor::closeMenu() {
    menu_.open = false;
    menu_.items.clear();
    menu_.pick = nullptr;
    dirty_ = true;
}

void Editor::openChoiceMenu(const Hit& h) {
    const auto index = static_cast<std::size_t>(h.a);
    const params::ParamSpec& p = spec(index);
    std::vector<std::string> items;
    std::vector<double> values;
    const so::Structure& st = host_.structure();
    const auto loc = so::locate(index);
    const bool wave = so::slotInfo(loc.slot).block == so::Block::Osc &&
                      loc.field == static_cast<std::uint16_t>(so::OscF::wave);
    if (p.type == params::Type::Bool) {
        items = {"Off", "On"};
        values = {0.0, 1.0};
    } else {
        for (std::size_t k = 0; k < p.choices.size(); ++k) {
            if (wave) {
                const auto t = st.osc[static_cast<std::size_t>(loc.slot - so::kOsc1)];
                if (t && !so::waveApplies(*t, static_cast<so::Wave>(k))) continue;
            }
            items.emplace_back(p.choices[k].label);
            values.push_back(static_cast<double>(k));
        }
    }
    openMenu(h.x, h.y + h.h, std::string(p.name), std::move(items), [this, index, values](int i) {
        editOnce(index, values[static_cast<std::size_t>(i)]);
    });
}

void Editor::openTypeMenu(const Hit& h) {
    const so::Slot s = static_cast<so::Slot>(h.a);
    const so::Block b = so::slotInfo(s).block;
    std::vector<std::string> items;
    if (b == so::Block::Osc)
        for (int t = 0; t < so::kOscTypeCount; ++t)
            items.emplace_back(so::oscTypeName(static_cast<so::OscType>(t)));
    else if (b == so::Block::Filter)
        for (int t = 0; t < so::kFilterTypeCount; ++t)
            items.emplace_back(so::filterTypeName(static_cast<so::FilterType>(t)));
    else
        items = {"off", "soft", "tape", "fold", "hard"};
    openMenu(h.x, h.y + h.h, "Type", std::move(items),
             [this, s](int t) { apply(edits::setType(host_.structure(), s, t)); });
}

void Editor::openAddRouteMenu(float x, float y) {
    const so::Structure& st = host_.structure();
    const auto sources = edits::availableSources(st);
    std::vector<std::string> names;
    for (so::Source s : sources) names.emplace_back(so::sourceName(s));
    openMenu(x, y, "Route from", std::move(names), [this, sources, x, y](int i) {
        const so::Source src = sources[static_cast<std::size_t>(i)];
        const auto dests = edits::availableDests(host_.structure(), src);
        // Group destinations by module, then pick the parameter.
        std::vector<so::Slot> slots;
        for (const so::Dest& d : dests)
            if (std::find(slots.begin(), slots.end(), edits::destSlot(d)) == slots.end())
                slots.push_back(edits::destSlot(d));
        std::vector<std::string> slotNames;
        for (so::Slot s : slots) slotNames.push_back(tileTitle(s));
        openMenu(x, y, std::string(so::sourceName(src)) + " → module", std::move(slotNames),
                 [this, src, dests, slots, x, y](int k) {
                     const so::Slot slot = slots[static_cast<std::size_t>(k)];
                     std::vector<so::Dest> in;
                     std::vector<std::string> labels;
                     for (const so::Dest& d : dests)
                         if (edits::destSlot(d) == slot) {
                             in.push_back(d);
                             labels.push_back(so::destName(d));
                         }
                     openMenu(x, y, "→ parameter", std::move(labels), [this, src, in](int j) {
                         so::Route r;
                         r.source = src;
                         r.dest = in[static_cast<std::size_t>(j)];
                         r.amount = edits::defaultAmount(r.dest);
                         apply(edits::setRoute(r));
                     });
                 });
    });
}

// --- input -----------------------------------------------------------------------------------

void Editor::endDrag() {
    if (drag_.kind == Drag::Param) host_.endEdit(static_cast<std::size_t>(drag_.target));
    drag_ = {};
    dirty_ = true;
}

bool Editor::onPointerDown(float x, float y, int button, uint32_t mods) {
    endDrag();  // a second press mid-drag (another button) ends the drag first
    layout();
    if (menu_.open) {
        const Hit* h = hitAt(x, y);
        if (h && h->kind == Kind::MenuItem) {
            auto pick = menu_.pick;
            const int item = h->a;
            closeMenu();
            if (pick) pick(item);  // may open the next menu
        } else {
            closeMenu();
        }
        return true;
    }
    const Hit* h = hitAt(x, y);
    if (!h) return false;
    closeWheelGesture();
    const so::Structure& st = host_.structure();
    const bool alt = button == 1;  // right click
    switch (h->kind) {
        case Kind::Tile:
            select(static_cast<so::Slot>(h->a));
            return true;
        case Kind::AddTile: {
            const auto s = static_cast<so::Slot>(h->a);
            apply(edits::addSlot(st, s));
            if (host_.structure().has(s)) select(s);
            return true;
        }
        case Kind::Knob:
        case Kind::Macro: {
            const auto i = static_cast<std::size_t>(h->a);
            focus_ = h->a;
            drag_ = {Drag::Param, h->a, y, params::toNormalized(spec(i), host_.value(i))};
            host_.beginEdit(i);
            dirty_ = true;
            return true;
        }
        case Kind::Choice:
            focus_ = h->a;
            openChoiceMenu(*h);
            return true;
        case Kind::TypePill:
            openTypeMenu(*h);
            return true;
        case Kind::TargetPill: {
            const int o = h->a;
            openMenu(
                h->x, h->y + h->h, "Output", {"filter 1", "filter 2", "both", "direct", "mod only"},
                [this, o](int t) { apply(edits::setOscTarget(o, static_cast<so::OscTarget>(t))); });
            return true;
        }
        case Kind::Remove:
            apply(edits::removeSlot(st, static_cast<so::Slot>(h->a)));
            return true;
        case Kind::Routing:
            apply(edits::setRouting(st.routing == so::FilterRouting::Serial
                                        ? so::FilterRouting::Parallel
                                        : so::FilterRouting::Serial));
            return true;
        case Kind::Quality:
            apply(edits::setQuality(static_cast<so::Quality>(h->a)));
            return true;
        case Kind::RouteAmount: {
            const so::Route& r = st.routes[static_cast<std::size_t>(h->a)];
            if (alt) {  // right click: choose a "via" source
                std::vector<std::string> names = {"(none)"};
                std::vector<so::Source> vias;
                for (so::Source s : edits::availableSources(st))
                    if (!so::isAudioSource(s)) {
                        names.emplace_back(so::sourceName(s));
                        vias.push_back(s);
                    }
                const so::Route route = r;
                openMenu(h->x, h->y + h->h, "Scale by (via)", std::move(names),
                         [this, route, vias](int k) {
                             so::Route next = route;
                             next.via = k == 0 ? std::nullopt
                                               : std::optional<so::Source>(
                                                     vias[static_cast<std::size_t>(k - 1)]);
                             apply(edits::setRoute(next));
                         });
                return true;
            }
            drag_ = {Drag::RouteAmount, h->a, y, r.amount};
            return true;
        }
        case Kind::RouteRemove:
            apply(edits::removeRoute(st.routes[static_cast<std::size_t>(h->a)]));
            return true;
        case Kind::AddRoute:
            openAddRouteMenu(h->x - 120, h->y + h->h);
            return true;
        case Kind::AddFx: {
            std::vector<std::string> items;
            std::vector<so::FxKind> kinds;
            for (so::FxKind k : kFxKinds)
                if (std::find(st.chain.begin(), st.chain.end(), k) == st.chain.end()) {
                    items.emplace_back(so::fxName(k));
                    kinds.push_back(k);
                }
            openMenu(h->x, h->y + h->h, "Add FX", std::move(items), [this, kinds](int k) {
                const so::FxKind kind = kinds[static_cast<std::size_t>(k)];
                apply(edits::addFx(host_.structure(), kind));
                select(fxSlot(kind));
            });
            return true;
        }
        default:
            (void)mods;
            return false;
    }
}

void Editor::onPointerDouble(float x, float y, int button, uint32_t mods) {
    layout();
    const Hit* h = hitAt(x, y);
    if (!h || menu_.open) {
        onPointerDown(x, y, button, mods);
        return;
    }
    if (h->kind == Kind::Knob || h->kind == Kind::Macro) {
        if (drag_.kind == Drag::Param) onPointerUp(x, y, button, mods);
        const auto i = static_cast<std::size_t>(h->a);
        editOnce(i, so::defaultValue(host_.structure(), i));
        return;
    }
    if (h->kind == Kind::RouteAmount) {
        drag_ = {};
        so::Route r = host_.structure().routes[static_cast<std::size_t>(h->a)];
        r.amount = edits::defaultAmount(r.dest);
        apply(edits::setRoute(r));
        return;
    }
    onPointerDown(x, y, button, mods);
}

void Editor::onPointerMove(float x, float y, uint32_t mods) {
    const bool fine = (mods & kModShift) != 0;
    if (drag_.kind == Drag::Param) {
        const double scale = fine ? 0.0015 : 0.006;
        setParamNormalized(static_cast<std::size_t>(drag_.target),
                           drag_.startValue + static_cast<double>(drag_.startY - y) * scale);
        return;
    }
    if (drag_.kind == Drag::RouteAmount) {
        const so::Structure& st = host_.structure();
        if (drag_.target >= static_cast<int>(st.routes.size())) return;
        so::Route r = st.routes[static_cast<std::size_t>(drag_.target)];
        const double step = amountStep(r.dest) * (fine ? 0.2 : 1.0);
        const double lim = amountLimit(r.dest);
        const double next =
            std::clamp(drag_.startValue + static_cast<double>(drag_.startY - y) * step, -lim, lim);
        if (std::fabs(next - r.amount) < 1e-9) return;
        r.amount = std::round(next / step) * step;
        apply(edits::setRoute(r));
        return;
    }
    layout();
    const Hit* h = hitAt(x, y);
    const int next = h ? static_cast<int>(h - hits_.data()) : -1;
    if (menu_.open) {
        const int item = h && h->kind == Kind::MenuItem ? h->a : -1;
        if (item != menu_.hover) {
            menu_.hover = item;
            dirty_ = true;
        }
    }
    if (next != hover_) {
        hover_ = next;
        dirty_ = true;
    }
}

void Editor::onPointerUp(float, float, int, uint32_t) { endDrag(); }

bool Editor::onWheel(float x, float y, float, float dy, uint32_t mods) {
    layout();
    const Hit* h = hitAt(x, y);
    if (!h || (h->kind != Kind::Knob && h->kind != Kind::Macro)) return false;
    const auto i = static_cast<std::size_t>(h->a);
    if (wheelParam_ != h->a) {
        closeWheelGesture();
        wheelParam_ = h->a;
        host_.beginEdit(i);
    }
    wheelIdle_ = 0.0f;
    focus_ = h->a;
    const double step = (mods & kModShift) ? 0.005 : 0.025;
    setParamNormalized(
        i, params::toNormalized(spec(i), host_.value(i)) + static_cast<double>(dy) * step);
    return true;
}

bool Editor::onKeyCode(uint32_t k, uint32_t) {
    if (k == key::kEscape && menu_.open) {
        closeMenu();
        return true;
    }
    return false;
}

void Editor::onKey(EditorKey k, bool fineOrReverse) {
    const auto ps = inspectorParams();
    if (ps.empty()) return;
    if (k == EditorKey::Tab) {
        auto it = std::find(ps.begin(), ps.end(), static_cast<std::size_t>(std::max(0, focus_)));
        std::size_t at = it == ps.end() ? 0 : static_cast<std::size_t>(it - ps.begin());
        if (focus_ >= 0)
            at = fineOrReverse ? (at + ps.size() - 1) % ps.size() : (at + 1) % ps.size();
        focus_ = static_cast<int>(ps[at]);
        dirty_ = true;
        return;
    }
    if (focus_ < 0) return;
    const auto i = static_cast<std::size_t>(focus_);
    const params::ParamSpec& p = spec(i);
    if (isChoice(p) && (k == EditorKey::Enter || k == EditorKey::Space)) {
        const double v = host_.value(i) + 1.0;
        editOnce(i, v > p.max ? p.min : v);
        return;
    }
    double n = params::toNormalized(p, host_.value(i));
    const double step = fineOrReverse ? 0.002 : 0.01;
    switch (k) {
        case EditorKey::Home:
            n = 0.0;
            break;
        case EditorKey::End:
            n = 1.0;
            break;
        case EditorKey::Left:
        case EditorKey::Down:
            n -= step;
            break;
        case EditorKey::Right:
        case EditorKey::Up:
            n += step;
            break;
        default:
            return;
    }
    host_.beginEdit(i);
    if (p.type != params::Type::Float) {
        const double direction =
            k == EditorKey::Left || k == EditorKey::Down || k == EditorKey::End ? -1. : 1.;
        double value = k == EditorKey::Home  ? p.min
                       : k == EditorKey::End ? p.max
                                             : std::clamp(host_.value(i) + direction, p.min, p.max);
        // Wave's permanent table is the union of oscillator types. Skip
        // inapplicable entries, just as its choice menu does, and stop at
        // the current type's boundary rather than falling back to its default.
        const auto loc = so::locate(i);
        if (so::slotInfo(loc.slot).block == so::Block::Osc &&
            loc.field == static_cast<std::uint16_t>(so::OscF::wave)) {
            const auto type = host_.structure().osc[static_cast<std::size_t>(loc.slot - so::kOsc1)];
            while (type && value >= p.min && value <= p.max &&
                   !so::waveApplies(*type, static_cast<so::Wave>(static_cast<int>(value))))
                value += direction;
            if (value < p.min || value > p.max) value = host_.value(i);
        }
        host_.edit(i, value);
    } else
        setParamNormalized(i, n);
    host_.endEdit(i);
    dirty_ = true;
}

void Editor::onPointerLeave() {
    onPointerUp(0, 0, 0, 0);
    if (hover_ >= 0) {
        hover_ = -1;
        dirty_ = true;
    }
}

void Editor::onFocusLost() {
    onPointerUp(0, 0, 0, 0);
    closeWheelGesture();
    closeMenu();
    focus_ = -1;
    hover_ = -1;
}

Cursor Editor::cursor() const {
    if (drag_.kind != Drag::None) return Cursor::UpDown;
    if (hover_ < 0 || hover_ >= static_cast<int>(hits_.size())) return Cursor::Arrow;
    switch (hits_[static_cast<std::size_t>(hover_)].kind) {
        case Kind::Knob:
        case Kind::Macro:
        case Kind::RouteAmount:
            return Cursor::UpDown;
        case Kind::None:
            return Cursor::Arrow;
        default:
            return Cursor::Hand;
    }
}

// --- drawing ---------------------------------------------------------------------------------

void Editor::render(NVGcontext* vg) {
    layout();
    if (host_.revision() != seenRevision_) {
        seenRevision_ = host_.revision();
        describe_ = host_.describe();
    }
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, kDesignW, kDesignH);
    nvgFillColor(vg, rgb(col::ink));
    nvgFill(vg);
    drawHeader(vg);
    drawFlow(vg);
    drawModulators(vg);
    drawInspector(vg);
    drawRoutes(vg);
    // Footer: the message line.
    if (!status_.empty())
        label(vg, status_.c_str(), kMargin, kFooterY + 6, 11,
              rgb(statusError_ ? col::alert : col::dust));
    else
        label(vg, "Click a module to edit it · right-click a route amount to scale it by a source",
              kMargin, kFooterY + 6, 10, rgb(col::dim));
    label(vg, "CLAP · semantic patch", kDesignW - kMargin, kFooterY + 6, 10, rgb(col::dim),
          Align::Right);
    drawMenu(vg);
    dirty_ = false;
}

void Editor::drawHeader(NVGcontext* vg) {
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, kDesignW, kHeaderH);
    nvgFillColor(vg, rgb(col::panel));
    nvgFill(vg);
    hairline(vg, 0, kHeaderH, kDesignW, kHeaderH, rgb(col::edgeHi));
    label(vg, "Supple Audio", kMargin, 18, 10, rgb(col::dust));
    label(vg, "ORRERY", kMargin, 40, 24, rgb(col::bone));
    // The patch in one line of prose, fitted to the space.
    nvgSave(vg);
    nvgScissor(vg, 150, 12, kDesignW - 440 - 150 - 12, 36);
    label(vg, describe_.c_str(), 150, 31, 11, rgb(col::dust));
    nvgRestore(vg);
    const so::Structure& st = host_.structure();
    for (const Hit& h : hits_)
        if (h.kind == Kind::Quality) {
            constexpr const char* kQ[] = {"eco", "normal", "high"};
            pill(vg, h.x, h.y, h.w, h.h, kQ[h.a], static_cast<int>(st.quality) == h.a,
                 rgb(col::accent), false,
                 hover_ >= 0 && &hits_[static_cast<std::size_t>(hover_)] == &h);
        }
    char v[32];
    std::snprintf(v, sizeof(v), "%d voices", voices_);
    num(vg, v, kDesignW - 136, 31, 11, rgb(col::dust), Align::Right);
    // Output meter: two bars, -60..0 dBFS.
    for (int c = 0; c < 2; ++c) {
        const float peak = c == 0 ? peakL_ : peakR_;
        const float db = peak > 1e-6f ? 20.0f * std::log10(peak) : -120.0f;
        const float t = std::clamp((db + 60.0f) / 60.0f, 0.0f, 1.0f);
        const float x = kDesignW - 120, y = 22 + static_cast<float>(c) * 10, w = 100;
        fillRoundRect(vg, x, y, w, 6, 2, rgb(col::well));
        fillRoundRect(vg, x, y, w * t, 6, 2, rgb(db > -0.5f ? col::alert : col::accent));
    }
}

void Editor::drawTile(NVGcontext* vg, const Hit& h, const char* title, const char* detail,
                      unsigned tone) {
    const bool sel = h.kind == Kind::Tile && h.a == selected_;
    const bool hot = hover_ >= 0 && &hits_[static_cast<std::size_t>(hover_)] == &h;
    if (h.kind == Kind::AddTile || h.kind == Kind::AddFx) {
        strokeRoundRect(vg, h.x + 0.5f, h.y + 0.5f, h.w - 1, h.h - 1, 5,
                        rgba(col::edgeHi, hot ? 1.0f : 0.6f));
        std::string t = std::string("+ ") + title;
        label(vg, t.c_str(), h.x + h.w * 0.5f, h.y + h.h * 0.5f, 10.5f,
              rgb(hot ? col::dust : col::dim), Align::Center, true);
        return;
    }
    fillRoundRect(vg, h.x, h.y, h.w, h.h, 5, rgb(sel ? col::knobTop : col::panel));
    strokeRoundRect(vg, h.x + 0.5f, h.y + 0.5f, h.w - 1, h.h - 1, 5,
                    sel ? rgb(tone) : rgba(col::edgeHi, hot ? 1.0f : 0.8f));
    fillRoundRect(vg, h.x + 6, h.y + 8, 3, h.h - 16, 1.5f, rgba(tone, sel ? 1.0f : 0.55f));
    label(vg, title, h.x + 16, h.y + 15, 11, rgb(col::bone), Align::Left, true);
    if (detail && *detail) {
        nvgSave(vg);
        nvgScissor(vg, h.x + 14, h.y + 22, h.w - 20, h.h - 24);
        label(vg, detail, h.x + 16, h.y + 32, 10, rgb(col::dust));
        nvgRestore(vg);
    }
}

void Editor::drawFlow(NVGcontext* vg) {
    cell(vg, kFlowX, kFlowY, kFlowW, kFlowH, "Patch");
    const so::Structure& st = host_.structure();
    auto tileOf = [&](so::Slot s) -> const Hit* {
        for (const Hit& h : hits_)
            if ((h.kind == Kind::Tile || h.kind == Kind::AddTile) && h.a == s) return &h;
        return nullptr;
    };
    auto wire = [&](const Hit* a, const Hit* b, unsigned tone, float alpha) {
        if (!a || !b || a->kind != Kind::Tile || b->kind != Kind::Tile) return;
        const float x0 = a->x + a->w, y0 = a->y + a->h * 0.5f, x1 = b->x, y1 = b->y + b->h * 0.5f;
        nvgBeginPath(vg);
        nvgMoveTo(vg, x0, y0);
        nvgBezierTo(vg, x0 + 20, y0, x1 - 20, y1, x1, y1);
        nvgStrokeColor(vg, rgba(tone, alpha));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    };
    const Hit* f1 = tileOf(so::kFilter1);
    const Hit* f2 = tileOf(so::kFilter2);
    const Hit* drive = tileOf(so::kDrive);
    const Hit* amp = tileOf(so::kAmp);
    const Hit* post = drive && drive->kind == Kind::Tile ? drive : amp;
    for (int i = 0; i < so::kOscCount; ++i) {
        const Hit* o = tileOf(so::osc(i));
        if (!o || o->kind != Kind::Tile) continue;
        switch (st.to[static_cast<std::size_t>(i)]) {
            case so::OscTarget::Filter1:
                wire(o, f1, col::accent, 0.8f);
                break;
            case so::OscTarget::Filter2:
                wire(o, f2, col::accent, 0.8f);
                break;
            case so::OscTarget::Both:
                wire(o, f1, col::accent, 0.8f);
                wire(o, f2, col::accent, 0.8f);
                break;
            case so::OscTarget::Direct:
                wire(o, post, col::accent, 0.5f);
                break;
            case so::OscTarget::None:
                break;
        }
    }
    if (st.routing == so::FilterRouting::Serial && st.filter[0] && st.filter[1]) {
        nvgBeginPath(vg);
        nvgMoveTo(vg, f1->x + f1->w * 0.5f, f1->y + f1->h);
        nvgLineTo(vg, f2->x + f2->w * 0.5f, f2->y);
        nvgStrokeColor(vg, rgba(col::accent, 0.8f));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
        wire(f2, post, col::accent, 0.8f);
    } else {
        wire(f1, post, col::accent, 0.8f);
        wire(f2, post, col::accent, 0.8f);
    }
    // amp → fx chain → output.
    {
        const Hit* out = tileOf(so::kOutput);
        const Hit* prev = amp;
        for (so::FxKind k : st.chain) {
            const Hit* fxTile = tileOf(fxSlot(k));
            if (prev == amp)
                wire(amp, fxTile, col::accent, 0.8f);
            else if (prev && fxTile) {
                nvgBeginPath(vg);
                nvgMoveTo(vg, prev->x + prev->w * 0.5f, prev->y + prev->h);
                nvgLineTo(vg, fxTile->x + fxTile->w * 0.5f, fxTile->y);
                nvgStrokeColor(vg, rgba(col::accent, 0.8f));
                nvgStrokeWidth(vg, 1.5f);
                nvgStroke(vg);
            }
            prev = fxTile;
        }
        if (prev && out) {
            const float x0 = prev == amp ? amp->x + amp->w * 0.5f : prev->x;
            const float y0 = prev == amp ? amp->y + amp->h : prev->y + prev->h * 0.5f;
            const float x1 = prev == amp ? out->x + out->w * 0.5f : out->x + out->w;
            const float y1 = prev == amp ? out->y : out->y + out->h * 0.5f;
            nvgBeginPath(vg);
            nvgMoveTo(vg, x0, y0);
            if (prev == amp)
                nvgLineTo(vg, x1, y1);
            else
                nvgBezierTo(vg, x0 - 20, y0, x1 + 20, y1, x1, y1);
            nvgStrokeColor(vg, rgba(col::accent, 0.8f));
            nvgStrokeWidth(vg, 1.5f);
            nvgStroke(vg);
        }
    }
    if (post == drive) {
        nvgBeginPath(vg);
        nvgMoveTo(vg, drive->x + drive->w * 0.5f, drive->y + drive->h);
        nvgLineTo(vg, amp->x + amp->w * 0.5f, amp->y);
        nvgStrokeColor(vg, rgba(col::accent, 0.8f));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }

    for (const Hit& h : hits_) {
        char detail[64] = "";
        const so::Slot s = static_cast<so::Slot>(h.a);
        switch (h.kind) {
            case Kind::Tile:
            case Kind::AddTile: {
                const so::Block b = so::slotInfo(s).block;
                if (b != so::Block::Osc && b != so::Block::Filter && b != so::Block::Drive &&
                    b != so::Block::Amp && b != so::Block::Output && b < so::Block::Chorus)
                    break;
                if (h.kind == Kind::Tile) {
                    if (b == so::Block::Osc) {
                        const auto k = static_cast<std::size_t>(s - so::kOsc1);
                        const std::size_t wi = so::index(s, so::OscF::wave);
                        const bool hasWave = so::paramActive(st, wi);
                        std::snprintf(detail, sizeof(detail), "%s%s%s  %s",
                                      std::string(so::oscTypeName(*st.osc[k])).c_str(),
                                      hasWave ? " · " : "",
                                      hasWave ? choiceLabel(spec(wi), host_.value(wi)).c_str() : "",
                                      targetName(st.to[k]).c_str());
                    } else if (b == so::Block::Filter) {
                        const auto k = static_cast<std::size_t>(s - so::kFilter1);
                        const std::size_t ci = so::index(s, so::FilterF::cutoff);
                        std::snprintf(detail, sizeof(detail), "%s · %s",
                                      std::string(so::filterTypeName(*st.filter[k])).c_str(),
                                      params::format(spec(ci), host_.value(ci)).c_str());
                    } else if (b == so::Block::Drive) {
                        constexpr const char* kD[] = {"off", "soft", "tape", "fold", "hard"};
                        std::snprintf(detail, sizeof(detail), "%s",
                                      kD[static_cast<std::size_t>(st.drive)]);
                    } else if (b == so::Block::Amp || b == so::Block::Output) {
                        const std::size_t li = b == so::Block::Amp
                                                   ? so::index(s, so::AmpF::level)
                                                   : so::index(s, so::OutputF::level);
                        std::snprintf(detail, sizeof(detail), "%s",
                                      params::format(spec(li), host_.value(li)).c_str());
                    }
                }
                drawTile(vg, h, tileTitle(s).c_str(), detail, col::accent);
                break;
            }
            case Kind::AddFx:
                drawTile(vg, h, "FX", nullptr, col::accent);
                break;
            case Kind::Routing:
                pill(vg, h.x, h.y, h.w, h.h,
                     st.routing == so::FilterRouting::Serial ? "serial" : "parallel", true,
                     rgb(col::accent), false,
                     hover_ >= 0 && &hits_[static_cast<std::size_t>(hover_)] == &h);
                break;
            default:
                break;
        }
    }
}

void Editor::drawModulators(NVGcontext* vg) {
    cell(vg, kModX, kModY, kModW, kModH, "Modulation");
    const so::Structure& st = host_.structure();
    for (const Hit& h : hits_) {
        if (h.kind != Kind::Tile && h.kind != Kind::AddTile) continue;
        const so::Slot s = static_cast<so::Slot>(h.a);
        const so::Block b = so::slotInfo(s).block;
        char detail[64] = "";
        if (b == so::Block::Env && h.kind == Kind::Tile) {
            const std::size_t a = so::index(s, so::EnvF::attack),
                              r = so::index(s, so::EnvF::release);
            std::snprintf(detail, sizeof(detail), "A %s · R %s",
                          params::format(spec(a), host_.value(a)).c_str(),
                          params::format(spec(r), host_.value(r)).c_str());
        } else if (b == so::Block::Lfo && h.kind == Kind::Tile) {
            const std::size_t w = so::index(s, so::LfoF::wave), r = so::index(s, so::LfoF::rate);
            std::snprintf(detail, sizeof(detail), "%s · %s",
                          choiceLabel(spec(w), host_.value(w)).c_str(),
                          params::format(spec(r), host_.value(r)).c_str());
        } else if (b == so::Block::Step && h.kind == Kind::Tile) {
            std::snprintf(detail, sizeof(detail), "%zu steps",
                          st.stepValues[static_cast<std::size_t>(s - so::kStep1)].size());
        } else if (b == so::Block::Macro) {
            int routed = 0;
            for (const so::Route& r : st.routes)
                if (r.source >= so::Source::Macro1 && r.source <= so::Source::Macro8) ++routed;
            std::snprintf(detail, sizeof(detail), "%d routed", routed);
        } else if (b == so::Block::Voice) {
            const std::size_t m = so::index(s, so::VoiceF::mode),
                              u = so::index(s, so::VoiceF::unison);
            std::snprintf(detail, sizeof(detail), "%s · %d×",
                          choiceLabel(spec(m), host_.value(m)).c_str(),
                          static_cast<int>(host_.value(u)));
        } else if (b != so::Block::Env && b != so::Block::Lfo && b != so::Block::Step) {
            continue;
        }
        drawTile(vg, h, b == so::Block::Macro ? "MACROS" : tileTitle(s).c_str(), detail, col::cool);
    }
}

void Editor::drawKnob(NVGcontext* vg, const Hit& h, std::size_t index) {
    const params::ParamSpec& p = spec(index);
    const double v = host_.value(index);
    const float t = static_cast<float>(params::toNormalized(p, v));
    const float cx = h.x + h.w * 0.5f, cy = h.y + 24, r = 17;
    const bool hot = (hover_ >= 0 && &hits_[static_cast<std::size_t>(hover_)] == &h) ||
                     (drag_.kind == Drag::Param && drag_.target == static_cast<int>(index));
    const bool active = so::paramActive(host_.structure(), index);
    constexpr float a0 = 0.75f * 3.14159265f, a1 = 2.25f * 3.14159265f;
    nvgBeginPath(vg);
    nvgArc(vg, cx, cy, r, a0, a1, NVG_CW);
    nvgStrokeColor(vg, rgb(col::knobWell));
    nvgStrokeWidth(vg, 4);
    nvgStroke(vg);
    const bool bipolar = p.min < 0.0 && p.max > 0.0;
    const float tz = bipolar ? static_cast<float>(params::toNormalized(p, 0.0)) : 0.0f;
    const float from = a0 + (a1 - a0) * std::min(t, tz), to = a0 + (a1 - a0) * std::max(t, tz);
    if (to > from + 1e-3f) {
        nvgBeginPath(vg);
        nvgArc(vg, cx, cy, r, from, to, NVG_CW);
        nvgStrokeColor(vg,
                       rgba(h.kind == Kind::Macro ? col::cool : col::accent, active ? 1.0f : 0.4f));
        nvgStrokeWidth(vg, 4);
        nvgStroke(vg);
    }
    nvgBeginPath(vg);
    nvgCircle(vg, cx, cy, r - 5);
    nvgFillPaint(
        vg, nvgLinearGradient(vg, cx, cy - r, cx, cy + r, rgb(col::knobTop), rgb(col::knobBot)));
    nvgFill(vg);
    nvgStrokeColor(vg, rgba(col::knobRim, hot ? 1.0f : 0.7f));
    nvgStrokeWidth(vg, 1);
    nvgStroke(vg);
    const float a = a0 + (a1 - a0) * t;
    nvgBeginPath(vg);
    nvgMoveTo(vg, cx + std::cos(a) * (r - 11), cy + std::sin(a) * (r - 11));
    nvgLineTo(vg, cx + std::cos(a) * (r - 5), cy + std::sin(a) * (r - 5));
    nvgStrokeColor(vg, rgb(col::bone));
    nvgStrokeWidth(vg, 2);
    nvgStroke(vg);
    const auto loc = so::locate(index);
    std::string name(so::fieldLabel(so::slotInfo(loc.slot).block, loc.field));
    if (h.kind == Kind::Macro) {
        const int m = so::locate(index).slot - so::kMacro1;
        const std::string& custom = host_.structure().macroName[static_cast<std::size_t>(m)];
        name = custom.empty() ? "Macro " + std::to_string(m + 1) : custom;
    }
    label(vg, name.c_str(), cx, cy + r + 12, 10, rgb(active ? col::dust : col::dim), Align::Center,
          true);
    num(vg, params::format(p, v).c_str(), cx, cy + r + 25, 10, rgb(active ? col::bone : col::dim),
        Align::Center);
    if (focus_ == static_cast<int>(index)) {
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, r + 6);
        nvgStrokeColor(vg, rgba(col::bone, 0.7f));
        nvgStrokeWidth(vg, 1);
        nvgStroke(vg);
    }
}

void Editor::drawInspector(NVGcontext* vg) {
    const so::Structure& st = host_.structure();
    std::string title =
        selected_ == so::kMacro1 ? "Macros" : std::string(so::slotInfo(selected_).group);
    cell(vg, kInspX, kInspY, kInspW, kInspH, title.c_str());
    for (const Hit& h : hits_) {
        const bool hot = hover_ >= 0 && &hits_[static_cast<std::size_t>(hover_)] == &h;
        switch (h.kind) {
            case Kind::Knob:
            case Kind::Macro:
                drawKnob(vg, h, static_cast<std::size_t>(h.a));
                break;
            case Kind::Choice: {
                const auto i = static_cast<std::size_t>(h.a);
                const params::ParamSpec& p = spec(i);
                const auto loc = so::locate(i);
                const std::string name(so::fieldLabel(so::slotInfo(loc.slot).block, loc.field));
                label(vg, name.c_str(), h.x + h.w * 0.5f, h.y + 14, 10, rgb(col::dust),
                      Align::Center, true);
                pill(vg, h.x + 6, h.y + 26, h.w - 12, 22, choiceLabel(p, host_.value(i)).c_str(),
                     true, rgb(col::accent), false, hot || focus_ == h.a);
                break;
            }
            case Kind::TypePill: {
                std::string t;
                const so::Block b = so::slotInfo(selected_).block;
                if (b == so::Block::Osc)
                    t = std::string(
                        so::oscTypeName(*st.osc[static_cast<std::size_t>(selected_ - so::kOsc1)]));
                else if (b == so::Block::Filter)
                    t = std::string(so::filterTypeName(
                        *st.filter[static_cast<std::size_t>(selected_ - so::kFilter1)]));
                else {
                    constexpr const char* kD[] = {"off", "soft", "tape", "fold", "hard"};
                    t = kD[static_cast<std::size_t>(st.drive)];
                }
                t += " ▾";
                pill(vg, h.x, h.y, h.w, h.h, t.c_str(), true, rgb(col::accent), false, hot);
                break;
            }
            case Kind::TargetPill:
                pill(vg, h.x, h.y, h.w, h.h,
                     targetName(st.to[static_cast<std::size_t>(h.a)]).c_str(), false,
                     rgb(col::accent), false, hot);
                break;
            case Kind::Remove:
                pill(vg, h.x, h.y, h.w, h.h, "Remove", false, rgb(col::alert), false, hot);
                break;
            default:
                break;
        }
    }
    if (inspectorParams().empty())
        label(vg, "No parameters for this module in the current patch.", kInspX + 16, kInspY + 60,
              11, rgb(col::dim));
}

void Editor::drawRoutes(NVGcontext* vg) {
    const so::Structure& st = host_.structure();
    char title[48];
    std::snprintf(title, sizeof(title), "Modulation routes · %zu", st.routes.size());
    cell(vg, kRouteX, kRouteY, kRouteW, kRouteH, title);
    int shown = 0;
    for (const Hit& h : hits_) {
        const bool hot = hover_ >= 0 && &hits_[static_cast<std::size_t>(hover_)] == &h;
        if (h.kind == Kind::AddRoute) {
            pill(vg, h.x, h.y, h.w, h.h, "+ Route", false, rgb(col::cool), false, hot);
        } else if (h.kind == Kind::RouteAmount) {
            const so::Route& r = st.routes[static_cast<std::size_t>(h.a)];
            const auto src = edits::sourceSlot(r.source);
            const bool mine = (src && *src == selected_) || edits::destSlot(r.dest) == selected_;
            const float y = h.y + h.h * 0.5f;
            if (mine)
                fillRoundRect(vg, kRouteX + 10, h.y, kRouteW - 20, h.h, 3, rgba(col::cool, 0.10f));
            std::string text =
                std::string(so::sourceName(r.source)) + "  →  " + so::destName(r.dest);
            label(vg, text.c_str(), kRouteX + 16, y, 11, rgb(mine ? col::bone : col::dust));
            fillRoundRect(vg, h.x, h.y + 2, h.w, h.h - 4, 3,
                          rgb(hot || drag_.target == h.a ? col::knobTop : col::well));
            num(vg, fmtAmount(r).c_str(), h.x + h.w - 8, y, 10.5f, rgb(col::cool), Align::Right);
            if (r.via) {
                const std::string via = "× " + std::string(so::sourceName(*r.via));
                label(vg, via.c_str(), h.x + 8, y, 10, rgb(col::dim));
            }
            ++shown;
        } else if (h.kind == Kind::RouteRemove) {
            label(vg, "×", h.x + h.w * 0.5f, h.y + h.h * 0.5f, 13, rgb(hot ? col::alert : col::dim),
                  Align::Center);
        }
    }
    if (st.routes.empty())
        label(vg,
              "No routes yet: \"+ Route\" connects a source (envelope, LFO, macro, …) to a "
              "parameter.",
              kRouteX + 16, kRouteY + 52, 11, rgb(col::dim));
    else if (shown < static_cast<int>(st.routes.size())) {
        char more[48];
        std::snprintf(more, sizeof(more), "+ %zu more in the patch",
                      st.routes.size() - static_cast<std::size_t>(shown));
        label(vg, more, kRouteX + 16, kRouteY + kRouteH - 10, 10, rgb(col::dim));
    }
}

void Editor::drawMenu(NVGcontext* vg) {
    if (!menu_.open) return;
    const int n = static_cast<int>(menu_.items.size());
    const int cols = std::max(1, (n + kMenuRows - 1) / kMenuRows);
    const float w = menu_.w * static_cast<float>(cols);
    const float h = 26 + static_cast<float>(std::min(n, kMenuRows)) * kMenuRowH + 6;
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, kDesignW, kDesignH);
    nvgFillColor(vg, rgba(col::ink, 0.35f));
    nvgFill(vg);
    fillRoundRect(vg, menu_.x, menu_.y, w, h, 5, rgb(col::panel));
    strokeRoundRect(vg, menu_.x + 0.5f, menu_.y + 0.5f, w - 1, h - 1, 5, rgb(col::edgeHi));
    label(vg, menu_.title.c_str(), menu_.x + 10, menu_.y + 14, 10, rgb(col::accent), Align::Left,
          true);
    for (const Hit& it : hits_) {
        if (it.kind != Kind::MenuItem) continue;
        if (it.a == menu_.hover)
            fillRoundRect(vg, it.x + 3, it.y, it.w - 6, it.h, 3, rgb(col::knobTop));
        label(vg, menu_.items[static_cast<std::size_t>(it.a)].c_str(), it.x + 10,
              it.y + it.h * 0.5f, 11, rgb(it.a == menu_.hover ? col::bone : col::dust));
    }
}

}  // namespace orrery::gui
