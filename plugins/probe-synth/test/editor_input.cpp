// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include <cstdio>

#include "Editor.h"

using namespace probesynth;
using namespace probesynth::gui;

struct Host : EditorHost {
    TelemetryRing ring;
    double values[kNumParams]{};
    int begins = 0, ends = 0, sets = 0;
    Host() {
        for (const auto& p : kParams) values[p.id] = p.def;
    }
    double paramValue(uint32_t id) const override { return values[id]; }
    void beginEdit(uint32_t) override { ++begins; }
    void setParam(uint32_t id, double value) override {
        values[id] = value;
        ++sets;
    }
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
    ring.push(10);
    ring.push(20);
    ring.push(30);
    int a = 0, b = 0;
    check(ring.pop(a) && ring.pop(b) && a == 10 && b == 20,
          "full SPSC ring never overwrites unread storage");
    Ring<int, 1> retry;
    retry.push(7);
    retry.push(8);
    int held = 0, heldAgain = 0;
    check(retry.peek(held) && retry.peek(heldAgain) && held == 7 && heldAgain == 7,
          "peek retains an event until the consumer accepts it");
    retry.consume();
    check(retry.pop(held) && held == 8, "consume advances a retained event exactly once");

    Host host;
    Editor editor(host);
    for (int i = 0; i < 20; ++i) editor.onScroll(255, 125, -1);
    check(host.begins == 1 && host.ends == 0 && host.sets == 20,
          "scroll wheel coalesces into one open host gesture");
    editor.pump(0.24);
    check(host.ends == 0, "scroll gesture remains open before 250 ms");
    editor.pump(0.02);
    check(host.ends == 1, "scroll gesture closes after 250 ms real time");

    host.begins = host.ends = 0;
    editor.onScroll(255, 125, -1);
    editor.onScroll(410, 125, -1);
    editor.pump(0.26);
    check(host.begins == 2 && host.ends == 2, "changing controls closes the previous gesture");

    host.begins = host.ends = 0;
    const bool clicked = editor.onMouseDown(720, 125);
    check(clicked && host.values[kFilterSlope] == 1.0 && host.begins == host.ends,
          "toggle works before the first rendered frame");

    Host keys;
    Editor keyboard(keys);
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::Tab, false);
    const double before = keys.values[kCutoff];
    keyboard.onKey(EditorKey::Right, true);
    check(keys.values[kCutoff] > before && keys.begins == keys.ends,
          "keyboard fine adjustment emits balanced gestures");
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::End, false);
    check(keys.values[kResonance] == 1.0, "Home and End reach parameter limits");
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::Tab, false);
    keyboard.onKey(EditorKey::Space, false);
    check(keys.values[kFilterSlope] == 1.0 && keys.begins == keys.ends,
          "keyboard toggles discrete controls");

    Host discrete;
    Editor steps(discrete);
    steps.onKey(EditorKey::Tab, false);
    steps.onKey(EditorKey::Home, false);
    steps.onKey(EditorKey::Right, true);
    check(discrete.values[kWaveform] == 1, "enum fine arrow moves one physical step");
    steps.onKey(EditorKey::Left, false);
    check(discrete.values[kWaveform] == 0, "enum reverse arrow moves one physical step");
    for (int i = 0; i < 4; ++i) steps.onKey(EditorKey::Tab, false);
    steps.onKey(EditorKey::Home, false);
    steps.onKey(EditorKey::Up, true);
    check(discrete.values[kFilterSlope] == 1, "bool fine arrow toggles on");
    steps.onKey(EditorKey::Down, false);
    check(discrete.values[kFilterSlope] == 0, "bool arrow toggles off");
    for (int i = 0; i < 10; ++i) steps.onKey(EditorKey::Tab, false);
    steps.onKey(EditorKey::Home, false);
    const double minimum = discrete.values[kSeed];
    steps.onKey(EditorKey::Right, false);
    check(discrete.values[kSeed] == minimum + 1, "integer arrow moves one physical step");
    steps.onKey(EditorKey::Left, true);
    check(discrete.values[kSeed] == minimum, "integer fine reverse moves one physical step");
    steps.onKey(EditorKey::End, false);
    const double maximum = discrete.values[kSeed];
    steps.onKey(EditorKey::Up, false);
    check(discrete.values[kSeed] == maximum && discrete.begins == discrete.ends,
          "discrete steps clamp at boundaries with balanced gestures");
    std::printf("\n%s\n", failures ? "FAILURES" : "all checks passed");
    return failures ? 1 : 0;
}
