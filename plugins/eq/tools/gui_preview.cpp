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

#include "EqParams.h"
#include "Gui.h"
#include "Telemetry.h"
#include "eq/processor.h"

extern "C" void eqReadPixels(int w, int h, unsigned char* rgb);
extern "C" void eqViewportSize(int* w, int* h);

using namespace eq;
using namespace eq::gui;

namespace {
constexpr double kRate = 250.0;

class PreviewHost : public EditorHost {
  public:
    explicit PreviewHost(const char* scenario) {
        values_ = supple::eq::defaults();
        if (std::strcmp(scenario, "flat") == 0) {
            for (std::size_t b = 0; b < 8; ++b) values_[b * 7] = 0;
        } else {
            values_[2] = 80;
            values_[10] = -3;
            values_[9] = 250;
            values_[11] = 1.2;
            values_[24] = 2;
            values_[28] = 1;
            values_[30] = 3500;
            values_[31] = -2;
            values_[34] = supple::eq::Side;
        }
        for (std::size_t i = 0; i < values_.size(); ++i) processor_.setParam(i, values_[i]);
        processor_.prepare(48000, 256);
    }
    double paramValue(uint32_t id) const override { return values_[id]; }
    void beginEdit(uint32_t) override {}
    void endEdit(uint32_t) override {}
    void setParam(uint32_t id, double v) override {
        values_[id] = supple::params::clampPhysical(kParams[id], v);
    }
    TelemetryRing& telemetry() override { return ring_; }
    int latencySamples() const override { return 0; }
    double sampleRate() const override { return 48000; }
    void advance(int frames) {
        for (int i = 0; i < frames; ++i) {
            Frame f;

            for (int k = 0; k < 256; ++k) {
                const double t = static_cast<double>(sample_++) / 48000;
                f.input[k] = static_cast<float>(.2 * std::sin(2 * 3.141592653589793 * 110 * t) +
                                                .08 * std::sin(2 * 3.141592653589793 * 870 * t) +
                                                .03 * std::sin(2 * 3.141592653589793 * 7300 * t));
            }
            for (int k = 0; k < 256; ++k) {
                const double t =
                    static_cast<double>(sample_ - 256 + static_cast<uint64_t>(k)) / 48000;
                const float side =
                    static_cast<float>(.04 * std::sin(2 * 3.141592653589793 * 3500 * t));
                f.inputRight[k] = f.input[k] - side;
                f.input[k] += side;
            }
            const float* in[] = {f.input, f.inputRight};
            float* out[] = {f.output, f.outputRight};
            processor_.process(in, out, 256);
            ring_.push(f);
        }
    }

  private:
    supple::eq::Values values_{};
    supple::eq::Processor processor_;
    TelemetryRing ring_;
    uint64_t sample_ = 0;
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
    eqViewportSize(&c->w, &c->h);
    if (c->w <= 0 || c->h <= 0) return;
    c->rgb.resize(static_cast<size_t>(c->w) * c->h * 3);
    eqReadPixels(c->w, c->h, c->rgb.data());
    c->got = true;
}
}  // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 180;
    const char* output = argc > 2 ? argv[2] : "eq-gui.ppm";
    const int scale = argc > 3 ? std::atoi(argv[3]) : 1;
    if (scale != 1 && scale != 2) return 4;
    PreviewHost host(argc > 4 ? argv[4] : "flat");
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
