// SPDX-License-Identifier: MIT
#include <cstdio>
#include "Editor.h"
using namespace supple::comp;
using namespace supple::comp::gui;
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
    auto check = [&](bool b, const char* name) {
        std::printf("%s: %s\n", name, b ? "ok" : "FAIL");
        if (!b) ++fail;
    };
    for (int i = 0; i < 20; ++i) e.onScroll(55, 358, -1);
    check(h.begins == 1 && h.ends == 0 && h.sets == 20, "wheel coalesces gesture");
    e.pump(.24);
    check(h.ends == 0, "wheel remains open");
    e.pump(.02);
    check(h.ends == 1, "wheel closes at 250ms");
    check(e.onMouseDown(230, 34) && h.values[kMode] == 1 && h.begins == h.ends,
          "mode toggle before drawing");
    e.onMouseDown(55, 358);
    e.onMouseMove(55, 345, false);
    e.onPointerLeave();
    check(h.begins == h.ends, "pointer leave ends drag");
    e.onScroll(125, 358, 1);
    e.onMouseDown(195, 358);
    e.onFocusLost();
    check(h.begins == h.ends, "focus loss closes wheel and drag");
    e.onDoubleClick(125, 358);
    check(h.values[kRatio] == 4 && h.begins == h.ends, "double click resets ratio");
    Host keys;
    Editor keyboard(keys);
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::Space, false);
    check(keys.values[kMode] == 1, "keyboard switches mode");
    keyboard.onKey(EditorKey::Tab, false);
    const auto t = keys.values[kThreshold];
    keyboard.onKey(EditorKey::Right, true);
    check(keys.values[kThreshold] > t && keys.begins == keys.ends, "fine keyboard edit balanced");
    keyboard.onKey(EditorKey::Home, false);
    check(keys.values[kThreshold] == -60, "Home reaches minimum");
    keyboard.onKey(EditorKey::End, false);
    check(keys.values[kThreshold] == 0, "End reaches maximum");
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
