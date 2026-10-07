// SPDX-License-Identifier: MIT
#include <cstdio>
#include "Editor.h"
using namespace supple::utility;
using namespace supple::utility::gui;
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
    for (int id = 0; id < 10; ++id) {
        e.onKey(EditorKey::Tab, false);
        e.onKey(EditorKey::End, false);
    }
    check(h.begins == h.ends);
    check(h.v[kMute] == 1);
    e.onFocusLost();
    e.onScroll(62, 106, -1);
    check(h.begins == h.ends + 1);
    e.pump(.24);
    check(h.begins == h.ends + 1);
    e.pump(.02);
    check(h.begins == h.ends);
    e.onMouseDown(62, 106);
    e.onMouseMove(62, 90, false);
    e.onPointerLeave();
    check(h.begins == h.ends);
    e.onMouseDown(477, 218);
    check(h.v[kMute] == 0);
    check(h.begins == h.ends);
    e.onDoubleClick(160, 106);
    check(h.v[kPan] == 0);
    check(h.begins == h.ends);
    e.onScroll(338, 86, -1);
    e.onFocusLost();
    check(h.begins == h.ends);
    check(e.onMouseDown(242, 86));
    check(h.v[kWidth] == 0);
    e.onMouseMove(414, 86, false);
    check(h.v[kWidth] == 200);
    e.onMouseUp();
    check(h.begins == h.ends);
    Host keys;
    Editor keyboard(keys);
    for (int n = 0; n < 4; ++n) keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::Home, false);
    keyboard.onKey(EditorKey::Right, true);
    check(keys.v[kChannels] == 1);
    keyboard.onKey(EditorKey::Up, false);
    check(keys.v[kChannels] == 2);
    keyboard.onKey(EditorKey::Down, true);
    check(keys.v[kChannels] == 1);
    keyboard.onKey(EditorKey::Left, false);
    check(keys.v[kChannels] == 0);
    keyboard.onKey(EditorKey::Left, true);
    check(keys.v[kChannels] == 0);
    keyboard.onKey(EditorKey::End, false);
    keyboard.onKey(EditorKey::Right, false);
    check(keys.v[kChannels] == 4);
    keyboard.onKey(EditorKey::Left, true);
    check(keys.v[kChannels] == 3);
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::Home, false);
    keyboard.onKey(EditorKey::Up, true);
    check(keys.v[kSwap] == 1);
    keyboard.onKey(EditorKey::Down, false);
    check(keys.v[kSwap] == 0);
    check(keys.begins == keys.ends);
    std::printf("editor-input: %d failures\n", fail);
    return fail ? 1 : 0;
}
