// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Saved state is untrusted input: a host hands the plugin whatever bytes a
// project file, a preset or the clipboard holds. This loads damaged ones
// into the real plugin, through CLAP, and checks what must hold for any
// input at all:
//
//   - nothing crashes, and no sanitizer has anything to say;
//   - a refused load changes nothing: the state saves as it did before;
//   - an accepted one saves, loads into a fresh instance and saves again
//     byte for byte;
//   - a plugin that accepted it processes finite audio.
//
// One file, two programs:
//   state-fuzz (every preset, a ctest): every kind of state the product
//     makes -- defaults and random settings, in the session and preset
//     contexts, and as pasted text -- each damaged a few hundred ways from a
//     fixed seed, so a failure always reproduces.
//     `state-fuzz --seeds DIR` writes those states as a corpus.
//   state-fuzzer (the fuzz preset; clang's libFuzzer): the same check, run
//     by ctest for a minute from that corpus. For longer:
//     build/fuzz/state-fuzzer build/fuzz/state-corpus -max_total_time=3600
//
// An input is a mode byte, then the bytes: bit 0 loads in the preset
// context, bit 1 reads the bytes as pasted text (base64), bit 2 also
// activates and processes.
#include <clap/clap.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <string>
#include <vector>

#include "core/ParamTable.h"
#include "core/PresetLibrary.h"
#include "core/PropertyTable.h"
#include "core/StateFormat.h"
#include "plugin/Entry.h"
#include "ProductInfo.h"

using namespace supple;

namespace {

#define FUZZ_CHECK(cond, ...)                                                \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::fprintf(stderr, "state-fuzz: %s failed: ", #cond);          \
            std::fprintf(stderr, __VA_ARGS__);                               \
            std::fprintf(stderr, "\n");                                      \
            std::abort();                                                    \
        }                                                                    \
    } while (0)

enum Mode : uint8_t { kPreset = 1, kText = 2, kProcess = 4 };

[[maybe_unused]] int gAccepted = 0, gProcessed = 0;  // reported by the deterministic run

const clap_host_t* host() {
    static clap_host_t h{};
    h.clap_version = CLAP_VERSION;
    h.name = "state-fuzz";
    h.vendor = "";
    h.url = "";
    h.version = "1";
    h.get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
    h.request_restart = [](const clap_host_t*) {};
    h.request_process = [](const clap_host_t*) {};
    h.request_callback = [](const clap_host_t*) {};
    return &h;
}

struct Instance {
    const clap_plugin_t* plugin = nullptr;
    const clap_plugin_state_context_t* context = nullptr;

    Instance() {
        const auto* factory = static_cast<const clap_plugin_factory_t*>(supple_entry_get_factory(CLAP_PLUGIN_FACTORY_ID));
        plugin = factory->create_plugin(factory, host(), product::info::kId);
        plugin->init(plugin);
        context = static_cast<const clap_plugin_state_context_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE_CONTEXT));
    }
    ~Instance() { plugin->destroy(plugin); }

    std::vector<uint8_t> save(uint32_t type = CLAP_STATE_CONTEXT_FOR_PROJECT) {
        std::vector<uint8_t> bytes;
        clap_ostream_t os{&bytes, [](const clap_ostream_t* s, const void* p, uint64_t n) -> int64_t {
                              auto* b = static_cast<std::vector<uint8_t>*>(s->ctx);
                              b->insert(b->end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
                              return static_cast<int64_t>(n);
                          }};
        context->save(plugin, &os, type);
        return bytes;
    }

    bool load(const std::vector<uint8_t>& bytes, uint32_t type) {
        struct Reader {
            const std::vector<uint8_t>* bytes;
            size_t at = 0;
        } reader{&bytes};
        clap_istream_t is{&reader, [](const clap_istream_t* s, void* p, uint64_t n) -> int64_t {
                              auto* r = static_cast<Reader*>(s->ctx);
                              const size_t take = std::min<size_t>(static_cast<size_t>(n), r->bytes->size() - r->at);
                              if (take) std::memcpy(p, r->bytes->data() + r->at, take);
                              r->at += take;
                              return static_cast<int64_t>(take);
                          }};
        return context->load(plugin, &is, type);
    }

    // A few blocks of noise (and a held note, for a product that takes
    // them), every output finite.
    void process() {
        FUZZ_CHECK(plugin->activate(plugin, 48000.0, 1, 256), "activation refused");
        plugin->start_processing(plugin);
        const uint32_t channels = static_cast<uint32_t>(product::info::kChannelLayouts[0]);
        std::vector<std::vector<float>> in(channels, std::vector<float>(256)), out(channels, std::vector<float>(256));
        std::vector<float*> inPtr(channels), outPtr(channels);
        for (uint32_t c = 0; c < channels; ++c) {
            inPtr[c] = in[c].data();
            outPtr[c] = out[c].data();
        }
        clap_audio_buffer_t inBus{inPtr.data(), nullptr, channels, 0, 0};
        clap_audio_buffer_t outBus{outPtr.data(), nullptr, channels, 0, 0};
        clap_event_note_t note{{sizeof(clap_event_note_t), 0, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_NOTE_ON, 0}, 1, 0, 0, 60, 0.8};
        struct Events {
            const clap_event_note_t* note;
            bool give;
        } events{&note, product::info::kNoteInput};
        clap_input_events_t inEvents{&events,
                                     [](const clap_input_events_t* l) -> uint32_t { return static_cast<const Events*>(l->ctx)->give ? 1 : 0; },
                                     [](const clap_input_events_t* l, uint32_t) -> const clap_event_header_t* {
                                         return &static_cast<const Events*>(l->ctx)->note->header;
                                     }};
        clap_output_events_t outEvents{nullptr, [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
        std::minstd_rand rng(1);
        for (int block = 0; block < 8; ++block) {
            for (auto& channel : in)
                for (float& x : channel) x = static_cast<float>(rng() % 2001) / 1000.0f - 1.0f;
            clap_process_t p{};
            p.steady_time = block * 256;
            p.frames_count = 256;
            p.audio_inputs = product::info::kAudioInput ? &inBus : nullptr;
            p.audio_inputs_count = product::info::kAudioInput ? 1 : 0;
            p.audio_outputs = channels ? &outBus : nullptr;
            p.audio_outputs_count = channels ? 1 : 0;
            p.in_events = &inEvents;
            p.out_events = &outEvents;
            plugin->process(plugin, &p);
            events.give = false;
            for (const auto& channel : out)
                for (float x : channel) FUZZ_CHECK(std::isfinite(x), "non-finite output in block %d", block);
        }
        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
    }
};

void fuzzOne(const uint8_t* data, size_t size) {
    if (size == 0) return;
    const uint8_t mode = data[0];
    std::vector<uint8_t> bytes(data + 1, data + size);
    if (mode & kText) {
        std::vector<uint8_t> decoded;
        if (!base64::decode(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()), decoded)) return;
        bytes = std::move(decoded);
    }
    const uint32_t type = (mode & kPreset) ? CLAP_STATE_CONTEXT_FOR_PRESET : CLAP_STATE_CONTEXT_FOR_PROJECT;
    Instance a;
    const std::vector<uint8_t> before = a.save();
    if (!a.load(bytes, type)) {
        FUZZ_CHECK(a.save() == before, "a refused load of %zu bytes changed the state", bytes.size());
        return;
    }
    ++gAccepted;
    const std::vector<uint8_t> once = a.save();
    {
        Instance b;
        FUZZ_CHECK(b.load(once, CLAP_STATE_CONTEXT_FOR_PROJECT), "the plugin refused its own state");
        FUZZ_CHECK(b.save() == once, "a loaded state did not save again byte for byte");
    }
    if (mode & kProcess) {
        a.process();
        ++gProcessed;
    }
}

// --- the deterministic run -----------------------------------------------------------

#ifndef SUPPLE_LIBFUZZER

std::vector<uint8_t> stateWith(std::mt19937& rng, bool random, state::Context context) {
    state::Snapshot s;
    s.defaultProperties();
    if (random) {
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        for (size_t i = 0; i < params::kCount; ++i) {
            const ParamDef& p = params::at(i);
            s.host[i] = sanitizeHost(p, p.hostMin() + unit(rng) * (p.hostMax() - p.hostMin()));
        }
        s.editorScale = 1.0 + unit(rng);
        s.hasPreset = true;
        s.presetName = "Fuzzed";
        s.presetModified = true;
        static const char* const texts[] = {"", "x", "C E G", "../../etc/passwd.wav", "\xff\xfe", "Am7 / D9 / G"};
        for (size_t i = 0; i < properties::kCount; ++i) {
            const std::string text = texts[rng() % (sizeof(texts) / sizeof(texts[0]))];
            if (properties::accepts(i, text)) s.property[i] = text;
        }
    }
    return state::save(state::fourcc(product::info::kStateTag), s, context);
}

std::vector<std::vector<uint8_t>> seeds() {
    std::mt19937 rng(7);
    std::vector<std::vector<uint8_t>> out;
    auto add = [&](uint8_t mode, const std::vector<uint8_t>& payload) {
        std::vector<uint8_t> input{mode};
        input.insert(input.end(), payload.begin(), payload.end());
        out.push_back(std::move(input));
    };
    for (int k = 0; k < 4; ++k) {
        add(0, stateWith(rng, k > 0, state::Context::Project));
        add(kPreset, stateWith(rng, k > 0, state::Context::Preset));
    }
    const std::string text = base64::encode(stateWith(rng, true, state::Context::Preset));
    add(kText | kPreset, std::vector<uint8_t>(text.begin(), text.end()));
    add(kProcess, stateWith(rng, true, state::Context::Project));
    return out;
}

// One input damaged: bits flipped, cut short, bytes inserted, a length or
// count replaced with an awkward number, a run repeated.
std::vector<uint8_t> damage(std::vector<uint8_t> x, std::mt19937& rng) {
    const int edits = 1 + static_cast<int>(rng() % 4);
    for (int e = 0; e < edits && x.size() > 1; ++e) {
        const size_t at = 1 + rng() % (x.size() - 1);  // the mode byte stays
        switch (rng() % 5) {
            case 0: x[at] ^= static_cast<uint8_t>(1u << (rng() % 8)); break;
            case 1: x.resize(at); break;
            case 2: x.insert(x.begin() + static_cast<std::ptrdiff_t>(at), static_cast<uint8_t>(rng())); break;
            case 3: {
                static const uint32_t awkward[] = {0u, 1u, 0x7fffffffu, 0xffffffffu, 0x80000000u, 12u, 255u, 65536u};
                const uint32_t v = awkward[rng() % 8];
                for (size_t k = 0; k < 4 && at + k < x.size(); ++k) x[at + k] = static_cast<uint8_t>(v >> (8 * k));
                break;
            }
            default: {
                const size_t length = std::min<size_t>(1 + rng() % 24, x.size() - at);
                std::vector<uint8_t> run(x.begin() + static_cast<std::ptrdiff_t>(at),
                                         x.begin() + static_cast<std::ptrdiff_t>(at + length));
                x.insert(x.begin() + static_cast<std::ptrdiff_t>(at), run.begin(), run.end());
            }
        }
    }
    return x;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc == 3 && !std::strcmp(argv[1], "--seeds")) {
        std::filesystem::create_directories(argv[2]);
        int n = 0;
        for (const auto& seed : seeds()) {
            std::ofstream f(std::filesystem::path(argv[2]) / ("seed-" + std::to_string(n++)), std::ios::binary);
            f.write(reinterpret_cast<const char*>(seed.data()), static_cast<std::streamsize>(seed.size()));
        }
        std::printf("wrote %d seeds to %s\n", n, argv[2]);
        return 0;
    }
    const int perSeed = argc > 1 ? std::atoi(argv[1]) : 300;
    std::mt19937 rng(2026);
    int inputs = 0;
    for (const auto& seed : seeds()) {
        fuzzOne(seed.data(), seed.size());
        ++inputs;
        for (int k = 0; k < perSeed; ++k) {
            std::vector<uint8_t> x = damage(seed, rng);
            if (rng() % 64 == 0) x[0] |= kProcess;
            fuzzOne(x.data(), x.size());
            ++inputs;
        }
    }
    std::printf("%d inputs, %d accepted and round-tripped, %d processed; every refusal changed nothing\n", inputs,
                gAccepted, gProcessed);
    if (gAccepted == 0 || gAccepted == inputs) return 1;  // the damage or the seeds are not what they should be
    return 0;
}

#else

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzzOne(data, size);
    return 0;
}

#endif
