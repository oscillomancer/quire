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
#include "AnalyzerParams.h"
#include "Telemetry.h"

extern "C" void analyzerReadPixels(int w, int h, unsigned char* rgb);
extern "C" void analyzerViewportSize(int* w, int* h);

using namespace analyzer;
using namespace analyzer::gui;

namespace {
constexpr double kRate = 250.0;

class PreviewHost : public EditorHost {
  public:
    PreviewHost() {
        for (const auto& p : kParams) values_[p.id] = p.def;
        values_[kInputGain] = 3.0;
    }
    double paramValue(uint32_t id) const override { return values_[id]; }
    void beginEdit(uint32_t) override {}
    void setParam(uint32_t id, double value) override {
        if (const ParamDef* p = findParam(id); p && !p->readonly)
            values_[id] = std::clamp(value, p->min, p->max);
    }
    void endEdit(uint32_t) override {}
    TelemetryRing& telemetry() override { return ring_; }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000.0; }

    void advance(int frames) {
        for (int i = 0; i < frames; ++i) {
            time_ += 1.0 / kRate;
            const double beat = time_ * 2.0;
            const double phase = beat - std::floor(beat);
            const double transient = std::exp(-phase * 18.0);
            const double bed = -18.0 + 3.0 * std::sin(time_ * 0.7);
            Frame f;
            f.inputPeakDb = static_cast<float>(bed + transient * 17.0);
            f.outputPeakDb = static_cast<float>(f.inputPeakDb + values_[kInputGain] +
                                                 values_[kOutputGain]);
            ring_.push(f);
        }
    }

  private:
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
    analyzerViewportSize(&c->w, &c->h);
    if (c->w <= 0 || c->h <= 0) return;
    c->rgb.resize(static_cast<size_t>(c->w) * c->h * 3);
    analyzerReadPixels(c->w, c->h, c->rgb.data());
    c->got = true;
}
}  // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 180;
    const char* output = argc > 2 ? argv[2] : "analyzer-gui.ppm";
    const int scale = argc > 3 ? std::atoi(argv[3]) : 1;
    if (scale != 1 && scale != 2) return 4;
    PreviewHost host;
    Gui gui(host, true);
    Capture cap;
    gui.setFrameCallback(&capture, &cap);
    gui.setSize(static_cast<uint32_t>(kDesignW * scale),
                static_cast<uint32_t>(kDesignH * scale));
    if (!gui.create() || !gui.setParent(0) || !gui.show()) return 1;
    // Let the X11 view realize before feeding it telemetry. Otherwise the
    // 256-slot queue fills while the map/expose travels through the server,
    // and captures contain a timing-dependent slice of the history.
    for (int i = 0; i < 300 && !cap.frameSeen; ++i) {
        gui.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!cap.frameSeen) return 2;
    const int observations = static_cast<int>(kHistorySeconds * kRate) + 100 +
                             std::max(0, frames) * 4;
    for (int i = 0; i < observations; i += 4) {
        host.advance(std::min(4, observations - i));
        gui.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (argc > 4) {
        if (std::strcmp(argv[4], "bypass-on") == 0) gui.previewClick(620, 382);
        else if (std::strcmp(argv[4], "bypass-hover") != 0) return 4;
        gui.previewMove(620, 382);
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
