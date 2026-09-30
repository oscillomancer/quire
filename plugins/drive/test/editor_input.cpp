// SPDX-License-Identifier: MIT
#include <cstdio>
#include "Editor.h"
using namespace supple::drive;
using namespace supple::drive::gui;
struct Host : EditorHost {
    Values v = defaults();
    TelemetryRing ring;
    int begins = 0, ends = 0;
    double paramValue(uint32_t i) const override { return v[i]; }
    void beginEdit(uint32_t) override { ++begins; }
    void setParam(uint32_t i, double x) override { v[i] = x; }
    void endEdit(uint32_t) override { ++ends; }
    TelemetryRing& telemetry() override { return ring; }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000; }
};
int main() {
    Host h;
    Editor e(h);
    int fail = 0;
    auto check = [&](bool ok) {
        if (!ok) ++fail;
    };
    for (int id = 0; id < 8; ++id) {
        e.onKey(EditorKey::Tab, false);
        e.onKey(EditorKey::End, false);
    }
    check(h.begins == h.ends);
    check(h.v[kOutput] == 24);
    e.onFocusLost();
    e.onScroll(90, 345, -1);
    check(h.begins == h.ends + 1);
    e.pump(.24);
    check(h.begins == h.ends + 1);
    e.pump(.02);
    check(h.begins == h.ends);
    e.onMouseDown(90, 345);
    e.onMouseMove(90, 330, false);
    e.onPointerLeave();
    check(h.begins == h.ends);
    e.onDoubleClick(240, 345);
    check(h.v[kBias] == 0);
    check(h.begins == h.ends);
    e.onMouseDown(100, 282);
    check(h.v[kCurve] == 0);
    check(h.begins == h.ends);
    e.onMouseDown(490, 282);
    check(h.v[kOversampling] == 0);
    check(h.begins == h.ends);
    e.onScroll(370, 345, -1);
    e.onFocusLost();
    check(h.begins == h.ends);
    e.onScroll(100, 282, -1);
    check(h.v[kCurve] == 1);
    e.pump(.25);
    check(h.begins == h.ends);
    e.onKey(EditorKey::Right, false);
    check(h.v[kCurve] == 2);
    check(h.begins == h.ends);
    e.onMouseDown(90, 345);
    e.onScroll(370, 345, -1);
    e.onFocusLost();
    check(h.begins == h.ends);
    std::printf("editor-input: %d failures\n", fail);
    return fail ? 1 : 0;
}
