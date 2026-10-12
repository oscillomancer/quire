// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Offline renderer: a WAV file through the same Engine and DSP the plugin
// runs, with parameters set by name and in their own units, at fixed frames
// if wanted. For measurements, null tests and block-boundary debugging
// without a DAW.
//
//   render-cli in.wav out.wav --set "Input Gain=+6 dB" --set "Low Cut=120 Hz"
//   render-cli in.wav out.wav --at 48000 "Mix=50%" --block 257
//   render-cli in.wav out.wav --sidechain key.wav     (PRODUCT_SIDECHAIN)
//   render-cli in.wav out.wav --preset "Wide Pan" --set "Depth=40%"
//   render-cli --list
//
// Aux outputs (PRODUCT_AUX_OUTPUTS) are written beside the output, one file
// each: out-Ducked.wav. A note effect has no audio to render.
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/Engine.h"
#include "core/ParamTable.h"
#include "core/Preset.h"
#include "core/PropertyTable.h"
#include "ProductInfo.h"
#include "wav.h"

using namespace supple;

namespace {

void usage() {
    std::printf(
        "%s %s offline renderer\n\n"
        "  render-cli <input.wav> <output.wav> [options]\n"
        "  render-cli --list               the parameter table: names, ranges, defaults\n\n"
        "  --preset \"Name\"                 a factory preset first (PRODUCT_PRESETS); --set overrides it\n"
        "  --set \"Name=value\"              a parameter, in its own text (\"-6 dB\", \"1.2k\", \"50%%\")\n"
        "  --property \"Name=text\"          a property (product/Properties.h)\n"
        "  --at <frame> \"Name=value\"       the same, changed at that sample frame\n"
        "  --block <frames>                host block size (default 512); repeat to cycle sizes\n"
        "  --bits <24|32>                  24-bit PCM or 32-bit float (default)\n"
        "  --sidechain <key.wav>           the sidechain input (silence after it ends)\n"
        "  --offline                       render as a host bounces (a processor's kOfflineMode)\n"
        "Aux outputs are written beside the output as <output>-<bus name>.wav.\n",
        product::info::kName, product::info::kVersion);
}

void list() {
    std::printf("%-28s %-18s %-18s %s\n", "name", "min", "max", "default");
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (p.is(pflag::kHidden)) continue;
        char lo[48], hi[48], def[48];
        formatValue(p, p.hostMin(), lo, sizeof(lo));
        formatValue(p, p.hostMax(), hi, sizeof(hi));
        formatValue(p, hostDefault(p), def, sizeof(def));
        std::printf("%-28s %-18s %-18s %s%s\n", p.name, lo, hi, def, p.isOutput() ? "  (output)" : "");
    }
    for (size_t i = 0; i < properties::kCount; ++i)
        std::printf("%s%-28s \"%s\"%s\n", i == 0 ? "\nproperties\n" : "", properties::at(i).name,
                    properties::at(i).def, properties::at(i).perProjectOnly() ? "  (per project)" : "");
    for (size_t i = 0; i < presets::kFactoryCount; ++i)
        std::printf("%s%-28s %s\n", i == 0 ? "\nfactory presets\n" : "", presets::factory(i).name,
                    presets::factory(i).category);
}

struct Change {
    int frame;
    int index;
    double host;
};

bool parseAssignment(const char* text, int frame, std::vector<Change>& out, bool automation = false) {
    const char* eq = std::strchr(text, '=');
    if (!eq) return false;
    const std::string name(text, static_cast<size_t>(eq - text));
    for (size_t i = 0; i < params::kCount; ++i) {
        const ParamDef& p = params::at(i);
        if (name != p.name) continue;
        if (p.isOutput()) {
            std::fprintf(stderr, "error: %s is an output: the processor reports it\n", p.name);
            return false;
        }
        if (automation && p.is(pflag::kRestart)) {
            std::fprintf(stderr, "error: --at cannot automate %s: it requires a restart (use --set)\n", p.name);
            return false;
        }
        double host = 0.0;
        if (!parseValue(p, eq + 1, host)) {
            std::fprintf(stderr, "error: \"%s\" is not a value for %s\n", eq + 1, p.name);
            return false;
        }
        out.push_back({frame, static_cast<int>(i), host});
        return true;
    }
    std::fprintf(stderr, "error: no parameter named \"%s\" (try --list)\n", name.c_str());
    return false;
}

}  // namespace

// out.wav and "Ducked" -> out-Ducked.wav.
std::string auxPath(const std::string& output, const char* bus) {
    const size_t dot = output.rfind('.');
    const size_t slash = output.find_last_of("/\\");
    const bool extension = dot != std::string::npos && (slash == std::string::npos || dot > slash);
    std::string name(bus);
    for (char& ch : name)
        if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' && ch != '_') ch = '_';
    return (extension ? output.substr(0, dot) : output) + "-" + name + (extension ? output.substr(dot) : ".wav");
}

int main(int argc, char** argv) {
    if (product::info::kNoteEffect) {
        std::fprintf(stderr, "%s is a note effect: there is no audio to render\n", product::info::kName);
        return 2;
    }
    std::string input, output, sidechainFile;
    std::vector<Change> changes;
    std::vector<std::pair<size_t, std::string>> propertyValues;
    int preset = -1;
    std::vector<int> blocks;
    int bits = 32;
    bool offline = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") return usage(), 0;
        if (arg == "--list") return list(), 0;
        if (arg == "--set" && i + 1 < argc) {
            if (!parseAssignment(argv[++i], 0, changes)) return 2;
        } else if (arg == "--at" && i + 2 < argc) {
            const int frame = std::atoi(argv[++i]);
            if (!parseAssignment(argv[++i], std::max(0, frame), changes, true)) return 2;
        } else if (arg == "--preset" && i + 1 < argc) {
            preset = presets::findFactory(argv[++i]);
            if (preset < 0) {
                std::fprintf(stderr, "error: no factory preset named \"%s\" (try --list)\n", argv[i]);
                return 2;
            }
        } else if (arg == "--property" && i + 1 < argc) {
            const std::string text = argv[++i];
            const size_t eq = text.find('=');
            int k = -1;
            for (size_t j = 0; eq != std::string::npos && j < properties::kCount; ++j)
                if (text.compare(0, eq, properties::at(j).name) == 0 && std::strlen(properties::at(j).name) == eq)
                    k = static_cast<int>(j);
            if (k < 0 || !properties::accepts(static_cast<size_t>(k), text.substr(eq + 1))) {
                std::fprintf(stderr, "error: cannot set property \"%s\" (try --list)\n", text.c_str());
                return 2;
            }
            propertyValues.push_back({static_cast<size_t>(k), text.substr(eq + 1)});
        } else if (arg == "--block" && i + 1 < argc) {
            blocks.push_back(std::max(1, std::atoi(argv[++i])));
        } else if (arg == "--bits" && i + 1 < argc) {
            bits = std::atoi(argv[++i]);
        } else if (arg == "--sidechain" && i + 1 < argc) {
            sidechainFile = argv[++i];
        } else if (arg == "--offline") {
            offline = true;
        } else if (!arg.empty() && arg[0] != '-' && input.empty()) {
            input = arg;
        } else if (!arg.empty() && arg[0] != '-' && output.empty()) {
            output = arg;
        } else {
            usage();
            return 2;
        }
    }
    if (input.empty() || output.empty() || (bits != 24 && bits != 32)) {
        usage();
        return 2;
    }
    if (blocks.empty()) blocks.push_back(512);

    wavio::Audio audio;
    std::string error;
    if (!wavio::read(input, audio, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    const int channels = audio.channels;
    const int frames = static_cast<int>(audio.frames());
    bool supported = false;
    for (int layout : product::info::kChannelLayouts) supported |= layout == channels;
    if (!supported) {
        std::fprintf(stderr, "error: %d channels; this product takes", channels);
        for (int layout : product::info::kChannelLayouts) std::fprintf(stderr, " %d", layout);
        std::fprintf(stderr, "\n");
        return 1;
    }

    wavio::Audio key;
    if (!sidechainFile.empty()) {
        if (!product::info::kSidechain) {
            std::fprintf(stderr, "error: this product has no sidechain\n");
            return 2;
        }
        if (!wavio::read(sidechainFile, key, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        if (key.channels != channels || key.sampleRate != audio.sampleRate) {
            std::fprintf(stderr, "error: the sidechain must match the input: %d channels at %d Hz\n", channels,
                         audio.sampleRate);
            return 1;
        }
        for (auto& c : key.ch) c.resize(static_cast<size_t>(frames), 0.0);
    }
    std::vector<wavio::Audio> aux(static_cast<size_t>(product::info::kAuxOutputs), audio);
    for (auto& bus : aux)
        for (auto& c : bus.ch) std::fill(c.begin(), c.end(), 0.0);

    auto engine = std::make_unique<Engine>();
    if (preset >= 0) {
        // What loading it in the editor does: its values, defaults for the rest.
        const Preset& p = presets::factory(static_cast<size_t>(preset));
        for (const PresetValue& v : p.values) {
            const int i = params::indexOf(v.id);
            if (i >= 0) engine->store().set(static_cast<size_t>(i), toHost(params::at(static_cast<size_t>(i)), v.plain));
        }
        for (const PresetText& t : p.texts) {
            const int i = properties::indexOf(t.id);
            if (i >= 0) engine->setProperty(static_cast<size_t>(i), t.value, true);
        }
    }
    for (const auto& [k, value] : propertyValues) engine->setProperty(k, value, true);
    std::stable_sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.frame < b.frame; });
    size_t next = 0;
    while (next < changes.size() && changes[next].frame == 0) {
        engine->store().set(static_cast<size_t>(changes[next].index), changes[next].host);
        ++next;
    }
    const int maxBlock = *std::max_element(blocks.begin(), blocks.end());
    engine->setOffline(offline);
    engine->activate(audio.sampleRate, maxBlock, product::info::kAudioInput ? channels : 0, channels);

    struct Source {
        const std::vector<Change>* changes;
        size_t* cursor;
        int start, end;
        bool next(EngineEvent& e) {
            if (*cursor >= changes->size() || (*changes)[*cursor].frame >= end) return false;
            const Change& c = (*changes)[(*cursor)++];
            e = EngineEvent{};
            e.type = EngineEvent::Type::Value;
            e.time = static_cast<uint32_t>(std::max(0, c.frame - start));
            e.index = c.index;
            e.value = c.host;
            return true;
        }
    };

    std::vector<double*> pointers(static_cast<size_t>(channels));
    std::vector<const double*> keys(static_cast<size_t>(channels));
    std::vector<std::vector<double*>> auxChannels(aux.size(), std::vector<double*>(static_cast<size_t>(channels)));
    std::vector<double* const*> auxBuses(aux.size());
    const auto started = std::chrono::steady_clock::now();
    for (int frame = 0, b = 0; frame < frames; ++b) {
        const int n = std::min(blocks[static_cast<size_t>(b) % blocks.size()], frames - frame);
        for (size_t c = 0; c < static_cast<size_t>(channels); ++c) {
            pointers[c] = audio.ch[c].data() + frame;
            if (!key.ch.empty()) keys[c] = key.ch[c].data() + frame;
            for (size_t k = 0; k < aux.size(); ++k) auxChannels[k][c] = aux[k].ch[c].data() + frame;
        }
        for (size_t k = 0; k < aux.size(); ++k) auxBuses[k] = auxChannels[k].data();
        AudioBuses<double> io;
        io.in = product::info::kAudioInput ? pointers.data() : nullptr;
        io.out = pointers.data();
        io.sidechain = key.ch.empty() ? nullptr : keys.data();
        io.aux = aux.empty() ? nullptr : auxBuses.data();
        Source source{&changes, &next, frame, frame + n};
        engine->process(io, n, source, nullptr);
        frame += n;
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();

    if (!wavio::write(output, audio, bits, error)) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    for (size_t k = 0; k < aux.size(); ++k) {
        const std::string path = auxPath(output, product::info::kAuxOutputNames[k]);
        if (!wavio::write(path, aux[k], bits, error)) {
            std::fprintf(stderr, "error: %s\n", error.c_str());
            return 1;
        }
        std::printf("  %s -> %s\n", product::info::kAuxOutputNames[k], path.c_str());
    }
    double peak = 0.0;
    for (const auto& ch : audio.ch)
        for (double x : ch) peak = std::max(peak, std::fabs(x));
    const double duration = frames / static_cast<double>(audio.sampleRate);
    std::printf("%s -> %s: %d ch, %d Hz, %.2f s, latency %d, output peak %.2f dBFS, %.0fx realtime\n",
                input.c_str(), output.c_str(), channels, audio.sampleRate, duration, engine->latency(),
                dsp::gainToDb(peak), duration / std::max(seconds, 1e-9));
    for (uint16_t i : params::kOutputIndices) {
        char value[64];
        formatValue(params::at(i), engine->store().get(i), value, sizeof(value));
        std::printf("  %s at the end: %s\n", params::at(i).name, value);
    }
    if (engine->safetyResets())
        std::printf("warning: %u blocks produced non-finite output and were silenced\n", engine->safetyResets());
    return 0;
}
