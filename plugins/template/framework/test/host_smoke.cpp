// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// A minimal host that loads the built CLAP from disk and drives it the way a
// DAW would: factory, create, ports, parameters, processing, the latency
// protocol, state into a fresh instance, teardown. Product-independent: what
// it needs to know arrives as compile definitions from product.cmake.
//
// The CLAP binary is what clap-wrapper turns into the VST3 and AU, so this
// exercises the code all three formats share -- not the wrappers themselves,
// which is what format validators and real hosts are for.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <clap/clap.h>
#include "host_contract.h"

// Loading a shared library is the one thing these hosts do that has no
// portable spelling. dlopen is POSIX; MSVC ships no <dlfcn.h> at all, so the
// three calls below are the whole of the difference.
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
namespace {
using LibHandle = HMODULE;
LibHandle libOpen(const char* p) { return LoadLibraryA(p); }
void* libSym(LibHandle h, const char* n) { return (void*)GetProcAddress(h, n); }
std::string libError() { return "error " + std::to_string((unsigned long)GetLastError()); }
}  // namespace
#else
#  include <dlfcn.h>
namespace {
using LibHandle = void*;
LibHandle libOpen(const char* p) { return dlopen(p, RTLD_LOCAL | RTLD_NOW); }
void* libSym(LibHandle h, const char* n) { return dlsym(h, n); }
std::string libError() { const char* e = dlerror(); return e ? e : "unknown error"; }
}  // namespace
#endif

namespace {

int g_fail = 0;

void check(bool ok, const char* what) {
    std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    if (!ok) ++g_fail;
}

// A stream backed by a vector, for the state round trip.
struct MemStream {
    std::vector<uint8_t> buf;
    size_t pos = 0;
};

int64_t CLAP_ABI memWrite(const clap_ostream* s, const void* p, uint64_t n) {
    auto* m = (MemStream*)s->ctx;
    const uint8_t* b = (const uint8_t*)p;
    m->buf.insert(m->buf.end(), b, b + n);
    return (int64_t)n;
}

int64_t CLAP_ABI memRead(const clap_istream* s, void* p, uint64_t n) {
    auto* m = (MemStream*)s->ctx;
    const size_t avail = m->buf.size() - m->pos;
    const size_t take = (size_t)n < avail ? (size_t)n : avail;
    std::memcpy(p, m->buf.data() + m->pos, take);
    m->pos += take;
    return (int64_t)take;
}

// A restart tears down and re-activates the plugin, resetting every filter, so
// a host that is asked to restart repeatedly produces a click every time. This
// counts the requests so the test can assert they only happen when latency
// genuinely moves.
int g_restarts = 0;
const clap_plugin_t* g_plug = nullptr;

// What the plugin said was wrong. This host keeps every rule, so a debug
// build checking each call's thread must find nothing to report.
std::vector<std::string> g_misbehaviour;
void CLAP_ABI hostLog(const clap_host*, clap_log_severity severity, const char* message) {
    if (severity == CLAP_LOG_HOST_MISBEHAVING || severity == CLAP_LOG_PLUGIN_MISBEHAVING)
        g_misbehaviour.push_back(message ? message : "");
}
const clap_host_log_t g_log = {hostLog};

const void* CLAP_ABI hostGetExtension(const clap_host*, const char* id) {
    return !std::strcmp(id, CLAP_EXT_LOG) ? &g_log : nullptr;
}
void CLAP_ABI hostRequestRestart(const clap_host*) { ++g_restarts; }
void CLAP_ABI hostRequestProcess(const clap_host*) {}

// Real hosts service this later, on the main thread; this one does after
// each block and flush (served()), never inside the call that asked, which
// may be process() -- where on_main_thread's work is not allowed. It is where
// the plugin makes its restart request.
bool g_callbackPending = false;
void CLAP_ABI hostRequestCallback(const clap_host*) { g_callbackPending = true; }
void serve() {
    if (g_callbackPending && g_plug) {
        g_callbackPending = false;
        g_plug->on_main_thread(g_plug);
    }
}
clap_process_status served(clap_process_status status) {
    serve();
    return status;
}

// An event list holding a single parameter change at frame 0.
struct ParamEvents {
    clap_event_param_value_t ev{};
    bool active = false;
};
ParamEvents g_events;

uint32_t CLAP_ABI evCount(const clap_input_events*) { return g_events.active ? 1u : 0u; }
const clap_event_header_t* CLAP_ABI evGet(const clap_input_events*, uint32_t i) {
    if (!g_events.active || i != 0) return nullptr;
    return &g_events.ev.header;
}
bool CLAP_ABI evPush(const clap_output_events*, const clap_event_header_t*) { return true; }

void setParamEvent(clap_id id, double value) {
    g_events.ev = {};
    g_events.ev.header.size = sizeof(clap_event_param_value_t);
    g_events.ev.header.time = 0;
    g_events.ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
    g_events.ev.header.type = CLAP_EVENT_PARAM_VALUE;
    g_events.ev.header.flags = 0;
    g_events.ev.param_id = id;
    g_events.ev.value = value;
    g_events.ev.note_id = -1;
    g_events.ev.port_index = -1;
    g_events.ev.channel = -1;
    g_events.ev.key = -1;
    g_events.active = true;
}

}  // namespace

// The plugin id this build is expected to advertise. Supplied by CMake so the
// test itself stays identical across products.
#ifndef HOST_SMOKE_PLUGIN_ID
#  error "HOST_SMOKE_PLUGIN_ID must be defined by the build"
#endif
// A parameter that must never change latency. Every product has one.
#ifndef HOST_SMOKE_PLAIN_PARAM
#  error "HOST_SMOKE_PLAIN_PARAM must be defined by the build"
#endif

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);  // a crash keeps every line before it
    if (argc < 2) {
        std::fprintf(stderr, "usage: host-smoke <Product.clap or its binary> [--layout 2|1|5.1|7.1|7.1.4|ambi1..3|N|none]\n");
        return 2;
    }

    // A .clap is a bundle directory on macOS and a bare shared object
    // everywhere else. When it is a bundle, the executable inside is named
    // after it, so the one argument is enough either way and no product name
    // has to be compiled in here.
    const std::string bundle = argv[1];
    std::string path = bundle;
    if (std::filesystem::is_directory(bundle)) {
        std::string name = bundle;
        if (const size_t slash = name.find_last_of('/'); slash != std::string::npos)
            name = name.substr(slash + 1);
        if (name.size() > 5 && name.compare(name.size() - 5, 5, ".clap") == 0)
            name.erase(name.size() - 5);
        path = bundle + "/Contents/MacOS/" + name;
    }

    // Select the requested layout before activation; the host buffers alone
    // must not be what changes width.
    const char* layoutName = "2";
    for (int i = 2; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--layout") || !std::strcmp(argv[i], "--channels")) layoutName = argv[i + 1];
    host_contract::LayoutSpec layout;
    if (!host_contract::parseLayout(layoutName, layout)) {
        std::fprintf(stderr, "host-smoke: '%s' is not a layout\n", layoutName);
        return 2;
    }
    const uint32_t channels = layout.channels;

    std::printf("clap host smoke test (layout %s, %u channels)\n", layoutName, channels);

    LibHandle lib = libOpen(path.c_str());
    if (!lib) {
        std::printf("  load failed: %s\n", libError().c_str());
        return 1;
    }
    check(true, "bundle loads");

    auto* entry = (const clap_plugin_entry_t*)libSym(lib, "clap_entry");
    check(entry != nullptr, "exports clap_entry");
    if (!entry) return 1;

    check(entry->init(path.c_str()), "entry init");

    auto* fac = (const clap_plugin_factory_t*)entry->get_factory(CLAP_PLUGIN_FACTORY_ID);
    check(fac != nullptr, "plugin factory available");
    if (!fac) return 1;

    check(fac->get_plugin_count(fac) == 1, "factory advertises one plugin");
    const clap_plugin_descriptor_t* desc = fac->get_plugin_descriptor(fac, 0);
    check(desc && !std::strcmp(desc->id, HOST_SMOKE_PLUGIN_ID),
          "descriptor id is " HOST_SMOKE_PLUGIN_ID);

    clap_host_t host{};
    host.clap_version = CLAP_VERSION;
    host.name = "smoke";
    host.vendor = "";
    host.url = "";
    host.version = "1.0";
    host.get_extension = hostGetExtension;
    host.request_restart = hostRequestRestart;
    host.request_process = hostRequestProcess;
    host.request_callback = hostRequestCallback;

    const clap_plugin_t* plug = fac->create_plugin(fac, &host, desc->id);
    check(plug != nullptr, "plugin instantiates");
    if (!plug) return 1;
    g_plug = plug;

    check(plug->init(plug), "plugin init");

    check(host_contract::selectLayout(plug, channels), "negotiate channel layout before activation");

    auto* params = (const clap_plugin_params_t*)plug->get_extension(plug, CLAP_EXT_PARAMS);
    auto* ports = (const clap_plugin_audio_ports_t*)plug->get_extension(plug,
                                                                       CLAP_EXT_AUDIO_PORTS);
    auto* latency = (const clap_plugin_latency_t*)plug->get_extension(plug, CLAP_EXT_LATENCY);
    auto* state = (const clap_plugin_state_t*)plug->get_extension(plug, CLAP_EXT_STATE);
    check(params && ports && latency && state, "advertises params/ports/latency/state");

    // An effect declares a main audio input. That one fact is enough to know
    // which of the checks below apply, so no product has to configure it: an
    // effect must pass signal through, while an instrument sitting with no
    // notes is correct to be silent, and a note effect has no audio at all.
    const bool isEffect = host_contract::hasMainInput(plug);
    const auto* notePorts = (const clap_plugin_note_ports_t*)plug->get_extension(plug, CLAP_EXT_NOTE_PORTS);
    const bool takesNotes = notePorts && notePorts->count(plug, true) > 0;
    std::printf("     audio ports: %u in, %u out (%s)%s\n", ports->count(plug, true), ports->count(plug, false),
                isEffect ? "effect" : channels ? "instrument" : "note effect", takesNotes ? ", notes in" : "");

    const uint32_t np = params->count(plug);
    check(np > 0, "exposes at least one parameter");
    std::printf("     parameters: %u\n", np);

    bool namesOk = true, textOk = true;
    for (uint32_t i = 0; i < np; ++i) {
        clap_param_info_t info{};
        if (!params->get_info(plug, i, &info)) namesOk = false;
        if (!info.name[0]) namesOk = false;
        char buf[128];
        if (!params->value_to_text(plug, info.id, info.default_value, buf, sizeof(buf)))
            textOk = false;
    }
    check(namesOk, "every parameter reports info and a name");
    check(textOk, "every parameter formats its default value");

    host_contract::ports(plug, layout, check);
    host_contract::parameterText(plug, check);
    host_contract::processing(fac, desc->id, HOST_SMOKE_PLAIN_PARAM, channels,
                              48000.0, isEffect, takesNotes, check);

    const uint32_t block = 512;
    check(plug->activate(plug, 48000.0, 32, block), "activate at 48 kHz");
    check(plug->start_processing(plug), "start processing");

    const uint32_t lat = latency->get(plug);
    check(lat < 65536, "reports a sane latency");
    std::printf("     reported latency: %u samples\n", lat);

    // Every port the plugin declared, the main buses in place, as most hosts
    // run an effect. silence() clears them all between uses below.
    host_contract::Buses<float> buses;
    buses.build(plug, block, true);
    auto quiet = [&] { buses.silence(block); };

    clap_input_events_t inEv{nullptr, evCount, evGet};
    clap_output_events_t outEv{nullptr, evPush};

    clap_process_t proc{};
    proc.frames_count = block;
    buses.bind(proc);
    proc.in_events = &inEv;
    proc.out_events = &outEv;

    bool finite = true;
    double peak = 0.0;
    for (int b = 0; b < 200; ++b) {
        quiet();
        if (float* const* in = buses.mainInput())
            for (uint32_t c = 0; c < channels; ++c)
                for (uint32_t i = 0; i < block; ++i)
                    in[c][i] = (float)(0.3 * std::sin(2.0 * 3.14159265358979323846 * 220.0 * (double)(b * block + i) / 48000.0));
        if (served(plug->process(plug, &proc)) == CLAP_PROCESS_ERROR) finite = false;
        buses.forEachOutput([&](float* c) {
            for (uint32_t i = 0; i < block; ++i) {
                if (!std::isfinite(c[i])) finite = false;
            }
        });
        if (float* const* out = buses.mainOutput())
            for (uint32_t i = 0; i < block; ++i) peak = std::max(peak, (double)std::fabs(out[0][i]));
    }
    check(finite, "processes 100k frames without NaN or error");
    check(peak < 2.0, "output stays bounded");
    if (isEffect) check(peak > 0.05, "effect passes signal through");
    std::printf("     output peak: %.4f\n", peak);

    // --- restart storm regression ---
    //
    // The symptom this guards against: constant popping in REAPER on the CLAP
    // and VST3 builds while the AU was clean. The plugin was reporting a
    // latency change on every setParams(), so every parameter touch — and every
    // routine parameter flush the host makes on its own — asked for a restart,
    // and each restart reset the whole filter chain. The AU wrapper handles
    // latency via a property notification and ignores request_restart, which is
    // exactly why it alone sounded fine.
    auto paramIdByName = [&](const char* want) -> clap_id {
        for (uint32_t i = 0; i < np; ++i) {
            clap_param_info_t info{};
            if (params->get_info(plug, i, &info) && !std::strcmp(info.name, want))
                return info.id;
        }
        return CLAP_INVALID_ID;
    };

    // Which parameter plays which role is the one product-specific thing this
    // test needs, so the build supplies the names. A product with no
    // latency-changing parameter simply leaves the second one undefined and
    // the restart section below is skipped rather than faked.
    const clap_id idPlain = paramIdByName(HOST_SMOKE_PLAIN_PARAM);
    check(idPlain != CLAP_INVALID_ID, "found the " HOST_SMOKE_PLAIN_PARAM " parameter");

    // Idle host behaviour: repeated flushes with nothing changing.
    g_restarts = 0;
    clap_input_events_t emptyEv{nullptr, [](const clap_input_events*) -> uint32_t { return 0; },
                                [](const clap_input_events*, uint32_t)
                                    -> const clap_event_header_t* { return nullptr; }};
    for (int i = 0; i < 50; ++i) {
        params->flush(plug, &emptyEv, &outEv);
        serve();
    }
    check(g_restarts == 0, "50 idle parameter flushes request no restart");

    // Ordinary automation of a parameter that cannot change latency. Swept
    // across the parameter's own range, whatever that is.
    clap_param_info_t plainInfo{};
    for (uint32_t i = 0; i < np; ++i) {
        clap_param_info_t info{};
        if (params->get_info(plug, i, &info) && info.id == idPlain) plainInfo = info;
    }
    g_restarts = 0;
    for (int i = 0; i < 50; ++i) {
        const double t = (double)i / 49.0;
        setParamEvent(idPlain, plainInfo.min_value +
                                   t * (plainInfo.max_value - plainInfo.min_value));
        quiet();
        served(plug->process(plug, &proc));
    }
    check(g_restarts == 0,
          "50 blocks of " HOST_SMOKE_PLAIN_PARAM " automation request no restart");

#ifdef HOST_SMOKE_LATENCY_PARAM
    // The other half: a parameter that genuinely moves latency. CLAP allows
    // latency to change only across activation, so the plugin must keep
    // reporting the old value, ask for a restart (once), and report the new
    // value after the host deactivates and reactivates it.
    const clap_id idLat = paramIdByName(HOST_SMOKE_LATENCY_PARAM);
    check(idLat != CLAP_INVALID_ID, "found the " HOST_SMOKE_LATENCY_PARAM " parameter");
    clap_param_info_t latInfo{};
    for (uint32_t i = 0; i < np; ++i) {
        clap_param_info_t info{};
        if (params->get_info(plug, i, &info) && info.id == idLat) latInfo = info;
    }
    auto applyAndProcess = [&](clap_id id, double v) {
        setParamEvent(id, v);
        quiet();
        served(plug->process(plug, &proc));
        g_events.active = false;
    };
    auto restart = [&] {
        plug->stop_processing(plug);
        plug->deactivate(plug);
        plug->activate(plug, 48000.0, 32, block);
        plug->start_processing(plug);
    };
    // Search the range for a setting that moves latency rather than assuming
    // which one does: a product may change it at one end only.
    double latNow = 0.0;
    params->get_value(plug, idLat, &latNow);
    const uint32_t latBase = latency->get(plug);
    bool moved = false;
    for (double v = latInfo.min_value; v <= latInfo.max_value + 1e-9 && !moved;
         v += (latInfo.flags & CLAP_PARAM_IS_STEPPED) ? 1.0 : (latInfo.max_value - latInfo.min_value) / 8.0) {
        if (v == latNow) continue;
        g_restarts = 0;
        applyAndProcess(idLat, v);
        check(latency->get(plug) == latBase, "latency holds until the host restarts");
        if (g_restarts == 0) continue;
        applyAndProcess(idLat, v);
        check(g_restarts == 1, "and a second block does not ask again");
        restart();
        moved = latency->get(plug) != latBase;
        check(moved, "after the restart the new latency is reported");
    }
    check(moved, HOST_SMOKE_LATENCY_PARAM " has a setting that changes latency");
    applyAndProcess(idLat, latNow);
    restart();
    g_restarts = 0;
    for (int i = 0; i < 50; ++i) {
        quiet();
        served(plug->process(plug, &proc));
    }
    check(g_restarts == 0, "a settled latency asks for no restart");
#endif

    g_events.active = false;

    // State, into a fresh instance: every value set here must come back.
    std::vector<std::pair<clap_id, double>> expected;
    for (uint32_t i = 0; i < np; ++i) {
        clap_param_info_t info{};
        if (!params->get_info(plug, i, &info) || (info.flags & CLAP_PARAM_IS_READONLY)) continue;
        double v = info.min_value + 0.7 * (info.max_value - info.min_value);
        if (info.flags & CLAP_PARAM_IS_STEPPED) v = std::round(v);
        setParamEvent(info.id, v);
        served(plug->process(plug, &proc));
        double now = 0.0;
        params->get_value(plug, info.id, &now);
        expected.push_back({info.id, now});
    }
    g_events.active = false;
    MemStream ms;
    clap_ostream_t os{&ms, memWrite};
    check(state->save(plug, &os), "state saves");
    check(ms.buf.size() > 16, "state is non-trivial");
    {
        const clap_plugin_t* fresh = fac->create_plugin(fac, &host, desc->id);
        fresh->init(fresh);
        auto* freshParams = (const clap_plugin_params_t*)fresh->get_extension(fresh, CLAP_EXT_PARAMS);
        auto* freshState = (const clap_plugin_state_t*)fresh->get_extension(fresh, CLAP_EXT_STATE);
        clap_istream_t is{&ms, memRead};
        check(freshState->load(fresh, &is), "state loads into a fresh instance");
        bool same = true;
        for (const auto& [id, v] : expected) {
            double got = 0.0;
            same = same && freshParams->get_value(fresh, id, &got) && got == v;
        }
        check(same, "and every parameter value is restored");
        MemStream again;
        clap_ostream_t os2{&again, memWrite};
        freshState->save(fresh, &os2);
        check(again.buf == ms.buf, "and saving it again is byte-identical");
        fresh->destroy(fresh);
    }

    // --- reset ---
    //
    // CLAP's reset() promises that what follows depends only on the state. A
    // host that reuses one instance across renders (an offline bounce, a
    // render farm, an agent's sandbox) relies on it: load a state, reset,
    // render, and expect exactly what a fresh instance with that state
    // renders. Bypass is held off so the DSP itself is what is compared.
    {
        auto setAll = [&](double position) {
            for (uint32_t i = 0; i < np; ++i) {
                clap_param_info_t info{};
                if (!params->get_info(plug, i, &info) || (info.flags & CLAP_PARAM_IS_READONLY)) continue;
                double v = info.min_value + position * (info.max_value - info.min_value);
                if (info.flags & CLAP_PARAM_IS_STEPPED) v = std::round(v);
                if (info.flags & CLAP_PARAM_IS_BYPASS) v = info.min_value;
                setParamEvent(info.id, v);
                quiet();
                served(plug->process(plug, &proc));
            }
            g_events.active = false;
        };
        uint32_t seed = 12345;
        auto noise = [&] {
            seed = seed * 1664525u + 1013904223u;
            return (float)((int32_t)seed) * (0.25f / 2147483648.0f);
        };
        // Noise into every input (the sidechain too); every output kept.
        auto feed = [&] {
            quiet();
            if (float* const* in = buses.mainInput())
                for (uint32_t c = 0; c < channels; ++c)
                    for (uint32_t k = 0; k < block; ++k) in[c][k] = noise();
            buses.forEachSidechain([&](float* c) {
                for (uint32_t k = 0; k < block; ++k) c[k] = noise();
            });
        };
        auto render = [&](const clap_plugin_t* p, int blocks) {
            seed = 777;
            std::vector<float> out;
            clap_process_t pr = proc;
            for (int b = 0; b < blocks; ++b) {
                feed();
                p->process(p, &pr);
                buses.forEachOutput([&](float* c) { out.insert(out.end(), c, c + block); });
            }
            return out;
        };

        g_restarts = 0;
        setAll(0.35);
        MemStream wanted;
        clap_ostream_t ow{&wanted, memWrite};
        state->save(plug, &ow);
        setAll(0.8);
        for (int b = 0; b < 40; ++b) {
            feed();
            served(plug->process(plug, &proc));
        }
        clap_istream_t iw{&wanted, memRead};
        check(state->load(plug, &iw), "a state loads into the active instance");
        // A restart parameter moved (a latency, an oversampling factor): a
        // host restarts the plugin when asked, so do that first.
        if (g_restarts > 0) {
            plug->stop_processing(plug);
            plug->deactivate(plug);
            plug->activate(plug, 48000.0, 32, block);
            plug->start_processing(plug);
            g_restarts = 0;
        }
        plug->reset(plug);
        const std::vector<float> reused = render(plug, 60);

        const clap_plugin_t* fresh = fac->create_plugin(fac, &host, desc->id);
        fresh->init(fresh);
        host_contract::selectLayout(fresh, channels);
        auto* freshState = (const clap_plugin_state_t*)fresh->get_extension(fresh, CLAP_EXT_STATE);
        wanted.pos = 0;
        clap_istream_t iw2{&wanted, memRead};
        freshState->load(fresh, &iw2);
        fresh->activate(fresh, 48000.0, 32, block);
        fresh->start_processing(fresh);
        const std::vector<float> clean = render(fresh, 60);
        fresh->stop_processing(fresh);
        fresh->deactivate(fresh);
        fresh->destroy(fresh);
        size_t first = reused.size();
        for (size_t k = 0; k < reused.size() && k < clean.size(); ++k)
            if (reused[k] != clean[k]) {
                first = k;
                break;
            }
        if (first < reused.size())
            std::printf("     first difference at sample %zu: %g vs %g\n", first, reused[first], clean[first]);
        check(reused.size() == clean.size() && first == reused.size(),
              "after state + reset(), output equals a fresh instance's");
    }

    // --- what the host says about the session ---
    if (auto* render = (const clap_plugin_render_t*)plug->get_extension(plug, CLAP_EXT_RENDER)) {
        const bool offline = render->set(plug, CLAP_RENDER_OFFLINE);
        bool rendered = true;
        for (int b = 0; b < 4; ++b) {
            quiet();
            rendered = rendered && served(plug->process(plug, &proc)) != CLAP_PROCESS_ERROR;
            buses.forEachOutput([&](float* c) {
                for (uint32_t k = 0; k < block; ++k) rendered = rendered && std::isfinite(c[k]);
            });
        }
        check(offline && rendered && render->set(plug, CLAP_RENDER_REALTIME), "render: offline, then real time");
    }
    if (auto* menu = (const clap_plugin_context_menu_t*)plug->get_extension(plug, CLAP_EXT_CONTEXT_MENU)) {
        int items = 0;
        clap_context_menu_builder_t builder{&items,
                                            [](const clap_context_menu_builder_t* b, clap_context_menu_item_kind_t,
                                               const void*) {
                                                ++*static_cast<int*>(b->ctx);
                                                return true;
                                            },
                                            [](const clap_context_menu_builder_t*, clap_context_menu_item_kind_t) {
                                                return true;
                                            }};
        clap_param_info_t info{};
        params->get_info(plug, 0, &info);
        const clap_context_menu_target_t target{CLAP_CONTEXT_MENU_TARGET_KIND_PARAM, info.id};
        check(menu->populate(plug, &target, &builder) && items > 0, "context menu: items for a parameter");
    }
    if (auto* names = (const clap_plugin_note_name_t*)plug->get_extension(plug, CLAP_EXT_NOTE_NAME)) {
        const uint32_t count = names->count(plug);
        bool valid = count > 0;
        for (uint32_t k = 0; k < count; ++k) {
            clap_note_name_t n{};
            valid = valid && names->get(plug, k, &n) && n.name[0] && n.key >= -1 && n.key <= 127;
        }
        std::printf("     %u note names\n", count);
        check(valid, "note names: each with a name and a key");
    }

    // --- presets, as a host's browser finds and loads them ---
    check(plug->get_extension(plug, CLAP_EXT_STATE_CONTEXT) != nullptr, "advertises state-context");
    if (auto* discovery = (const clap_preset_discovery_factory_t*)entry->get_factory(CLAP_PRESET_DISCOVERY_FACTORY_ID)) {
        auto* load = (const clap_plugin_preset_load_t*)plug->get_extension(plug, CLAP_EXT_PRESET_LOAD);
        check(load != nullptr && discovery->count(discovery) == 1, "presets: one provider, and preset-load");
        struct Found {
            std::vector<std::string> keys;
            bool plugin = false;
        } found;
        clap_preset_discovery_indexer_t indexer{};
        indexer.clap_version = CLAP_VERSION;
        indexer.name = "host-smoke";
        indexer.indexer_data = &found;
        indexer.declare_filetype = [](const clap_preset_discovery_indexer_t*, const clap_preset_discovery_filetype_t*) {
            return true;
        };
        indexer.declare_location = [](const clap_preset_discovery_indexer_t* x, const clap_preset_discovery_location_t* l) {
            if (l->kind == CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN) static_cast<Found*>(x->indexer_data)->plugin = true;
            return true;
        };
        indexer.declare_soundpack = [](const clap_preset_discovery_indexer_t*, const clap_preset_discovery_soundpack_t*) {
            return true;
        };
        indexer.get_extension = [](const clap_preset_discovery_indexer_t*, const char*) -> const void* { return nullptr; };
        const auto* provider = discovery->create(discovery, &indexer, discovery->get_descriptor(discovery, 0)->id);
        check(provider && provider->init(provider), "presets: the provider initialises");
        clap_preset_discovery_metadata_receiver_t receiver{};
        receiver.receiver_data = &found;
        receiver.on_error = [](const clap_preset_discovery_metadata_receiver_t*, int32_t, const char*) {};
        receiver.begin_preset = [](const clap_preset_discovery_metadata_receiver_t* r, const char*, const char* key) {
            static_cast<Found*>(r->receiver_data)->keys.push_back(key ? key : "");
            return true;
        };
        receiver.add_plugin_id = [](const clap_preset_discovery_metadata_receiver_t*, const clap_universal_plugin_id_t*) {};
        receiver.set_soundpack_id = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        receiver.set_flags = [](const clap_preset_discovery_metadata_receiver_t*, uint32_t) {};
        receiver.add_creator = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        receiver.set_description = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        receiver.set_timestamps = [](const clap_preset_discovery_metadata_receiver_t*, clap_timestamp, clap_timestamp) {};
        receiver.add_feature = [](const clap_preset_discovery_metadata_receiver_t*, const char*) {};
        receiver.add_extra_info = [](const clap_preset_discovery_metadata_receiver_t*, const char*, const char*) {};
        if (provider && found.plugin) {
            check(provider->get_metadata(provider, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, &receiver),
                  "presets: the factory presets are listed");
            // Each one loaded while processing, as a host's browser does.
            bool loaded = true, stillFinite = true;
            for (const std::string& key : found.keys) {
                loaded = loaded && load && load->from_location(plug, CLAP_PRESET_DISCOVERY_LOCATION_PLUGIN, nullptr, key.c_str());
                for (int b = 0; b < 4; ++b) {
                    quiet();
                    stillFinite = stillFinite && served(plug->process(plug, &proc)) != CLAP_PROCESS_ERROR;
                    buses.forEachOutput([&](float* c) {
                        for (uint32_t k = 0; k < block; ++k) stillFinite = stillFinite && std::isfinite(c[k]);
                    });
                }
            }
            std::printf("     %zu factory presets\n", found.keys.size());
            check(loaded && !found.keys.empty(), "presets: every factory preset loads by its key");
            check(stillFinite, "presets: and processes cleanly after each");
        }
        if (provider) provider->destroy(provider);
    }

    plug->stop_processing(plug);
    plug->deactivate(plug);
    plug->destroy(plug);
    entry->deinit();
    check(true, "clean shutdown");
    for (const std::string& m : g_misbehaviour) std::printf("     the plugin logged: %s\n", m.c_str());
    check(g_misbehaviour.empty(), "the plugin found nothing wrong with this host");

    std::printf("\n%s\n", g_fail == 0 ? "all checks passed" : "FAILURES PRESENT");
    return g_fail == 0 ? 0 : 1;
}
