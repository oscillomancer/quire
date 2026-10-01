// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Offline render of the real editor. The CLAP probe separately validates
// embedding, timers, resize and teardown in a host-like lifecycle.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include "Gui.h"
#include "ReverbParams.h"
#include "Telemetry.h"
#include "reverb/processor.h"
#if defined(__linux__)
#include <X11/Xlib.h>
#endif

extern "C" void reverbReadPixels(int w, int h, unsigned char* rgb);
extern "C" void reverbViewportSize(int* w, int* h);

using namespace supple::reverb;
using namespace supple::reverb::gui;

namespace {
constexpr double kRate = 20.0;
#if defined(__linux__)
struct PreviewParent {
    Display* display = XOpenDisplay(nullptr);
    ::Window window = 0;
    PreviewParent(int width, int height) {
        if (!display) return;
        XSetWindowAttributes attributes{};
        attributes.override_redirect = True;
        // An embedded view gets its design size even under a tiling WM.
        window =
            XCreateWindow(display, DefaultRootWindow(display), 40, 40, static_cast<unsigned>(width),
                          static_cast<unsigned>(height), 0, CopyFromParent, InputOutput,
                          CopyFromParent, CWOverrideRedirect, &attributes);
        XMapWindow(display, window);
        XSync(display, False);
    }
    ~PreviewParent() {
        if (window) XDestroyWindow(display, window);
        if (display) XCloseDisplay(display);
    }
};
#endif

class PreviewHost : public EditorHost {
  public:
    PreviewHost(const char* scenario) {
        for (const auto& p : kParams) values_[p.id] = p.def;
        if (!std::strcmp(scenario, "hall")) {
            values_[kSize] = 85;
            values_[kDecay] = 4500;
            values_[kPredelay] = 25;
            values_[kHighDecay] = .4;
        }
        if (!processor_.prepare(48000, 2400)) std::exit(1);
        processor_.setTelemetryEnabled(true);
        for (size_t i = 0; i < kNumParams; ++i) processor_.setParam(i, values_[i]);
    }
    double paramValue(uint32_t id) const override { return values_[id]; }
    void beginEdit(uint32_t) override {}
    void setParam(uint32_t id, double value) override {
        if (const ParamDef* p = findParam(id); p && !(p->flags & supple::params::kReadonly))
            values_[id] = std::clamp(value, p->min, p->max);
    }
    void endEdit(uint32_t) override {}
    TelemetryRing& telemetry() override { return ring_; }
    int latencySamples() const override { return static_cast<int>(processor_.latency()); }
    double sampleRate() const override { return 48000.0; }

    void advance(int frames) {
        for (int frame = 0; frame < frames; ++frame) {
            float l[2400], r[2400];
            for (int i = 0; i < 2400; ++i) {
                time_ += 1 / 48000.;
                const double phase = std::fmod(time_ * 2, 1.);
                const double amp = .08 + .8 * std::exp(-phase * 18);
                l[i] = static_cast<float>(amp * std::sin(time_ * 6283.185307));
                r[i] = l[i];
            }
            const float* in[]{l, r};
            float* out[]{l, r};
            processor_.process(in, out, 2400);
            const auto t = processor_.takeTelemetry();
            Frame f;
            f.inputPeakDb = static_cast<float>(t.inputPeakDb);
            f.outputPeakDb = static_cast<float>(t.outputPeakDb);
            ring_.push(f);
        }
    }

  private:
    Processor processor_;
    double values_[kNumParams]{};
    TelemetryRing ring_;
    double time_ = 0.0;
};

struct Capture {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
    bool want = false, got = false, frameSeen = false;
};

void capture(void* user) {
    auto* c = static_cast<Capture*>(user);
    c->frameSeen = true;
    if (!c->want || c->got) return;
    reverbViewportSize(&c->w, &c->h);
    if (c->w <= 0 || c->h <= 0) return;
    c->rgb.resize(static_cast<size_t>(c->w) * c->h * 3);
    reverbReadPixels(c->w, c->h, c->rgb.data());
    c->got = true;
}
}  // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 180;
    const char* output = argc > 2 ? argv[2] : "reverb-gui.ppm";
    const int scale = argc > 3 ? std::atoi(argv[3]) : 1;
    if (scale != 1 && scale != 2) return 4;
    const char* scenario = argc > 4 ? argv[4] : "default";
    if (std::strcmp(scenario, "default") && std::strcmp(scenario, "hall")) return 4;
    PreviewHost host(scenario);
    uintptr_t parent = 0;
#if defined(__linux__)
    PreviewParent previewParent(static_cast<int>(kDesignW * scale),
                                static_cast<int>(kDesignH * scale));
    if (!previewParent.window) return 1;
    parent = static_cast<uintptr_t>(previewParent.window);
#endif
    Gui gui(host, parent == 0);
    Capture cap;
    gui.setFrameCallback(&capture, &cap);
    gui.setSize(static_cast<uint32_t>(kDesignW * scale), static_cast<uint32_t>(kDesignH * scale));
    if (!gui.create() || !gui.setParent(parent) || !gui.show()) return 1;
    // Let the X11 view realize before feeding it telemetry. Otherwise the
    // 256-slot queue fills while the map/expose travels through the server,
    // and captures contain a timing-dependent slice of the history.
    for (int i = 0; i < 300 && !cap.frameSeen; ++i) {
        gui.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!cap.frameSeen) return 2;
    const int observations =
        static_cast<int>(kHistorySeconds * kRate) + 100 + std::max(0, frames) * 4;
    for (int i = 0; i < observations; i += 4) {
        host.advance(std::min(4, observations - i));
        gui.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    cap.want = true;
    gui.previewRepaint();
    // tick() polls with a zero timeout, by design: in a host it must never
    // block the loop it is called from. Here there is no host, and on X11 the
    // map and expose come back from the server rather than being generated
    // locally, so a tight spin finishes long before either arrives. Pace the
    // wait instead of burning through it.
    for (int i = 0; i < 300 && !cap.got; ++i) {
        gui.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!cap.got) return 2;
    FILE* file = std::fopen(output, "wb");
    if (!file) return 3;
    std::fprintf(file, "P6\n%d %d\n255\n", cap.w, cap.h);
    for (int y = cap.h - 1; y >= 0; --y)
        std::fwrite(cap.rgb.data() + static_cast<size_t>(y) * cap.w * 3, 1,
                    static_cast<size_t>(cap.w) * 3, file);
    std::fclose(file);
    std::printf("preview: wrote %s (%dx%d)\n", output, cap.w, cap.h);
    return 0;
}
