// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The editor against the real CLAP core (no window, no GL): what it lays out
// follows the patch structure, gestures reach the host balanced, and every
// structural action goes through the codec.
#include <clap/clap.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "../plugin/gui/Editor.h"
#include "orrery/clap_plugin.h"
#include "orrery/params.h"

namespace so = supple::orrery;
using Kind = orrery::gui::Editor::Hit::Kind;

namespace {

int failures = 0;
#define EXPECT(cond)                                                                       \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::fprintf(stderr, "%s:%d: EXPECT(%s) failed\n", __FILE__, __LINE__, #cond); \
            ++failures;                                                                    \
        }                                                                                  \
    } while (0)

struct Host {
    clap_host_t host{};
    clap_host_params_t params{};
    Host() {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "editor-test";
        host.vendor = host.url = host.version = "";
        host.get_extension = [](const clap_host_t* h, const char* id) -> const void* {
            return std::string(id) == CLAP_EXT_PARAMS ? &static_cast<Host*>(h->host_data)->params
                                                      : nullptr;
        };
        host.request_restart = host.request_process =
            host.request_callback = [](const clap_host_t*) {};
        params.rescan = [](const clap_host_t*, clap_param_rescan_flags) {};
        params.clear = [](const clap_host_t*, clap_id, clap_param_clear_flags) {};
        params.request_flush = [](const clap_host_t*) {};
    }
};

so::EditorBridge* captured = nullptr;
const so::ClapExtensions kCapture = {
    [](const clap_plugin_t*, const clap_host_t*, so::EditorBridge& b) -> void* {
        captured = &b;
        return &b;
    },
    [](void*) {}, nullptr, nullptr};

// Counts gesture events the plugin reports through params.flush.
struct Flush {
    clap_input_events_t in{};
    clap_output_events_t out{};
    int begins = 0, ends = 0, values = 0;
    Flush() {
        in.ctx = this;
        in.size = [](const clap_input_events_t*) -> uint32_t { return 0; };
        in.get = [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
            return nullptr;
        };
        out.ctx = this;
        out.try_push = [](const clap_output_events_t* o, const clap_event_header_t* h) {
            auto* f = static_cast<Flush*>(o->ctx);
            if (h->type == CLAP_EVENT_PARAM_GESTURE_BEGIN) ++f->begins;
            if (h->type == CLAP_EVENT_PARAM_GESTURE_END) ++f->ends;
            if (h->type == CLAP_EVENT_PARAM_VALUE) ++f->values;
            return true;
        };
    }
    void run(const clap_plugin_t* p) {
        auto* ext = static_cast<const clap_plugin_params_t*>(p->get_extension(p, CLAP_EXT_PARAMS));
        ext->flush(p, &in, &out);
    }
};

const orrery::gui::Editor::Hit* find(const orrery::gui::Editor& ed, Kind k, int a = -1) {
    for (const auto& h : ed.hits())
        if (h.kind == k && (a < 0 || h.a == a)) return &h;
    return nullptr;
}

void click(orrery::gui::Editor& ed, const orrery::gui::Editor::Hit* h) {
    if (!h) return;
    ed.onPointerDown(h->x + h->w * 0.5f, h->y + h->h * 0.5f, 0, 0);
    ed.onPointerUp(h->x + h->w * 0.5f, h->y + h->h * 0.5f, 0, 0);
    ed.layout();
}

void pickMenu(orrery::gui::Editor& ed, const std::string& item) {
    ed.layout();
    const auto& items = ed.menuItems();
    for (std::size_t i = 0; i < items.size(); ++i)
        if (items[i] == item) {
            click(ed, find(ed, Kind::MenuItem, static_cast<int>(i)));
            return;
        }
    std::fprintf(stderr, "menu has no item \"%s\"\n", item.c_str());
    ++failures;
}

std::size_t idx(std::string_view key) {
    return supple::params::indexOf(so::table(), *supple::params::findByKey(so::table(), key));
}

}  // namespace

int main() {
    Host h;
    const clap_plugin_t* p = so::createClapPlugin(&h.host, &kCapture);
    if (!p || !p->init(p) || !captured) return 1;
    so::EditorBridge& bridge = *captured;
    {
        orrery::gui::Editor ed(bridge);
        ed.layout();
        const so::Structure& st = bridge.structure();

        // The diagram follows the structure.
        EXPECT(find(ed, Kind::Tile, so::kOsc1) != nullptr);
        EXPECT(find(ed, st.osc[1] ? Kind::Tile : Kind::AddTile, so::kOsc2) != nullptr);
        EXPECT(find(ed, Kind::Tile, so::kAmp) != nullptr);

        // Adding an oscillator selects it and shows its parameters.
        click(ed, find(ed, Kind::AddTile, so::kOsc2));
        EXPECT(bridge.structure().osc[1].has_value());
        EXPECT(ed.selected() == so::kOsc2);
        EXPECT(find(ed, Kind::Knob, static_cast<int>(idx("osc2.level"))) != nullptr);

        // A knob drag is one balanced gesture that moves the value.
        const auto* level = find(ed, Kind::Knob, static_cast<int>(idx("osc2.level")));
        const double before = bridge.value(idx("osc2.level"));
        ed.onPointerDown(level->x + 5, level->y + 20, 0, 0);
        for (int i = 1; i <= 10; ++i)
            ed.onPointerMove(level->x + 5, level->y + 20 - static_cast<float>(i) * 3, 0);
        ed.onPointerUp(level->x + 5, level->y - 10, 0, 0);
        EXPECT(bridge.value(idx("osc2.level")) != before);
        Flush f;
        f.run(p);
        EXPECT(f.begins == 1 && f.ends == 1 && f.values >= 1);

        // A pointer leaving mid-drag still closes the gesture.
        ed.onPointerDown(level->x + 5, level->y + 20, 0, 0);
        ed.onPointerMove(level->x + 5, level->y, 0);
        ed.onPointerLeave();
        Flush g;
        g.run(p);
        EXPECT(g.begins == g.ends);

        // Choice parameters open a menu of their choices.
        ed.layout();
        const auto* wave = find(ed, Kind::Choice, static_cast<int>(idx("osc2.wave")));
        EXPECT(wave != nullptr);
        click(ed, wave);
        EXPECT(ed.menuOpen());
        pickMenu(ed, "Square");
        EXPECT(!ed.menuOpen());
        EXPECT(std::lround(bridge.value(idx("osc2.wave"))) == static_cast<long>(so::Wave::Square));

        // Type changes go through the codec.
        click(ed, find(ed, Kind::TypePill, so::kOsc2));
        EXPECT(ed.menuOpen());
        pickMenu(ed, "fm");
        EXPECT(bridge.structure().osc[1] == so::OscType::Fm);

        // An LFO, then a route from it through the three route menus.
        click(ed, find(ed, Kind::AddTile, so::kLfo1));
        EXPECT(bridge.structure().lfo[0]);
        const std::size_t routes = bridge.structure().routes.size();
        click(ed, find(ed, Kind::AddRoute));
        pickMenu(ed, "lfo1");
        pickMenu(ed, "OSC 1");
        pickMenu(ed, "osc1.pitch");
        EXPECT(bridge.structure().routes.size() == routes + 1);

        // Dragging a route amount changes it in the patch.
        ed.layout();
        const auto* amount = find(ed, Kind::RouteAmount);
        EXPECT(amount != nullptr);
        if (amount) {
            const int r = amount->a;
            const double a0 = bridge.structure().routes[static_cast<std::size_t>(r)].amount;
            ed.onPointerDown(amount->x + 10, amount->y + 5, 0, 0);
            ed.onPointerMove(amount->x + 10, amount->y - 40, 0);
            ed.onPointerUp(amount->x + 10, amount->y - 40, 0, 0);
            EXPECT(bridge.structure().routes[static_cast<std::size_t>(r)].amount > a0);
        }

        // Escape closes a menu; removing a module removes its routes too.
        click(ed, find(ed, Kind::AddRoute));
        EXPECT(ed.menuOpen());
        ed.onKeyCode(orrery::gui::key::kEscape, 0);
        EXPECT(!ed.menuOpen());
        click(ed, find(ed, Kind::Tile, so::kLfo1));
        click(ed, find(ed, Kind::Remove, so::kLfo1));
        EXPECT(!bridge.structure().lfo[0]);
        EXPECT(bridge.structure().routes.size() == routes);
        EXPECT(ed.status().empty());

        Flush drained;  // everything so far reaches the host first
        drained.run(p);
        // A wheel edit is one gesture, closed after 250 ms without wheel events.
        click(ed, find(ed, Kind::Tile, so::kOsc1));
        const auto* l1 = find(ed, Kind::Knob, static_cast<int>(idx("osc1.level")));
        EXPECT(l1 != nullptr);
        if (l1) {
            ed.onWheel(l1->x + 5, l1->y + 20, 0, 1, 0);
            ed.onWheel(l1->x + 5, l1->y + 20, 0, 1, 0);
            ed.pump(0.1);
            Flush w1;
            w1.run(p);
            EXPECT(w1.begins == 1 && w1.ends == 0);
            ed.pump(0.3);
            Flush w2;
            w2.run(p);
            EXPECT(w2.ends == 1);
        }

        // A second press mid-drag (on a choice, which opens a menu) ends the
        // drag: nothing moves under the menu and the gestures stay balanced.
        Flush drained2;
        drained2.run(p);
        ed.layout();
        const auto* knob = find(ed, Kind::Knob, static_cast<int>(idx("osc1.level")));
        const auto* choice = find(ed, Kind::Choice, static_cast<int>(idx("osc1.wave")));
        EXPECT(knob != nullptr && choice != nullptr);
        if (knob && choice) {
            ed.onPointerDown(knob->x + 5, knob->y + 20, 0, 0);
            ed.onPointerMove(knob->x + 5, knob->y + 5, 0);
            ed.onPointerDown(choice->x + choice->w * 0.5f, choice->y + choice->h * 0.5f, 1, 0);
            EXPECT(ed.menuOpen());
            EXPECT(!ed.dragging());
            const double held = bridge.value(idx("osc1.level"));
            ed.onPointerMove(knob->x + 5, knob->y - 60, 0);
            EXPECT(bridge.value(idx("osc1.level")) == held);
            ed.onPointerUp(knob->x + 5, knob->y - 60, 0, 0);
            ed.onKeyCode(orrery::gui::key::kEscape, 0);
            Flush m;
            m.run(p);
            EXPECT(m.begins == m.ends);
        }
        // The bridge closes an outstanding gesture when another begins.
        bridge.beginEdit(idx("osc1.level"));
        bridge.beginEdit(idx("osc1.fine"));
        bridge.endEdit(idx("osc1.fine"));
        Flush b2;
        b2.run(p);
        EXPECT(b2.begins == 2 && b2.ends == 2);
    }
    p->destroy(p);
    if (failures)
        std::fprintf(stderr, "%d failure(s)\n", failures);
    else
        std::printf("editor-input: all checks passed\n");
    return failures ? 1 : 0;
}
