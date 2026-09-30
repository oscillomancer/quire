// SPDX-License-Identifier: MIT
#include <clap/clap.h>
#include <clap/ext/tail.h>
#include <dlfcn.h>
#include <cmath>
#include <cstdio>
#include <string>
#include "supple/clap/semantic_params.h"
namespace {
bool processing = false, wrongThread = false;
int notifications = 0;
void CLAP_ABI tailChanged(const clap_host_t*) {
    ++notifications;
    wrongThread |= !processing;
}
const clap_host_tail_t tailHost{tailChanged};
const void* CLAP_ABI extension(const clap_host_t*, const char* id) {
    return std::string_view(id) == CLAP_EXT_TAIL ? &tailHost : nullptr;
}
void CLAP_ABI request(const clap_host_t*) {}
int64_t CLAP_ABI write(const clap_ostream_t* out, const void* data, uint64_t n) {
    static_cast<std::string*>(out->ctx)->append(static_cast<const char*>(data), n);
    return int64_t(n);
}
}  // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    void* library = dlopen(argv[1], RTLD_LOCAL | RTLD_NOW);
    if (!library) return 1;
    const auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(library, "clap_entry"));
    if (!entry || !entry->init(argv[1])) return 1;
    const auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    clap_host_t host{CLAP_VERSION_INIT, nullptr, "tail transport", "Supple Audio", "", "1",
                     extension,         request, request,          request};
    const auto* plugin = factory->create_plugin(factory, &host, "com.suppleaudio.delay");
    if (!plugin || !plugin->init(plugin)) return 1;
    const auto* tail =
        static_cast<const clap_plugin_tail_t*>(plugin->get_extension(plugin, CLAP_EXT_TAIL));
    const auto* semantic = static_cast<const clap_plugin_supple_semantic_params_t*>(
        plugin->get_extension(plugin, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    if (!tail || !semantic || !plugin->activate(plugin, 48000, 1, 256) ||
        !plugin->start_processing(plugin))
        return 1;
    float l[256]{}, r[256]{};
    float* buffers[]{l, r};
    clap_audio_buffer_t input{}, output{};
    input.data32 = output.data32 = buffers;
    input.channel_count = output.channel_count = 2;
    clap_event_transport_t transport{};
    transport.flags = CLAP_TRANSPORT_HAS_TEMPO;
    transport.tempo = 120;
    clap_process_t process{};
    process.frames_count = 256;
    process.audio_inputs = &input;
    process.audio_inputs_count = 1;
    process.audio_outputs = &output;
    process.audio_outputs_count = 1;
    process.transport = &transport;
    const auto run = [&] {
        processing = true;
        const auto result = plugin->process(plugin, &process);
        processing = false;
        return result != CLAP_PROCESS_ERROR;
    };
    int fail = 0;
    const auto check = [&](bool ok, const char* text) {
        std::printf("%s: %s\n", text, ok ? "ok" : "FAIL");
        if (!ok) ++fail;
    };
    check(run() && tail->get(plugin) == 164400, "120 BPM dotted eighth tail");
    const int before = notifications;
    transport.tempo = 60;
    check(run() && tail->get(plugin) == 326400, "transport tempo updates tail");
    check(notifications == before + 1, "tail change notified in process");
    std::string errors;
    clap_ostream_t errorOut{&errors, write};
    const auto patch = [&](const char* text) {
        return semantic->set_patch(plugin, CLAP_SUPPLE_SET_MERGE, text,
                                   uint32_t(std::char_traits<char>::length(text)), &errorOut);
    };
    const int count = notifications;
    check(patch(R"({"params":{"freeze":true}})"), "main-thread freeze edit accepted");
    check(notifications == count, "main-thread edit defers tail notification");
    check(run() && tail->get(plugin) == uint32_t(INT32_MAX), "freeze publishes infinite tail");
    check(patch(R"({"params":{"freeze":false,"feedback":110}})") && run() &&
              tail->get(plugin) == uint32_t(INT32_MAX),
          "self-oscillation has infinite tail");
    check(patch(R"({"params":{"feedback":0,"sync":"off","time":100}})") && run() &&
              tail->get(plugin) == 7200,
          "free time and zero-feedback tail");
    check(patch(R"({"params":{"sync":"n1_8d"}})"), "sync restores");
    process.transport = nullptr;
    check(run() && tail->get(plugin) == 20400, "missing transport restores 120 BPM");
    check(!wrongThread, "all tail notifications follow the audio-thread contract");
    plugin->stop_processing(plugin);
    plugin->deactivate(plugin);
    plugin->destroy(plugin);
    entry->deinit();
    dlclose(library);
    return fail ? 1 : 0;
}
