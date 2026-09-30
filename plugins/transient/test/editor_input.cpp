// SPDX-License-Identifier: MIT
#include <cstdio>
#include "Editor.h"
using namespace supple::transient;
using namespace supple::transient::gui;
struct Host : EditorHost {
    Values values = defaults();
    TelemetryRing ring;
    int begins = 0, ends = 0, sets = 0;
    double paramValue(uint32_t id) const override { return values[id]; }
    void beginEdit(uint32_t) override { ++begins; }
    void endEdit(uint32_t) override { ++ends; }
    void setParam(uint32_t id, double v) override {
        values[id] = v;
        ++sets;
    }
    TelemetryRing& telemetry() override { return ring; }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000; }
};
int main() {
    Host h;
    Editor e(h);
    int fail = 0;
    auto check = [&](bool ok, const char* name) {
        std::printf("%s: %s\n", name, ok ? "ok" : "FAIL");
        if (!ok) ++fail;
    };
    for (int i = 0; i < 20; ++i) e.onScroll(105, 419, -1);
    check(h.begins == 1 && h.ends == 0 && h.sets == 20, "wheel coalesces");
    e.pump(.24);
    check(h.ends == 0, "wheel remains open");
    e.pump(.02);
    check(h.ends == 1, "wheel closes at 250ms");
    e.onMouseDown(105, 419);
    e.onMouseMove(105, 400, false);
    e.onPointerLeave();
    check(h.begins == h.ends, "pointer leave closes drag");
    e.onScroll(240, 419, 1);
    e.onMouseDown(390, 419);
    e.onFocusLost();
    check(h.begins == h.ends, "focus loss balances gestures");
    e.onDoubleClick(105, 419);
    check(h.values[kAttack] == 0, "double click resets bipolar value");
    check(e.onMouseDown(600, 419) && h.values[kClip] == 1, "clip toggle works before drawing");
    e.onKey(EditorKey::Space, false);
    check(h.values[kClip] == 0, "Space toggles clip");
    e.onFocusLost();
    e.onKey(EditorKey::Tab, false);
    e.onKey(EditorKey::Right, true);
    check(h.values[kAttack] > 0, "Tab and fine arrows edit attack");
    e.onKey(EditorKey::Home, false);
    check(h.values[kAttack] == -100, "Home bipolar minimum");
    e.onKey(EditorKey::End, false);
    check(h.values[kAttack] == 100, "End bipolar maximum");
    e.onKey(EditorKey::Tab, true);
    e.onKey(EditorKey::Home, false);
    check(h.values[kOutput] == -24, "reverse Tab wraps to output");
    e.onFocusLost();
    check(h.begins == h.ends, "keyboard gestures balanced");
    Ring<int, 1> ring;
    ring.push(1);
    ring.push(2);
    check(!ring.push(3), "full queue drops newest");
    int v = 0;
    ring.peek(v);
    check(v == 1, "peek retains event");
    ring.consume();
    ring.pop(v);
    check(v == 2, "queue preserves second event");
    return fail ? 1 : 0;
}
