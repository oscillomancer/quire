// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Exercise the actual CLAP binary while the main thread replaces patches.
#include <clap/clap.h>
#include <dlfcn.h>
#include <supple/clap/semantic_params.h>
#include <supple/json/json.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {
const void* CLAP_ABI hostExt(const clap_host_t*, const char*) { return nullptr; }
void CLAP_ABI hostRequest(const clap_host_t*) {}
bool CLAP_ABI acceptEvent(const clap_output_events_t*, const clap_event_header_t*) { return true; }
int64_t CLAP_ABI writeText(const clap_ostream_t* stream, const void* data, uint64_t size) {
    static_cast<std::string*>(stream->ctx)->append(static_cast<const char*>(data), size);
    return static_cast<int64_t>(size);
}
struct EventList {
    clap_event_note_t note{};
    bool send = true;
    clap_input_events_t iface{
        this,
        [](const clap_input_events_t* e) -> uint32_t {
            return static_cast<EventList*>(e->ctx)->send ? 1u : 0u;
        },
        [](const clap_input_events_t* e, uint32_t) -> const clap_event_header_t* {
            return &static_cast<EventList*>(e->ctx)->note.header;
        }};
};
struct Input {
    std::vector<uint8_t> bytes;
    size_t offset = 0;
    clap_istream_t stream{this, [](const clap_istream_t* s, void* out, uint64_t count) -> int64_t {
                              auto* self = static_cast<Input*>(s->ctx);
                              const auto n =
                                  std::min<size_t>(count, self->bytes.size() - self->offset);
                              std::memcpy(out, self->bytes.data() + self->offset, n);
                              self->offset += n;
                              return static_cast<int64_t>(n);
                          }};
};
void put32(std::vector<uint8_t>& b, uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void putDouble(std::vector<uint8_t>& b, double v) {
    uint64_t bits = 0;
    std::memcpy(&bits, &v, 8);
    for (int i = 0; i < 8; ++i) b.push_back(uint8_t(bits >> (8 * i)));
}
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) {
        std::fprintf(stderr, "%s\n", dlerror());
        return 1;
    }
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(lib, "clap_entry"));
    if (!entry || !entry->init(argv[1])) return 1;
    const auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    clap_host_t host{};
    host.clap_version = CLAP_VERSION;
    host.name = "probe semantic RT test";
    host.vendor = "Supple Audio";
    host.version = "1";
    host.get_extension = hostExt;
    host.request_restart = hostRequest;
    host.request_process = hostRequest;
    host.request_callback = hostRequest;
    const auto* plugin = factory->create_plugin(factory, &host, "com.suppleaudio.probe-synth");
    if (!plugin || !plugin->init(plugin)) return 1;
    const auto* sem = static_cast<const clap_plugin_supple_semantic_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    const auto* state =
        static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    if (!sem || !state) return 1;

    // A binary v1 load must replace the previous JSON patch's envelope seed.
    const std::string seeded =
        R"({"schema":"supple.probe-synth-patch/1.0","seed":123,"params":{}})";
    std::string seedError;
    clap_ostream_t seedErrors{&seedError, writeText};
    if (!sem->set_patch(plugin, CLAP_SUPPLE_SET_REPLACE, seeded.c_str(),
                        static_cast<uint32_t>(seeded.size()), &seedErrors)) {
        std::fprintf(stderr, "seeded patch: %s\n", seedError.c_str());
        return 1;
    }

    // Template binary v1: cutoff 1200 Hz, amp sustain 0.5 ratio. Fractional
    // waveform and slope values used to truncate in the old DSP.
    Input legacy;
    put32(legacy.bytes, 0x51505231);
    put32(legacy.bytes, 1);
    put32(legacy.bytes, 4);
    put32(legacy.bytes, 1);
    putDouble(legacy.bytes, 1200);
    put32(legacy.bytes, 7);
    putDouble(legacy.bytes, 0.5);
    put32(legacy.bytes, 0);
    putDouble(legacy.bytes, 0.6);
    put32(legacy.bytes, 4);
    putDouble(legacy.bytes, 0.6);
    if (!state->load(plugin, &legacy.stream)) {
        std::fprintf(stderr, "legacy load failed\n");
        return 1;
    }
    std::string patchText;
    clap_ostream_t patchOut{&patchText, writeText};
    if (!sem->get_patch(plugin, CLAP_SUPPLE_PATCH_DETAIL_FULL, &patchOut)) {
        std::fprintf(stderr, "get_patch failed\n");
        return 1;
    }
    auto parsed = supple::json::parse(patchText);
    const auto* body = parsed ? parsed->find("params") : nullptr;
    const auto* seed = parsed ? parsed->find("seed") : nullptr;
    if (!body || !seed || seed->asDouble() != 0.0 || !body->find("amp.sustain") ||
        !body->find("filter.cutoff") || !body->find("osc.waveform") ||
        !body->find("filter.slope") || body->find("amp.sustain")->asDouble() != 50.0 ||
        body->find("filter.cutoff")->asDouble() != 1200.0 ||
        body->find("osc.waveform")->asString() != "saw" ||
        body->find("filter.slope")->asString() != "db12") {
        std::fprintf(stderr, "legacy patch: %s\n", patchText.c_str());
        return 1;
    }
    Input truncated;
    truncated.bytes = legacy.bytes;
    truncated.bytes.pop_back();
    if (state->load(plugin, &truncated.stream)) return 1;
    std::string afterBadLoad;
    clap_ostream_t afterBadOut{&afterBadLoad, writeText};
    if (!sem->get_patch(plugin, CLAP_SUPPLE_PATCH_DETAIL_FULL, &afterBadOut) ||
        afterBadLoad != patchText)
        return 1;

    if (!plugin->activate(plugin, 48000, 64, 64) || !plugin->start_processing(plugin)) {
        std::fprintf(stderr, "activation failed\n");
        return 1;
    }
    float left[64]{}, right[64]{};
    float* channels[]{left, right};
    clap_audio_buffer_t output{};
    output.data32 = channels;
    output.channel_count = 2;
    EventList events;
    events.note.header.size = sizeof(events.note);
    events.note.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    events.note.header.type = CLAP_EVENT_NOTE_ON;
    events.note.note_id = 1;
    events.note.port_index = 0;
    events.note.channel = 0;
    events.note.key = 60;
    events.note.velocity = 0.8;
    clap_output_events_t outEvents{nullptr, acceptEvent};
    clap_process_t process{};
    process.frames_count = 64;
    process.audio_outputs = &output;
    process.audio_outputs_count = 1;
    process.in_events = &events.iface;
    process.out_events = &outEvents;
    std::atomic<bool> running{true}, bad{false};
    std::atomic<uint32_t> blocks{0};
    std::thread audio([&] {
        while (running.load(std::memory_order_acquire)) {
            events.send = blocks.load(std::memory_order_relaxed) == 0;
            if (plugin->process(plugin, &process) != CLAP_PROCESS_CONTINUE) bad = true;
            for (float value : left)
                if (!std::isfinite(value)) bad = true;
            ++blocks;
        }
    });
    while (blocks.load(std::memory_order_acquire) < 8) std::this_thread::yield();
    for (int i = 0; i < 100; ++i) {
        const std::string text =
            "{\"schema\":\"supple.probe-synth-patch/1.0\",\"seed\":" + std::to_string(i) +
            ",\"params\":{\"filter.cutoff\":" + std::to_string(100 + i * 40) +
            ",\"osc.waveform\":\"sine\"}}";
        std::string error;
        clap_ostream_t errors{&error, writeText};
        if (!sem->set_patch(plugin, CLAP_SUPPLE_SET_REPLACE, text.c_str(),
                            static_cast<uint32_t>(text.size()), &errors)) {
            std::fprintf(stderr, "set_patch: %s\n", error.c_str());
            bad = true;
            break;
        }
    }
    running.store(false, std::memory_order_release);
    audio.join();
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    dlclose(lib);
    if (bad || blocks < 8) return 1;
    std::printf("set_patch during %u processing blocks: ok\n", blocks.load());
    return 0;
}
