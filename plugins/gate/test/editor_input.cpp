// SPDX-License-Identifier: MIT
#include <cstdio>
#include "Editor.h"
using namespace gate;
using namespace gate::gui;
struct Host : EditorHost {
    Values values = defaults();
    TelemetryRing ring;
    int begins = 0, ends = 0;
    double paramValue(uint32_t id) const override { return values[id]; }
    void beginEdit(uint32_t) override { ++begins; }
    void setParam(uint32_t id, double v) override { values[id] = v; }
    void endEdit(uint32_t) override { ++ends; }
    TelemetryRing& telemetry() override { return ring; }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000; }
};
int main() {
    int bad = 0;
    auto check = [&](bool ok, const char* message) {
        std::printf("%s: %s\n", message, ok ? "ok" : "FAIL");
        if (!ok) ++bad;
    };
    Host host;
    Editor e(host);
    check(e.onMouseDown(60, 355) && host.values[kMode] == 1, "mode works before first render");
    check(host.begins == host.ends, "enum click balances gesture");
    e.onMouseDown(160, 355);
    e.onMouseMove(160, 330, false);
    e.onMouseUp();
    check(host.values[kThreshold] > -40, "drag changes physical threshold");
    e.onScroll(160, 355, -1);
    e.onScroll(160, 355, -1);
    check(host.begins == host.ends + 1, "wheel coalesces gesture");
    e.pump(.24);
    check(host.begins == host.ends + 1, "wheel waits 250ms");
    e.pump(.02);
    check(host.begins == host.ends, "wheel closes after 250ms");
    e.onScroll(160, 355, 1);
    e.onFocusLost();
    check(host.begins == host.ends, "focus loss balances gesture");
    e.onDoubleClick(160, 355);
    check(host.values[kThreshold] == -40, "double click restores default");
    e.onFocusLost();
    e.onKey(EditorKey::Tab, false);
    e.onKey(EditorKey::Tab, false);
    e.onKey(EditorKey::End, false);
    check(host.values[kThreshold] == 0, "keyboard reaches bound");
    e.onKey(EditorKey::Home, false);
    check(host.values[kThreshold] == -80, "keyboard reaches lower bound");
    e.onKey(EditorKey::Right, true);
    check(host.values[kThreshold] > -80 && host.values[kThreshold] < -79,
          "fine key step remains physical float");
    e.onFocusLost();
    check(host.begins == host.ends, "all gestures balanced");
    Ring<int, 1> ring;
    ring.push(1);
    ring.push(2);
    check(!ring.push(3), "full ring drops new entry");
    int v;
    check(ring.pop(v) && v == 1, "ring preserves unread value");
    return bad ? 1 : 0;
}
