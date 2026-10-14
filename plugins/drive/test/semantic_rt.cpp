// SPDX-License-Identifier: MIT
#include <clap/clap.h>
#include "supple/plugin/test_module.h"
#include <array>
#include <atomic>
#include <cstring>
#include <string>
#include <thread>
#include "supple/base/rt.h"
#include "supple/clap/semantic_params.h"
static const void* CLAP_ABI extension(const clap_host_t*, const char*) { return nullptr; }
static void CLAP_ABI callback(const clap_host_t*) {}
static int64_t CLAP_ABI write(const clap_ostream_t* o, const void* d, uint64_t n) {
    static_cast<std::string*>(o->ctx)->append(static_cast<const char*>(d), n);
    return static_cast<int64_t>(n);
}
static int64_t CLAP_ABI read(const clap_istream_t* i, void* d, uint64_t n) {
    auto* s = static_cast<std::string*>(i->ctx);
    auto m = std::min(n, static_cast<uint64_t>(s->size()));
    std::memcpy(d, s->data(), m);
    s->erase(0, m);
    return static_cast<int64_t>(m);
}
static void process(const clap_plugin_t* p, const clap_process_t* c) SUPPLE_RT;
static void process(const clap_plugin_t* p, const clap_process_t* c) { p->process(p, c); }
int main(int argc, char** argv) {
    if (argc != 2) return 1;
    void* lib = supple::plugin::test::open(argv[1]);
    if (!lib) return 2;
    auto* entry = static_cast<const clap_plugin_entry_t*>(supple::plugin::test::symbol(lib, "clap_entry"));
    if (!entry || !entry->init(argv[1])) return 3;
    auto* f = static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    clap_host_t host{CLAP_VERSION_INIT, nullptr,  "Test",   "Supple Audio", "", "1",
                     extension,         callback, callback, callback};
    auto* p = f->create_plugin(f, &host, "com.suppleaudio.drive");
    if (!p || !p->init(p)) return 4;
    auto* semantic = static_cast<const clap_plugin_supple_semantic_params_t*>(
        p->get_extension(p, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    auto* state = static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
    if (!semantic || !state) return 5;
    const char* patch = "{\"params\":{\"drive\":24,\"output\":-3}}";
    std::string errors;
    clap_ostream_t out{&errors, write};
    if (!semantic->set_patch(p, CLAP_SUPPLE_SET_MERGE, patch,
                             static_cast<uint32_t>(std::strlen(patch)), &out))
        return 6;
    std::string saved;
    clap_ostream_t save{&saved, write};
    if (!state->save(p, &save)) return 7;
    std::string bad = "corrupt";
    clap_istream_t badIn{&bad, read};
    if (state->load(p, &badIn)) return 8;
    clap_istream_t savedIn{&saved, read};
    if (!state->load(p, &savedIn)) return 9;
    if (!p->activate(p, 48000, 1, 64) || !p->start_processing(p)) return 10;
    std::array<float, 64> l{}, r{};
    l.fill(.2f);
    r.fill(.1f);
    float* ptrs[] = {l.data(), r.data()};
    clap_audio_buffer_t buf{};
    buf.data32 = ptrs;
    buf.channel_count = 2;
    clap_process_t c{};
    c.frames_count = 64;
    c.audio_inputs = &buf;
    c.audio_outputs = &buf;
    c.audio_inputs_count = c.audio_outputs_count = 1;
    std::atomic<bool> ready{false}, done{false};
    std::thread audio([&] {
        ready.store(true, std::memory_order_release);
        do {
            l.fill(.2f);
            r.fill(.1f);
            process(p, &c);
        } while (!done.load(std::memory_order_acquire));
    });
    while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
    bool ok = true;
    for (int i = 0; i < 1000; ++i) {
        const char* text = i % 2 ? patch : "{\"params\":{\"drive\":12,\"output\":-6}}";
        ok = semantic->set_patch(p, CLAP_SUPPLE_SET_MERGE, text,
                                 static_cast<uint32_t>(std::strlen(text)), &out) &&
             ok;
    }
    done.store(true, std::memory_order_release);
    audio.join();
    p->stop_processing(p);
    p->deactivate(p);
    p->destroy(p);
    entry->deinit();
    supple::plugin::test::close(lib);
    return ok ? 0 : 11;
}
