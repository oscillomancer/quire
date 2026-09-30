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

#include "DriveParams.h"
#include "Gui.h"
#include "Telemetry.h"
#include "drive/processor.h"

extern "C" void driveReadPixels(int w, int h, unsigned char* rgb);
extern "C" void driveViewportSize(int* w, int* h);

using namespace supple::drive;
using namespace supple::drive::gui;

namespace {
constexpr double kRate = 250.0;

class PreviewHost : public EditorHost {
  public:
    PreviewHost() {
        for (const auto& p : kParams) values_[p.id] = p.def;
    }
    double paramValue(uint32_t id) const override { return values_[id]; }
    void beginEdit(uint32_t) override {}
    void setParam(uint32_t id, double value) override {
        if (const ParamDef* p = findParam(id); p && !(p->flags & pa::kReadonly))
            values_[id] = std::clamp(value, p->min, p->max);
    }
    void endEdit(uint32_t) override {}
    TelemetryRing& telemetry() override { return ring_; }
    int latencySamples() const override { return processor_.latencySamples(); }
    double sampleRate() const override { return 48000.0; }

    void start() {
        Values v{};
        std::copy_n(values_, kNumParams, v.begin());
        processor_.setParams(v);
        processor_.prepare(48000, 192, 2);
    }
    void advance(int frames) {
        for (int i = 0; i < frames; ++i) {
            double l[192], r[192];
            for (int n = 0; n < 192; ++n) {
                time_ += 1.0 / 48000;
                const double envelope = .08 + .1 * std::exp(-std::fmod(time_ * 2, 1.0) * 18);
                l[n] = r[n] = envelope * std::sin(2 * 3.141592653589793 * 1000 * time_);
            }
            double* io[] = {l, r};
            processor_.process(io, 192, true);
            const auto t = processor_.takeTelemetry();
            Frame f{};
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
    driveViewportSize(&c->w, &c->h);
    if (c->w <= 0 || c->h <= 0) return;
    c->rgb.resize(static_cast<size_t>(c->w) * c->h * 3);
    driveReadPixels(c->w, c->h, c->rgb.data());
    c->got = true;
}
}  // namespace

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 180;
    const char* output = argc > 2 ? argv[2] : "drive-gui.ppm";
    const int scale = argc > 3 ? std::atoi(argv[3]) : 1;
    if (scale != 1 && scale != 2) return 4;
    PreviewHost host;
    if (argc > 4) {
        if (std::strcmp(argv[4], "hot") == 0) {
            host.setParam(kCurve, 4);
            host.setParam(kDrive, 24);
            host.setParam(kBias, 30);
        } else if (std::strcmp(argv[4], "default") != 0)
            return 4;
    }
    host.start();
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
