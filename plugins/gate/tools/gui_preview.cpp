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

#include "GateParams.h"
#include "Gui.h"
#include "Telemetry.h"
#include "gate/processor.h"

extern "C" void gateReadPixels(int w, int h, unsigned char* rgb);
extern "C" void gateViewportSize(int* w, int* h);

using namespace gate;
using namespace gate::gui;

namespace {
constexpr double kRate = 20.0;
class PreviewHost : public EditorHost {
  public:
    explicit PreviewHost(bool drums) : drums_(drums) {
        processor_.prepare(48000, 256, 2);
        if (drums_) {
            values_[kHold] = 50;
            values_[kRange] = -30;
            values_[kScHighpass] = 160;
        }
        processor_.setParams(values_);
    }
    double paramValue(uint32_t id) const override { return values_[id]; }
    void beginEdit(uint32_t) override {}
    void setParam(uint32_t id, double value) override {
        values_[id] = value;
        processor_.setParam(id, value);
    }
    void endEdit(uint32_t) override {}
    TelemetryRing& telemetry() override { return ring_; }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000; }
    void advance(int observations) {
        for (int step = 0; step < observations; ++step) {
            for (int done = 0; done < 2400;) {
                const int count = std::min(256, 2400 - done);
                float l[256]{}, r[256]{};
                for (int i = 0; i < count; ++i) {
                    const double t = static_cast<double>(sample_++) / 48000;
                    const double phase = std::fmod(t, .5), pulse = std::exp(-phase * 30);
                    l[i] = static_cast<float>(.25 * pulse * std::sin(6.283185307179586 * 180 * t) +
                                              .08 * pulse * std::sin(6.283185307179586 * 1200 * t) +
                                              .001 * std::sin(6.283185307179586 * 700 * t));
                    r[i] = l[i] * .8f;
                }
                float* io[]{l, r};
                processor_.process(io, count);
                done += count;
            }
            const auto t = processor_.telemetry();
            ring_.push({t.level, t.gr, t.open, static_cast<double>(sample_) / 48000});
        }
    }

  private:
    bool drums_ = false;
    uint64_t sample_ = 0;
    Values values_ = defaults();
    TelemetryRing ring_;
    supple::gate::Processor processor_;
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
    gateViewportSize(&c->w, &c->h);
    if (c->w <= 0 || c->h <= 0) return;
    c->rgb.resize(static_cast<size_t>(c->w) * c->h * 3);
    gateReadPixels(c->w, c->h, c->rgb.data());
    c->got = true;
}
}  // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 180;
    const char* output = argc > 2 ? argv[2] : "gate-gui.ppm";
    const int scale = argc > 3 ? std::atoi(argv[3]) : 1;
    if (scale != 1 && scale != 2) return 4;
    const char* scenario = argc > 4 ? argv[4] : "default";
    if (std::strcmp(scenario, "default") && std::strcmp(scenario, "drums")) return 4;
    PreviewHost host(std::strcmp(scenario, "drums") == 0);
    Gui gui(host, true);
    Capture cap;
    gui.setFrameCallback(&capture, &cap);
    gui.setSize(static_cast<uint32_t>(kDesignW * scale), static_cast<uint32_t>(kDesignH * scale));
    if (!gui.create() || !gui.setParent(0) || !gui.show()) return 1;
    // Let the X11 view realize before feeding it telemetry. Otherwise the
    // 256-slot queue fills while the map/expose travels through the server,
    // and captures contain a timing-dependent slice of the history.
    for (int i = 0; i < 300 && !cap.frameSeen; ++i) {
        gui.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!cap.frameSeen) return 2;
    const int observations = static_cast<int>(kHistorySeconds * kRate) + 100 + std::max(0, frames);
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
