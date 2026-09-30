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
int restarts = 0;
void CLAP_ABI hostRestart(const clap_host_t*) { ++restarts; }
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
    host.request_restart = hostRestart;
    host.request_process = hostRequest;
    host.request_callback = hostRequest;
    const auto* plugin = factory->create_plugin(factory, &host, "com.suppleaudio.eq");
    if (!plugin || !plugin->init(plugin)) return 1;
    const auto* sem = static_cast<const clap_plugin_supple_semantic_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    const auto* state =
        static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    if (!sem || !state) return 1;

    const std::string initial = R"({"schema":"supple.eq-patch/1.0","params":{"band1.freq":80}})";
    std::string errors;
    clap_ostream_t errorOut{&errors, writeText};
    if (!sem->set_patch(plugin, CLAP_SUPPLE_SET_REPLACE, initial.c_str(),
                        static_cast<uint32_t>(initial.size()), &errorOut))
        return 1;
    std::string saved;
    clap_ostream_t saveOut{&saved, writeText};
    if (!state->save(plugin, &saveOut)) return 1;
    Input corrupt;
    corrupt.bytes.assign(saved.begin(), saved.end());
    corrupt.bytes.pop_back();
    if (state->load(plugin, &corrupt.stream)) return 1;
    std::string before;
    clap_ostream_t patchOut{&before, writeText};
    if (!sem->get_patch(plugin, CLAP_SUPPLE_PATCH_DETAIL_FULL, &patchOut)) return 1;
    Input good;
    good.bytes.assign(saved.begin(), saved.end());
    if (!state->load(plugin, &good.stream)) return 1;
    std::string after;
    clap_ostream_t afterOut{&after, writeText};
    if (!sem->get_patch(plugin, CLAP_SUPPLE_PATCH_DETAIL_FULL, &afterOut) || before != after)
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
    process.audio_inputs = &output;
    process.audio_inputs_count = 1;
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
        const std::string text = "{\"schema\":\"supple.eq-patch/1.0\",\"params\":{\"band1.freq\":" +
                                 std::to_string(100 + i * 40) + ",\"band2.gain\":-3}}";
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
    // Phase is stepped, nonautomatable, and changes latency only on reactivation.
    const auto* params =
        static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    const auto* latency =
        static_cast<const clap_plugin_latency_t*>(plugin->get_extension(plugin, CLAP_EXT_LATENCY));
    clap_param_info_t info{};
    if (!params->get_info(plugin, 56, &info) || (info.flags & CLAP_PARAM_IS_AUTOMATABLE)) return 1;
    if (!plugin->activate(plugin, 48000, 64, 64) || !plugin->start_processing(plugin)) return 1;
    const auto oldLatency = latency->get(plugin);
    const std::string linear =
        R"({"schema":"supple.eq-patch/1.0","params":{"phase":"linear","band1.enabled":false,"band2.enabled":false,"band3.enabled":false,"band4.enabled":false}})";
    if (!sem->set_patch(plugin, CLAP_SUPPLE_SET_REPLACE, linear.c_str(),
                        static_cast<uint32_t>(linear.size()), &errorOut))
        return 1;
    plugin->on_main_thread(plugin);
    plugin->on_main_thread(plugin);
    if (restarts != 1 || latency->get(plugin) != oldLatency) return 1;
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    if (!plugin->activate(plugin, 48000, 64, 64) || !plugin->start_processing(plugin) ||
        latency->get(plugin) != 2048)
        return 1;
    for (int block = 0; block < 70; ++block) {
        for (int k = 0; k < 64; ++k) left[k] = right[k] = block == 0 && k == 0 ? 1.f : 0.f;
        events.send = false;
        if (plugin->process(plugin, &process) != CLAP_PROCESS_CONTINUE) return 1;
        for (int k = 0; k < 64; ++k)
            if (std::abs(left[k] - (block * 64 + k == 2048 ? 1.f : 0.f)) > 1e-6f) return 1;
    }
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    dlclose(lib);
    if (bad || blocks < 8) return 1;
    std::printf("set_patch during %u processing blocks: ok\n", blocks.load());
    return 0;
}
