// Orrery CPU benchmark (synth.md §9): the reference patch — 8 notes × 3-voice
// unison, 2 va + sub, ladder 24 dB, tape drive, 2 envelopes, 1 LFO, chorus +
// reverb, normal quality — at 48 kHz / 256 frames, as a percentage of one
// core's real time. Variants remove one part at a time to show where the time
// goes. Build the release preset for meaningful numbers:
//   cmake --build --preset release --target orrery-bench
//   build/release/plugins/orrery/engine/orrery-bench [seconds]
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "orrery/engine.h"
#include "orrery/patch.h"

using namespace supple::orrery;

namespace {

constexpr const char* kReference = R"({"schema": "supple.orrery-patch/1.0",
  "voice": {"polyphony": 8, "unison": 3, "unison_detune": "12 ct", "unison_spread": "70 %", "quality": "normal"},
  "oscillators": {
    "osc1": {"type": "va", "wave": "saw", "level": "-6 dB", "to": "filter1"},
    "osc2": {"type": "va", "wave": "square", "level": "-6 dB", "fine": "+7 ct", "to": "filter1"},
    "osc3": {"type": "sub", "wave": "sine", "octave": -1, "level": "-12 dB", "to": "filter1"}},
  "filters": {"filter1": {"type": "ladder", "slope": "24 dB", "cutoff": "1.2 kHz", "resonance": "30 %"}},
  "drive": {"type": "tape", "amount": "6 dB"},
  "envelopes": {"amp": {"attack": "10 ms", "sustain": "80 %", "release": "400 ms"},
                "env2": {"attack": "200 ms", "decay": "1 s", "sustain": "20 %"}},
  "lfos": {"lfo1": {"rate": "0.3 Hz"}},
  "routes": {"env2>filter1.cutoff": {"amount": "+2 oct"}, "lfo1>osc1.fine": {"amount": "10 ct"}},
  "fx": {"chain": ["chorus", "reverb"]}})";

struct Variant {
    const char* name;
    const char* merge;  // a merge patch over the reference ("" = the reference)
};

constexpr Variant kVariants[] = {
    {"reference", ""},
    {"no fx", R"({"fx": {"chain": []}})"},
    {"no drive", R"({"drive": null})"},
    {"svf instead of ladder", R"({"filters": {"filter1": {"type": "svf"}}})"},
    {"no filter", R"({"filters": {"filter1": null}, "routes": {"env2>filter1.cutoff": null},
                      "oscillators": {"osc1": {"to": "direct"}, "osc2": {"to": "direct"}, "osc3": {"to": "direct"}}})"},
    {"1 osc", R"({"oscillators": {"osc2": null, "osc3": null}})"},
    {"unison 1", R"({"voice": {"unison": 1}})"},
    {"eco quality", R"({"voice": {"quality": "eco"}})"},
    {"high quality", R"({"voice": {"quality": "high"}})"},
};

double measure(const Patch& patch, double seconds) {
    constexpr double kRate = 48000.0;
    constexpr std::uint32_t kBlock = 256;
    Engine engine;
    engine.prepare(kRate, kBlock);
    engine.setPatch(patch);
    engine.reset();
    std::vector<float> l(kBlock), r(kBlock);
    std::vector<Event> on;
    for (int k : {48, 52, 55, 59, 62, 64, 67, 71}) {
        Event e;
        e.type = Event::Type::NoteOn;
        e.key = static_cast<std::int16_t>(k);
        e.value = 0.8;
        on.push_back(e);
    }
    engine.process(on, Transport{}, l.data(), r.data(), kBlock);
    for (int i = 0; i < 200; ++i)
        engine.process({}, Transport{}, l.data(), r.data(), kBlock);  // warm up
    const auto blocks = static_cast<long>(seconds * kRate / kBlock);
    volatile float sink = 0.0f;
    const auto t0 = std::chrono::steady_clock::now();
    for (long i = 0; i < blocks; ++i) {
        engine.process({}, Transport{}, l.data(), r.data(), kBlock);
        sink = sink + l[0];
    }
    const double spent =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    return 100.0 * spent / (static_cast<double>(blocks) * kBlock / kRate);
}

// Diverse patches for --dump: every module type and routing path the voice
// renderer has, so a refactor can be checked for bit-identical output.
constexpr const char* kDumpPatches[] = {
    R"({"oscillators": {"osc1": {"type": "fm", "ratio": 1}, "osc2": {"type": "fm", "ratio": 3, "feedback": 30, "to": "none"},
                        "osc3": {"type": "va", "wave": "pulse", "width": 30, "sync": true}},
        "routes": {"osc2>osc1.fm": {"amount": 3}, "osc1>osc2.fm": {"amount": 0.5}, "osc3>osc1.am": {"amount": 40}},
        "filters": {"routing": "parallel", "filter1": {"type": "svf", "mode": "bandpass"}, "filter2": {"type": "comb", "feedback": 50}}})",
    R"({"oscillators": {"osc1": {"type": "wavetable", "table": "formant", "position": 40, "pan": -40},
                        "osc2": {"type": "noise", "color": "pink", "level": -18, "pan": 50, "to": "filter2"}},
        "filters": {"filter1": {"type": "formant", "vowel": 1.5}, "filter2": {"type": "ladder", "resonance": 70}},
        "drive": {"type": "fold", "amount": 12}, "lfos": {"lfo1": {"rate": 5}},
        "routes": {"lfo1>osc1.position": {"amount": 30}, "lfo1>filter2.cutoff": {"amount": "+1 oct"}}})",
    R"({"voice": {"mode": "legato", "glide": "80 ms", "unison": 4, "unison_spread": "100 %", "quality": "high"},
        "oscillators": {"osc1": {"type": "va", "wave": "saw", "shape": 40}, "osc2": {"type": "sub", "wave": "square"}},
        "filters": {"filter1": {"type": "svf", "mode": "highpass"}}, "drive": {"type": "hard", "amount": 9},
        "fx": {"chain": ["phaser", "delay", "eq"]}})",
    R"({"voice": {"quality": "eco", "drift": "50 %"},
        "oscillators": {"osc1": {"type": "va", "wave": "triangle", "to": "direct"}, "osc2": {"type": "wavetable", "table": "bell", "to": "both"}},
        "filters": {"routing": "serial", "filter1": {"type": "ladder", "slope": "12 dB"}, "filter2": {"type": "svf"}},
        "drive": {"type": "soft"}, "steps": {"step1": {"values": [100, -50, 25, 0]}},
        "routes": {"step1>filter1.cutoff": {"amount": "+2 oct"}, "velocity>osc2.level": {"amount": -6}}})",
};

// Renders each dump patch (and the reference) for 2 s with notes and a
// release, and writes the raw float stereo output.
int dump(const PatchType& type, const char* path) {
    std::FILE* f = std::fopen(path, "wb");
    if (!f) return 3;
    std::vector<std::string> docs = {kReference};
    for (const char* d : kDumpPatches)
        docs.push_back(std::string(R"({"schema": "supple.orrery-patch/1.0", )") + (d + 1));
    for (const std::string& d : docs) {
        auto p = type.parse(d);
        if (!p) {
            std::fprintf(stderr, "%s\n", p.error().toString().c_str());
            return 1;
        }
        Engine engine;
        engine.prepare(48000.0, 128);
        engine.setPatch(*p);
        engine.reset();
        std::vector<float> l(128), r(128);
        for (int b = 0; b < 750; ++b) {
            std::vector<Event> ev;
            if (b == 0 || b == 100 || b == 300) {
                for (int k : {48, 55, 64}) {
                    Event e;
                    e.type = Event::Type::NoteOn;
                    e.key = static_cast<std::int16_t>(k + (b / 100) * 2);
                    e.value = 0.5 + 0.1 * (b / 100);
                    e.time = static_cast<std::uint32_t>(7 * (b / 100));
                    ev.push_back(e);
                }
            }
            if (b == 400) {
                for (int k : {48, 55, 64, 50, 57, 66, 54, 61, 70}) {
                    Event e;
                    e.type = Event::Type::NoteOff;
                    e.key = static_cast<std::int16_t>(k);
                    ev.push_back(e);
                }
            }
            engine.process(ev, Transport{}, l.data(), r.data(), 128);
            std::fwrite(l.data(), sizeof(float), 128, f);
            std::fwrite(r.data(), sizeof(float), 128, f);
        }
    }
    std::fclose(f);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    const PatchType type;
    if (argc > 2 && std::string(argv[1]) == "--dump") return dump(type, argv[2]);
    const double seconds = argc > 1 ? std::atof(argv[1]) : 10.0;
    auto reference = type.parse(kReference);
    if (!reference) {
        std::fprintf(stderr, "%s\n", reference.error().toString().c_str());
        return 1;
    }
    std::printf("Orrery benchmark: 48 kHz / 256 frames, %.0f s per variant, %% of one core\n",
                seconds);
    for (const Variant& v : kVariants) {
        Patch p = *reference;
        if (*v.merge) {
            auto merged = type.merge(p, supple::json::parse(v.merge).value());
            if (!merged) {
                std::fprintf(stderr, "%s: %s\n", v.name, merged.error().toString().c_str());
                return 1;
            }
            p = *merged;
        }
        // Median of three runs: the machine is shared with other work.
        double runs[3];
        for (double& x : runs) x = measure(p, seconds / 3.0);
        std::sort(runs, runs + 3);
        std::printf("  %-24s %6.2f %%\n", v.name, runs[1]);
    }
    return 0;
}
