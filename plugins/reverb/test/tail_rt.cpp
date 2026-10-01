// SPDX-License-Identifier: MIT
#include <clap/clap.h>
#include <clap/ext/tail.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include "ReverbParams.h"
#include "supple/clap/semantic_params.h"
namespace {
thread_local bool inProcess = false;
std::atomic<unsigned> notifications{0}, wrongThread{0};
void CLAP_ABI changed(const clap_host_t*) {
    if (!inProcess) ++wrongThread;
    ++notifications;
}
const clap_host_tail_t hostTail{changed};
const void* CLAP_ABI extension(const clap_host_t*, const char* id) {
    return !std::strcmp(id, CLAP_EXT_TAIL) ? &hostTail : nullptr;
}
void CLAP_ABI request(const clap_host_t*) {}
int64_t CLAP_ABI discard(const clap_ostream_t*, const void*, uint64_t n) {
    return static_cast<int64_t>(n);
}
}  // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    void* library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!library) return 1;
    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(library, "clap_entry"));
    if (!entry || !entry->init(argv[1])) return 1;
    auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    clap_host_t host{CLAP_VERSION, nullptr,   "Tail host", "Supple Audio", "",
                     "1",          extension, request,     request,        request};
    const auto* plugin = factory->create_plugin(factory, &host, "com.suppleaudio.reverb");
    if (!plugin || !plugin->init(plugin)) return 1;
    const auto* tail =
        static_cast<const clap_plugin_tail_t*>(plugin->get_extension(plugin, CLAP_EXT_TAIL));
    const auto* sem = static_cast<const clap_plugin_supple_semantic_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    const auto* params =
        static_cast<const clap_plugin_params_t*>(plugin->get_extension(plugin, CLAP_EXT_PARAMS));
    if (!tail || !sem || !params || tail->get(plugin) != 149280) return 1;
    if (!plugin->activate(plugin, 48000, 64, 64) || !plugin->start_processing(plugin)) return 1;
    double x[64]{}, y[64]{};
    double* data[] = {x, y};
    clap_audio_buffer_t audio{};
    audio.data64 = data;
    audio.channel_count = 2;
    clap_process_t process{};
    process.frames_count = 64;
    process.audio_inputs = &audio;
    process.audio_inputs_count = 1;
    process.audio_outputs = &audio;
    process.audio_outputs_count = 1;
    auto run = [&] {
        inProcess = true;
        const auto result = plugin->process(plugin, &process);
        inProcess = false;
        return result != CLAP_PROCESS_ERROR;
    };
    if (!run()) return 1;
    clap_ostream_t errors{nullptr, discard};
    const char* patch = R"({"params":{"decay":4000,"low_decay":2}})";
    if (!sem->set_patch(plugin, CLAP_SUPPLE_SET_MERGE, patch,
                        static_cast<uint32_t>(std::strlen(patch)), &errors))
        return 1;
    const auto before = notifications.load();
    if (wrongThread || tail->get(plugin) != 581280) return 1;
    if (!run() || notifications != before + 1) return 1;
    clap_event_param_value_t event{};
    event.header = {sizeof(event), 17, CLAP_CORE_EVENT_SPACE_ID, CLAP_EVENT_PARAM_VALUE, 0};
    event.param_id = supple::reverb::kFreeze;
    event.value = 1;
    clap_input_events_t events{
        &event, [](const clap_input_events_t*) -> uint32_t { return 1; },
        [](const clap_input_events_t* e, uint32_t) -> const clap_event_header_t* {
            return &static_cast<const clap_event_param_value_t*>(e->ctx)->header;
        }};
    process.in_events = &events;
    if (!run() || tail->get(plugin) != UINT32_MAX) return 1;
    // Inactive flush and state edits never make an audio-thread callback directly.
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    event.value = 0;
    params->flush(plugin, &events, nullptr);
    if (wrongThread || tail->get(plugin) != 581280) return 1;
    plugin->destroy(plugin);
    entry->deinit();
    dlclose(library);
    std::printf("Tail notifications: %u, all from process\n", notifications.load());
    return 0;
}
