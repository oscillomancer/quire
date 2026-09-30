// SPDX-License-Identifier: MIT
#include <cmath>
#include <cstdio>
#include "Editor.h"
using namespace supple::reverb;
using namespace supple::reverb::gui;
struct Host : EditorHost {
    Values values = defaults();
    TelemetryRing ring;
    int begins = 0, ends = 0;
    double paramValue(uint32_t id) const override { return values[id]; }
    void beginEdit(uint32_t) override { ++begins; }
    void endEdit(uint32_t) override { ++ends; }
    void setParam(uint32_t id, double v) override { values[id] = v; }
    TelemetryRing& telemetry() override { return ring; }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000; }
};
int main() {
    Host h;
    Editor e(h);
    if (!e.onMouseDown(665, 153)) return 1;
    e.onMouseMove(665, 140, false);
    e.onMouseUp();
    if (h.values[kSize] <= 50 || h.begins != h.ends) return 1;
    e.onDoubleClick(665, 153);
    if (h.values[kSize] != 50) return 1;
    if (!e.onScroll(805, 153, -1)) return 1;
    e.pump(.25);
    if (h.begins != h.ends) return 1;
    e.onMouseDown(825, 463);
    if (h.values[kFreeze] != 1 || h.begins != h.ends) return 1;
    Host keys;
    Editor keyboard(keys);
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::End, false);
    if (keys.values[kSize] != 100) return 1;
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::Home, false);
    if (keys.values[kDecay] != 100) return 1;
    e.onMouseDown(75, 348);
    e.onFocusLost();
    if (h.begins != h.ends || e.dragging()) return 1;
    return 0;
}
