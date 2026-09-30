#include <array>
#include <cstdio>
#include "Editor.h"
using namespace eq;
using namespace eq::gui;
struct Host : EditorHost {
    std::array<double, kNumParams> v{};
    TelemetryRing ring;
    int begins = 0, ends = 0;
    Host() {
        for (const auto& p : kParams) v[p.id] = p.def;
    }
    double paramValue(uint32_t id) const override { return v[id]; }
    void beginEdit(uint32_t) override { ++begins; }
    void endEdit(uint32_t) override { ++ends; }
    void setParam(uint32_t id, double value) override {
        v[id] = supple::params::clampPhysical(kParams[id], value);
    }
    TelemetryRing& telemetry() override { return ring; }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000; }
};
int main() {
    Host h;
    Editor e(h);
    auto check = [&](bool pass) {
        if (!pass) std::puts("editor input failed");
        return pass;
    };
    if (!check(e.onMouseDown(100, 406))) return 1;
    e.onMouseMove(100, 380, false);
    e.onMouseUp();
    if (!check(h.v[2] != 30 && h.begins == h.ends)) return 1;
    e.onKey(EditorKey::Tab, false);
    e.onKey(EditorKey::End, false);
    e.onFocusLost();
    if (!check(h.begins == h.ends)) return 1;
    if (!check(e.onScroll(100, 450, 1))) return 1;
    e.pump(.3);
    if (!check(h.begins == h.ends)) return 1;
    e.onMouseDown(100, 406);
    e.onFocusLost();
    if (!check(h.begins == h.ends && !e.dragging())) return 1;
    e.onPointerDown(100, 406, 2, 0);
    e.onMouseDown(0, 0);
    if (!check(h.begins == h.ends)) return 1;
    return 0;
}
