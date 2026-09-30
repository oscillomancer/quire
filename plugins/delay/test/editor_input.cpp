// SPDX-License-Identifier: MIT
#include <cstdio>
#include "Editor.h"
using namespace supple::delay;
using namespace supple::delay::gui;
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
    auto check = [&](bool b, const char* text) {
        std::printf("%s: %s\n", text, b ? "ok" : "FAIL");
        if (!b) ++fail;
    };
    for (int i = 0; i < 20; ++i) e.onScroll(178, 354, -1);
    check(h.begins == 1 && h.ends == 0 && h.sets == 20, "wheel coalesces gesture");
    e.pump(.24);
    check(h.ends == 0, "wheel remains open");
    e.pump(.02);
    check(h.ends == 1, "wheel ends at 250ms");
    e.onMouseDown(178, 354);
    e.onMouseMove(178, 340, false);
    e.onPointerLeave();
    check(h.begins == h.ends, "pointer leave balances drag");
    e.onDoubleClick(178, 354);
    check(h.values[kTime] == 375, "double click restores time");
    e.onMouseDown(802, 354);
    check(h.values[kFreeze] == 1, "freeze toggles");
    e.onKey(EditorKey::Tab, false);
    e.onKey(EditorKey::Right, true);
    check(h.values[kModRate] > .5, "keyboard follows visual order");
    e.onKey(EditorKey::Home, false);
    check(h.values[kModRate] == .05, "Home reaches minimum");
    e.onKey(EditorKey::End, false);
    check(h.values[kModRate] == 10, "End reaches maximum");
    e.onScroll(178, 354, -1);
    e.onFocusLost();
    check(h.begins == h.ends, "focus loss balances wheel");
    for (int id = 0; id < kNumParams; ++id) {
        e.onKey(EditorKey::Tab, false);
        e.onKey(EditorKey::Right, false);
    }
    check(h.begins == h.ends, "all keyboard edits balance");
    Ring<int, 1> ring;
    ring.push(1);
    ring.push(2);
    check(!ring.push(3), "full ring drops newest");
    int v = 0;
    ring.pop(v);
    check(v == 1, "ring preserves first event");
    return fail ? 1 : 0;
}
