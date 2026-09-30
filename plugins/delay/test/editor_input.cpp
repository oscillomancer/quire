// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include <cstdio>

#include "Editor.h"

using namespace delay;
using namespace delay::gui;

struct Host : EditorHost {
    TelemetryRing ring;
    double values[kNumParams]{};
    int begins = 0, ends = 0, sets = 0;
    Host() { for (const auto& p : kParams) values[p.id] = p.def; }
    double paramValue(uint32_t id) const override { return values[id]; }
    void beginEdit(uint32_t) override { ++begins; }
    void setParam(uint32_t id, double value) override { values[id] = value; ++sets; }
    void endEdit(uint32_t) override { ++ends; }
    TelemetryRing& telemetry() override { return ring; }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000.0; }
};

int failures = 0;
void check(bool ok, const char* name) {
    std::printf("  %-52s %s\n", name, ok ? "ok" : "FAIL");
    if (!ok) ++failures;
}

int main() {
    Ring<int, 1> ring;
    ring.push(10); ring.push(20); ring.push(30);
    int a = 0, b = 0;
    check(ring.pop(a) && ring.pop(b) && a == 10 && b == 20,
          "full SPSC ring never overwrites unread storage");
    Ring<int, 1> retry;
    retry.push(7); retry.push(8);
    int held = 0, heldAgain = 0;
    check(retry.peek(held) && retry.peek(heldAgain) && held == 7 && heldAgain == 7,
          "peek retains an event until the consumer accepts it");
    retry.consume();
    check(retry.pop(held) && held == 8, "consume advances a retained event exactly once");

    Host host;
    Editor editor(host);
    for (int i = 0; i < 20; ++i) editor.onScroll(610, 174, -1);
    check(host.begins == 1 && host.ends == 0 && host.sets == 20,
          "scroll wheel coalesces into one open host gesture");
    editor.pump(0.24);
    check(host.ends == 0, "scroll gesture remains open before 250 ms");
    editor.pump(0.02);
    check(host.ends == 1, "scroll gesture closes after 250 ms real time");

    host.begins = host.ends = 0;
    editor.onScroll(610, 174, -1);
    editor.onScroll(730, 174, -1);
    editor.pump(0.26);
    check(host.begins == 2 && host.ends == 2,
          "changing controls closes the previous gesture");

    host.begins = host.ends = 0;
    const bool clicked = editor.onMouseDown(620, 380);
    check(clicked && host.values[kBypass] == 1.0 && host.begins == host.ends,
          "toggle works before the first rendered frame");

    Host keys;
    Editor keyboard(keys);
    keyboard.onKey(EditorKey::Tab, false);
    const double before = keys.values[kInputGain];
    keyboard.onKey(EditorKey::Right, true);
    check(keys.values[kInputGain] > before && keys.begins == keys.ends,
          "keyboard fine adjustment emits balanced gestures");
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::End, false);
    check(keys.values[kMix] == 1.0, "Home and End reach parameter limits");
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::Space, false);
    check(keys.values[kBypass] == 1.0 && keys.begins == keys.ends,
          "keyboard toggles discrete controls");

    std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
