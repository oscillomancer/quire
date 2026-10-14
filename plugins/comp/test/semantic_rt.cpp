// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
// Exercise the actual CLAP binary while the main thread replaces patches.
#include <clap/clap.h>
#include "supple/plugin/test_module.h"
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
}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    void* lib = supple::plugin::test::open(argv[1]);
    if (!lib) {
        std::fprintf(stderr, "%s\n", supple::plugin::test::error());
        return 1;
    }
    auto* entry = static_cast<const clap_plugin_entry_t*>(supple::plugin::test::symbol(lib, "clap_entry"));
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
    const auto* plugin = factory->create_plugin(factory, &host, "com.suppleaudio.comp");
    if (!plugin || !plugin->init(plugin)) return 1;
    const auto* sem = static_cast<const clap_plugin_supple_semantic_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    const auto* state =
        static_cast<const clap_plugin_state_t*>(plugin->get_extension(plugin, CLAP_EXT_STATE));
    if (!sem || !state) return 1;

    const std::string patch =
        R"({"schema":"supple.comp-patch/1.0","params":{"threshold":-24,"ratio":6}})";
    std::string errorsText;
    clap_ostream_t errors{&errorsText, writeText};
    if (!sem->set_patch(plugin, CLAP_SUPPLE_SET_REPLACE, patch.c_str(),
                        static_cast<uint32_t>(patch.size()), &errors))
        return 1;
    std::string patchText;
    clap_ostream_t patchOut{&patchText, writeText};
    if (!sem->get_patch(plugin, CLAP_SUPPLE_PATCH_DETAIL_FULL, &patchOut)) return 1;
    std::string saved;
    clap_ostream_t savedOut{&saved, writeText};
    if (!state->save(plugin, &savedOut)) return 1;
    Input truncated;
    truncated.bytes.assign(saved.begin(), saved.end() - 1);
    if (state->load(plugin, &truncated.stream)) return 1;
    std::string after;
    clap_ostream_t afterOut{&after, writeText};
    if (!sem->get_patch(plugin, CLAP_SUPPLE_PATCH_DETAIL_FULL, &afterOut) || after != patchText)
        return 1;
    Input restored;
    restored.bytes.assign(saved.begin(), saved.end());
    if (!state->load(plugin, &restored.stream)) return 1;
    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    clap_audio_port_info_t key{};
    if (!ports || ports->count(plugin, true) != 2 || !ports->get(plugin, 1, true, &key) ||
        key.channel_count != 2 || (key.flags & CLAP_AUDIO_PORT_IS_MAIN) ||
        key.in_place_pair != CLAP_INVALID_ID)
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
    events.send = false;
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
            events.send = false;
            for (int i = 0; i < 64; ++i) {
                left[i] = .1f;
                right[i] = -.05f;
            }
            if (plugin->process(plugin, &process) != CLAP_PROCESS_CONTINUE) bad = true;
            for (float value : left)
                if (!std::isfinite(value)) bad = true;
            ++blocks;
        }
    });
    while (blocks.load(std::memory_order_acquire) < 8) std::this_thread::yield();
    for (int i = 0; i < 100; ++i) {
        const std::string text =
            "{\"schema\":\"supple.comp-patch/1.0\",\"params\":{\"threshold\":" +
            std::to_string(-10 - i % 40) + ",\"ratio\":10}}";
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
    supple::plugin::test::close(lib);
    if (bad || blocks < 8) return 1;
    std::printf("set_patch during %u processing blocks: ok\n", blocks.load());
    return 0;
}
