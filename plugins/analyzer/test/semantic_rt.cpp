// SPDX-License-Identifier: MIT
#include <clap/clap.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include "analyzer/params.h"
#include "supple/clap/semantic_params.h"
#include "supple/json/json.h"
#include "supple/plugin/test_module.h"

namespace {
const void* CLAP_ABI extension(const clap_host_t*, const char*) { return nullptr; }
void CLAP_ABI request(const clap_host_t*) {}
int64_t CLAP_ABI write(const clap_ostream_t* o, const void* d, uint64_t n) {
    static_cast<std::string*>(o->ctx)->append(static_cast<const char*>(d), n);
    return static_cast<int64_t>(n);
}
bool CLAP_ABI accept(const clap_output_events_t*, const clap_event_header_t*) { return true; }
}  // namespace
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    void* lib = supple::plugin::test::open(argv[1]);
    if (!lib) {
        std::puts(supple::plugin::test::error());
        return 1;
    }
    const auto* entry =
        static_cast<const clap_plugin_entry_t*>(supple::plugin::test::symbol(lib, "clap_entry"));
    if (!entry || !entry->init(argv[1])) return 1;
    const auto* factory =
        static_cast<const clap_plugin_factory_t*>(entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    clap_host_t host{CLAP_VERSION, nullptr, "Analyzer RT test", "Supple Audio",
                     "",           "1",     extension,          request,
                     request,      request};
    const auto* p = factory->create_plugin(factory, &host, supple::analyzer::kPluginId);
    if (!p || !p->init(p)) return 1;
    const auto* sem = static_cast<const clap_plugin_supple_semantic_params_t*>(
        p->get_extension(p, CLAP_EXT_SUPPLE_SEMANTIC_PARAMS));
    if (!sem || sem->struct_size < sizeof(*sem) || !sem->get_telemetry) return 1;
    std::string text;
    clap_ostream_t out{&text, write};
    if (!sem->get_telemetry(p, &out)) return 1;
    auto idle = supple::json::parse(text);
    if (!idle || idle->find("schema")->asString() != "supple.analysis-report/1.0") return 1;
    if (!p->activate(p, 48000, 256, 256) || !p->start_processing(p)) return 1;
    std::atomic<bool> bad = false;
    std::atomic<uint32_t> blocks = 0;
    std::thread audio([&] {
        float l[256], r[256], a[256], b[256];
        float *input[]{l, r}, *output[]{a, b};
        clap_audio_buffer_t ib{}, ob{};
        ib.data32 = input;
        ob.data32 = output;
        ib.channel_count = ob.channel_count = 2;
        clap_output_events_t events{nullptr, accept};
        clap_process_t process{};
        process.frames_count = 256;
        process.audio_inputs = &ib;
        process.audio_inputs_count = 1;
        process.audio_outputs = &ob;
        process.audio_outputs_count = 1;
        process.out_events = &events;
        for (int block = 0; block < 1000; ++block) {
            for (int i = 0; i < 256; ++i)
                l[i] = r[i] = static_cast<float>(.1 * std::sin((block * 256 + i) * .13));
            if (p->process(p, &process) != CLAP_PROCESS_CONTINUE || std::memcmp(l, a, sizeof(l)) ||
                std::memcmp(r, b, sizeof(r)))
                bad = true;
            ++blocks;
        }
    });
    for (int i = 0; i < 100; ++i) {
        const std::string patch = "{\"params\":{\"window\":\"since_reset\",\"hold\":false}}";
        if (!sem->set_patch(p, CLAP_SUPPLE_SET_MERGE, patch.data(),
                            static_cast<uint32_t>(patch.size()), nullptr))
            bad = true;
        text.clear();
        if (!sem->get_telemetry(p, &out) || !supple::json::parse(text)) bad = true;
    }
    audio.join();
    p->stop_processing(p);
    p->deactivate(p);
    const auto* state =
        static_cast<const clap_plugin_state_t*>(p->get_extension(p, CLAP_EXT_STATE));
    std::string saved;
    clap_ostream_t save{&saved, write};
    if (!state->save(p, &save)) bad = true;
    size_t off = 0;
    std::pair<std::string*, size_t*> cursor{&saved, &off};
    clap_istream_t input{&cursor, [](const clap_istream_t* s, void* d, uint64_t n) -> int64_t {
                             auto& c = *static_cast<std::pair<std::string*, size_t*>*>(s->ctx);
                             const auto count = std::min<size_t>(n, c.first->size() - *c.second);
                             std::memcpy(d, c.first->data() + *c.second, count);
                             *c.second += count;
                             return static_cast<int64_t>(count);
                         }};
    if (!state->load(p, &input)) bad = true;
    saved.pop_back();
    off = 0;
    if (state->load(p, &input)) bad = true;
    p->destroy(p);
    entry->deinit();
    supple::plugin::test::close(lib);
    std::printf("semantic telemetry and patches during %u bit-exact audio blocks: %s\n",
                blocks.load(), bad ? "FAIL" : "ok");
    return bad ? 1 : 0;
}
