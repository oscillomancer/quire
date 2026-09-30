// SPDX-License-Identifier: MIT
#include <cstdio>
#include "Editor.h"
using namespace analyzer;
using namespace analyzer::gui;
struct Host : EditorHost {
    TelemetryRing ring;
    Values values = defaults();
    int begins = 0, ends = 0, sets = 0;
    double paramValue(uint32_t id) const override { return values[id]; }
    void beginEdit(uint32_t) override { ++begins; }
    void setParam(uint32_t id, double value) override {
        values[id] = id == kReset ? 0 : value;
        ++sets;
    }
    void endEdit(uint32_t) override { ++ends; }
    TelemetryRing& telemetry() override { return ring; }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000; }
};
int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* text) {
        std::printf("%s: %s\n", text, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    };
    Ring<int, 1> ring;
    ring.push(10);
    ring.push(20);
    check(!ring.push(30), "full ring drops new item");
    int v = 0;
    check(ring.peek(v) && v == 10, "peek holds item");
    ring.consume();
    check(ring.pop(v) && v == 20, "consume advances once");
    Host host;
    Editor editor(host);
    check(editor.onMouseDown(280, 490) && host.values[kHold] == 1, "hold before first frame");
    check(host.begins == host.ends, "click brackets gesture");
    editor.onMouseDown(60, 490);
    check(host.values[kWindow] == 1, "window cycles enum");
    editor.onScroll(60, 490, -1);
    editor.onScroll(60, 490, -1);
    check(host.begins == host.ends + 1, "wheel one open gesture");
    editor.pump(.24);
    check(host.begins == host.ends + 1, "wheel stays open before 250ms");
    editor.pump(.02);
    check(host.begins == host.ends, "wheel closes at 250ms");
    editor.onScroll(60, 490, 1);
    editor.onPointerLeave();
    editor.onFocusLost();
    check(host.begins == host.ends, "focus loss closes gesture");
    Host keys;
    Editor keyboard(keys);
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::End, false);
    check(keys.values[kWindow] == 3, "keyboard End reaches since reset");
    keyboard.onKey(EditorKey::Home, false);
    check(keys.values[kWindow] == 0, "Home reaches 3 s");
    keyboard.onKey(EditorKey::Right, true);
    check(keys.values[kWindow] == 1, "arrow advances discrete choice");
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::Space, false);
    check(keys.values[kHold] == 1, "keyboard hold toggle");
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::Enter, false);
    check(keys.values[kReset] == 0 && keys.begins == keys.ends, "reset is momentary and balanced");
    return failures ? 1 : 0;
}
