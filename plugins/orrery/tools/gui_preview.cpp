// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Offline render of the real editor over the real CLAP core, holding a
// representative patch. Usage: orrery-gui-preview [out.ppm] [scale 1|2]
// [scenario: rest | type-menu | route-menu | lfo]. The CLAP probe separately
// validates embedding, timers, resize and teardown in a host-like lifecycle.
#include <clap/clap.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "Gui.h"
#include "orrery/clap_plugin.h"
#include "supple/json/json.h"

extern "C" void orreryReadPixels(int w, int h, unsigned char* rgb);
extern "C" void orreryViewportSize(int* w, int* h);

namespace so = supple::orrery;
using namespace orrery::gui;

namespace {

constexpr const char* kPatch = R"({
  "voice": {"polyphony": 8, "unison": 3, "unison_detune": "12 ct", "quality": "normal"},
  "oscillators": {
    "osc1": {"type": "va", "wave": "saw", "level": "-6 dB", "fine": "-7 ct", "to": "filter1"},
    "osc2": {"type": "va", "wave": "saw", "level": "-6 dB", "fine": "+7 ct", "to": "filter1"},
    "osc3": {"type": "sub", "wave": "sine", "octave": -1, "level": "-12 dB", "to": "filter1"}},
  "filters": {"filter1": {"type": "ladder", "slope": "24 dB", "cutoff": "1.2 kHz", "resonance": "25 %"}},
  "drive": {"type": "tape", "amount": "3 dB"},
  "envelopes": {"amp": {"attack": "600 ms", "decay": "1.2 s", "sustain": "80 %", "release": "1.5 s"},
                "env2": {"attack": "1.5 s", "decay": "3 s", "sustain": "30 %", "release": "2 s"}},
  "lfos": {"lfo1": {"wave": "sine", "rate": "0.2 Hz", "retrigger": "free"}},
  "routes": {"env2>filter1.cutoff": {"amount": "+2 oct"}, "lfo1>osc1.fine": {"amount": "8 ct"},
             "macro1>filter1.cutoff": {"amount": "+3 oct"}, "velocity>filter1.cutoff": {"amount": "+1 oct"}},
  "macros": {"macro1": {"name": "Brightness", "value": "20 %"}},
  "fx": {"chain": ["chorus", "reverb"], "reverb": {"size": "70 %", "mix": "25 %"}}
})";

struct PreviewHost {
    clap_host_t host{};
    PreviewHost() {
        host.clap_version = CLAP_VERSION;
        host.host_data = this;
        host.name = "orrery-gui-preview";
        host.vendor = host.url = host.version = "";
        host.get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
        host.request_restart = host.request_process =
            host.request_callback = [](const clap_host_t*) {};
    }
};

so::EditorBridge* bridge = nullptr;
const so::ClapExtensions kCapture = {
    [](const clap_plugin_t*, const clap_host_t*, so::EditorBridge& b) -> void* {
        bridge = &b;
        return &b;
    },
    [](void*) {}, nullptr, nullptr};

struct Capture {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;
    bool want = false, got = false, frameSeen = false;
};

void capture(void* user) {
    auto* c = static_cast<Capture*>(user);
    c->frameSeen = true;
    if (!c->want || c->got) return;
    orreryViewportSize(&c->w, &c->h);
    if (c->w <= 0 || c->h <= 0) return;
    c->rgb.resize(static_cast<size_t>(c->w) * static_cast<size_t>(c->h) * 3);
    orreryReadPixels(c->w, c->h, c->rgb.data());
    c->got = true;
}

}  // namespace

int main(int argc, char** argv) {
    const char* output = argc > 1 ? argv[1] : "orrery-gui.ppm";
    const int scale = argc > 2 ? std::atoi(argv[2]) : 1;
    const std::string scenario = argc > 3 ? argv[3] : "rest";
    if (scale != 1 && scale != 2) return 4;
    PreviewHost ph;
    const clap_plugin_t* plugin = so::createClapPlugin(&ph.host, &kCapture);
    if (!plugin || !plugin->init(plugin) || !bridge) return 5;
    std::string doc = std::string(R"({"schema": "supple.orrery-patch/1.0", )") + (kPatch + 1);
    if (!bridge->merge(supple::json::parse(doc).value())) return 6;
    int result = 0;
    {
        Gui gui(*bridge, true);
        Capture cap;
        gui.setFrameCallback(&capture, &cap);
        gui.setSize(static_cast<uint32_t>(kDesignW * static_cast<float>(scale)),
                    static_cast<uint32_t>(kDesignH * static_cast<float>(scale)));
        if (!gui.create() || !gui.setParent(0) || !gui.show()) return 1;
        for (int i = 0; i < 300 && !cap.frameSeen; ++i) {
            gui.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!cap.frameSeen) return 2;
        if (scenario == "type-menu") {
            gui.previewClick(80, 140);  // osc1 tile
            gui.previewClick(935, 92);  // its type pill
        } else if (scenario == "route-menu") {
            gui.previewClick(1050, 454);  // + Route
        } else if (scenario == "lfo") {
            gui.previewClick(90, 560);  // lfo1 tile
        } else if (scenario != "rest") {
            return 4;
        }
        cap.want = true;
        gui.previewRepaint();
        for (int i = 0; i < 300 && !cap.got; ++i) {
            gui.tick();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!cap.got) return 2;
        FILE* file = std::fopen(output, "wb");
        if (!file) return 3;
        std::fprintf(file, "P6\n%d %d\n255\n", cap.w, cap.h);
        for (int y = cap.h - 1; y >= 0; --y)
            std::fwrite(cap.rgb.data() + static_cast<size_t>(y) * static_cast<size_t>(cap.w) * 3, 1,
                        static_cast<size_t>(cap.w) * 3, file);
        std::fclose(file);
        std::printf("preview: wrote %s (%dx%d)\n", output, cap.w, cap.h);
    }
    plugin->destroy(plugin);
    return result;
}
