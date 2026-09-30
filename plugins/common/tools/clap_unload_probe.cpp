// SPDX-License-Identifier: MIT
// Exercise each factory descriptor, unload, reload, then exit with the image
// mapped. No editor, audio device or host extensions are needed.
#include <clap/clap.h>
#include <dlfcn.h>

#include <array>
#include <cmath>
#include <cstdio>
#include <exception>
#include <vector>

namespace supple {
namespace {

constexpr uint32_t kFrames = 256;

bool fail(const char* what) {
    std::fprintf(stderr, "clap-unload: %s\n", what);
    return false;
}

struct Instance {
    const clap_plugin_t* plugin{};
    bool active{}, processing{};
    ~Instance() {
        if (!plugin) return;
        if (processing) plugin->stop_processing(plugin);
        if (active) plugin->deactivate(plugin);
        plugin->destroy(plugin);
    }
};

struct Ports {
    struct Storage {
        std::vector<std::array<float, kFrames>> samples;
        std::vector<float*> channels;
    };
    std::vector<Storage> storage;
    std::vector<clap_audio_buffer_t> buffers;

    bool prepare(const clap_plugin_t* plugin, const clap_plugin_audio_ports_t* ports, bool input) {
        const uint32_t count = ports ? ports->count(plugin, input) : 0;
        if (count > 32) return fail("too many audio ports");
        storage.resize(count);
        buffers.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            clap_audio_port_info_t info{};
            if (!ports->get(plugin, i, input, &info) || info.channel_count == 0 ||
                info.channel_count > 64)
                return fail("invalid audio port");
            auto& data = storage[i];
            data.samples.resize(info.channel_count);
            for (auto& channel : data.samples) data.channels.push_back(channel.data());
            buffers[i].data32 = data.channels.data();
            buffers[i].channel_count = info.channel_count;
        }
        return true;
    }

    bool finite() const {
        bool valid = true;
        for (const auto& port : storage)
            for (const auto& channel : port.samples)
                for (float sample : channel) valid &= std::isfinite(sample);
        return valid;
    }
};

bool exercise(const clap_plugin_factory_t* factory, const clap_host_t& host,
              const clap_plugin_descriptor_t* descriptor) {
    if (!descriptor || !descriptor->id || !descriptor->name) return fail("invalid descriptor");
    Instance instance{factory->create_plugin(factory, &host, descriptor->id)};
    const auto* plugin = instance.plugin;
    if (!plugin) return fail("create_plugin failed");
    if (!plugin->init(plugin)) return fail("plugin init failed");
    const auto* ports = static_cast<const clap_plugin_audio_ports_t*>(
        plugin->get_extension(plugin, CLAP_EXT_AUDIO_PORTS));
    Ports inputs, outputs;
    if (!inputs.prepare(plugin, ports, true) || !outputs.prepare(plugin, ports, false))
        return false;
    if (!plugin->activate(plugin, 48000, 1, 512)) return fail("activate failed");
    instance.active = true;
    if (!plugin->start_processing(plugin)) return fail("start_processing failed");
    instance.processing = true;
    const clap_input_events_t inEvents{
        .size = [](const clap_input_events_t*) -> uint32_t { return 0; },
        .get = [](const clap_input_events_t*, uint32_t) -> const clap_event_header_t* {
            return nullptr;
        }};
    const clap_output_events_t outEvents{
        .try_push = [](const clap_output_events_t*, const clap_event_header_t*) { return true; }};
    clap_process_t process{.frames_count = kFrames,
                           .audio_inputs = inputs.buffers.data(),
                           .audio_outputs = outputs.buffers.data(),
                           .audio_inputs_count = static_cast<uint32_t>(inputs.buffers.size()),
                           .audio_outputs_count = static_cast<uint32_t>(outputs.buffers.size()),
                           .in_events = &inEvents,
                           .out_events = &outEvents};
    for (int block = 0; block < 8; ++block) {
        if (plugin->process(plugin, &process) == CLAP_PROCESS_ERROR) return fail("process failed");
        if (!outputs.finite()) return fail("non-finite output");
        process.steady_time += kFrames;
    }
    std::printf("clap-unload: %s: eight silence blocks processed\n", descriptor->name);
    return true;
}

bool cycle(const char* path, bool keep) {
    void* module = dlopen(path, RTLD_LOCAL | RTLD_NOW);
    if (!module) return fail(dlerror());
    const auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(module, "clap_entry"));
    if (!entry || !clap_version_is_compatible(entry->clap_version)) {
        dlclose(module);
        return fail("missing or incompatible clap_entry");
    }
    if (!entry->init(path)) {
        dlclose(module);
        return fail("entry init failed");
    }
    const auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    const clap_host_t host{
        .clap_version = CLAP_VERSION,
        .name = "Supple Audio unload probe",
        .vendor = "Supple Audio",
        .url = "https://supple.audio",
        .version = "1",
        .get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; },
        .request_restart = [](const clap_host_t*) {},
        .request_process = [](const clap_host_t*) {},
        .request_callback = [](const clap_host_t*) {}};
    bool ok = factory && factory->get_plugin_count(factory) > 0;
    if (!ok) fail("no plugin factory descriptors");
    if (factory) {
        for (uint32_t i = 0; ok && i < factory->get_plugin_count(factory); ++i)
            ok = exercise(factory, host, factory->get_plugin_descriptor(factory, i));
    }
    entry->deinit();
    if (!ok || !keep) {
        if (dlclose(module) != 0) return fail("dlclose failed");
    }
    if (!ok) return false;
    if (keep) {
        std::puts("clap-unload: pass (reload exercised; image kept mapped for process exit)");
        return true;
    }
    // NOLOAD acquires a reference if the module is still mapped; release that
    // diagnostic reference too, without hiding the failure behind NODELETE.
    if (void* pinned = dlopen(path, RTLD_LOCAL | RTLD_NOW | RTLD_NOLOAD)) {
        dlclose(pinned);
        return fail("pinned: module is still mapped after dlclose");
    }
    std::puts("clap-unload: module unloaded");
    return true;
}

}  // namespace

int unloadProbe(int argc, char** argv) {
    if (argc != 2) {
        fail("usage: clap-unload-probe <absolute .clap path>");
        return 1;
    }
    try {
        return cycle(argv[1], false) && cycle(argv[1], true) ? 0 : 1;
    } catch (const std::exception& error) {
        fail(error.what());
        return 1;
    }
}
}  // namespace supple

int main(int argc, char** argv) { return supple::unloadProbe(argc, argv); }
