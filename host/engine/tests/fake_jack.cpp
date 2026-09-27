// A stand-in libjack for the JACK backend tests: no server, no device. The
// process callback runs on a thread of its own, free-running at a period the
// test sets; the test drives server events (period and rate changes, xruns,
// stalls, shutdown) through the fakejack_* functions.
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

using Frames = std::uint32_t;
constexpr Frames kMaxPeriod = 8192;

struct Port {
    std::string name;
    bool output = false;
    std::vector<float> buffer = std::vector<float>(kMaxPeriod, 0.0f);
    std::atomic<float> peak{0.0f};
};

struct Client {
    std::string name;
    int (*process)(Frames, void*) = nullptr;
    void* processArg = nullptr;
    int (*xrun)(void*) = nullptr;
    void* xrunArg = nullptr;
    int (*bufferSize)(Frames, void*) = nullptr;
    void* bufferSizeArg = nullptr;
    int (*sampleRate)(Frames, void*) = nullptr;
    void* sampleRateArg = nullptr;
    void (*shutdown)(void*) = nullptr;
    void* shutdownArg = nullptr;
    std::vector<Port*> ports;
    std::thread thread;
    std::atomic<bool> active{false};
};

// The one client a test has open, and the server's state.
std::mutex g_mutex;
Client* g_client = nullptr;
std::atomic<Frames> g_period{1024}, g_appliedPeriod{1024};
std::atomic<Frames> g_rate{48000};
std::atomic<bool> g_stalled{false}, g_stallAck{false}, g_failOpen{false};
std::atomic<std::uint64_t> g_cycles{0};
std::atomic<Frames> g_bufferSizeRequest{0};
std::atomic<int> g_connections{0};
std::atomic<float> g_inputValue{0.0f};
std::string g_latencyEnv;

void run(Client* c) {
    while (c->active.load()) {
        if (g_stalled.load()) {
            g_stallAck = true;
            std::this_thread::sleep_for(std::chrono::microseconds(200));
            continue;
        }
        g_stallAck = false;
        const Frames n = g_period.load();
        if (n != g_appliedPeriod.load()) {
            g_appliedPeriod = n;
            if (c->bufferSize) c->bufferSize(n, c->bufferSizeArg);
        }
        for (Port* p : c->ports)
            if (!p->output) std::fill_n(p->buffer.begin(), n, g_inputValue.load());
        c->process(n, c->processArg);
        for (Port* p : c->ports) {
            if (!p->output) continue;
            float peak = p->peak.load();
            for (Frames i = 0; i < n; ++i) peak = std::max(peak, std::fabs(p->buffer[i]));
            p->peak = peak;
        }
        g_cycles.fetch_add(1);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
}

void stopThread(Client* c) {
    c->active = false;
    if (c->thread.joinable()) c->thread.join();
}

Port* findPort(const char* name) {
    if (!g_client) return nullptr;
    for (Port* p : g_client->ports)
        if (p->name == name) return p;
    return nullptr;
}

}  // namespace

extern "C" {

// --- the JACK API subset the backend binds -------------------------------------------

Client* jack_client_open(const char* name, int, int* status, ...) {
    std::lock_guard lock(g_mutex);
    if (g_failOpen.load() || g_client) {
        if (status) *status = 0x11;  // failure | server failed
        return nullptr;
    }
    const char* env = std::getenv("PIPEWIRE_LATENCY");
    g_latencyEnv = env ? env : "";
    g_appliedPeriod = g_period.load();
    g_cycles = 0;
    g_connections = 0;
    g_bufferSizeRequest = 0;
    g_client = new Client;
    g_client->name = name;
    if (status) *status = 0;
    return g_client;
}

int jack_deactivate(Client* c) {
    stopThread(c);
    return 0;
}

int jack_client_close(Client* c) {
    stopThread(c);
    std::lock_guard lock(g_mutex);
    for (Port* p : c->ports) delete p;
    delete c;
    g_client = nullptr;
    return 0;
}

int jack_activate(Client* c) {
    if (!c->process) return 1;
    c->active = true;
    c->thread = std::thread(run, c);
    return 0;
}

int jack_set_process_callback(Client* c, int (*cb)(Frames, void*), void* arg) {
    c->process = cb;
    c->processArg = arg;
    return 0;
}

int jack_set_xrun_callback(Client* c, int (*cb)(void*), void* arg) {
    c->xrun = cb;
    c->xrunArg = arg;
    return 0;
}

int jack_set_buffer_size_callback(Client* c, int (*cb)(Frames, void*), void* arg) {
    c->bufferSize = cb;
    c->bufferSizeArg = arg;
    return 0;
}

int jack_set_sample_rate_callback(Client* c, int (*cb)(Frames, void*), void* arg) {
    c->sampleRate = cb;
    c->sampleRateArg = arg;
    if (cb) cb(g_rate.load(), arg);  // like jack2: once, with the current rate
    return 0;
}

void jack_on_shutdown(Client* c, void (*cb)(void*), void* arg) {
    c->shutdown = cb;
    c->shutdownArg = arg;
}

Port* jack_port_register(Client* c, const char* name, const char*, unsigned long flags,
                         unsigned long) {
    auto* p = new Port;
    p->name = c->name + ":" + name;
    p->output = (flags & 0x2) != 0;
    c->ports.push_back(p);
    return p;
}

void* jack_port_get_buffer(Port* p, Frames) { return p->buffer.data(); }

const char* jack_port_name(const Port* p) { return p->name.c_str(); }

void jack_port_get_latency_range(Port* p, int mode, Frames* range) {
    range[0] = (p->output && mode == 1) ? 256 : 0;
    range[1] = (p->output && mode == 1) ? 512 : 0;
}

Frames jack_get_sample_rate(Client*) { return g_rate.load(); }

Frames jack_get_buffer_size(Client*) { return g_appliedPeriod.load(); }

int jack_set_buffer_size(Client*, Frames n) {
    g_bufferSizeRequest = n;
    return 0;
}

const char** jack_get_ports(Client*, const char*, const char*, unsigned long flags) {
    if ((flags & 0x5) != 0x5) return nullptr;  // physical inputs (playback) only
    static const char* kPlayback[] = {"system:playback_1", "system:playback_2"};
    auto** out = static_cast<const char**>(std::malloc(3 * sizeof(const char*)));
    out[0] = kPlayback[0];
    out[1] = kPlayback[1];
    out[2] = nullptr;
    return out;
}

int jack_connect(Client*, const char* from, const char* to) {
    if (!findPort(from) || std::strncmp(to, "system:playback_", 16) != 0) return 1;
    g_connections.fetch_add(1);
    return 0;
}

void jack_free(void* p) { std::free(p); }

// --- test controls ---------------------------------------------------------------------

void fakejack_set_period(Frames n) { g_period = std::min(n, kMaxPeriod); }
void fakejack_set_rate(Frames rate) {
    g_rate = rate;
    std::lock_guard lock(g_mutex);
    if (g_client && g_client->sampleRate) g_client->sampleRate(rate, g_client->sampleRateArg);
}
void fakejack_xrun() {
    std::lock_guard lock(g_mutex);
    if (g_client && g_client->xrun) g_client->xrun(g_client->xrunArg);
}
// Stalling returns once the process thread is idle: no cycle is in flight.
void fakejack_stall(int stalled) {
    g_stalled = stalled != 0;
    std::lock_guard lock(g_mutex);
    if (stalled && g_client && g_client->active.load())
        while (!g_stallAck.load()) std::this_thread::yield();
}
void fakejack_fail_open(int fail) { g_failOpen = fail != 0; }
void fakejack_set_input(float value) { g_inputValue = value; }
// The server goes away: the process thread ends, then the shutdown callback.
void fakejack_shutdown() {
    std::lock_guard lock(g_mutex);
    if (!g_client) return;
    stopThread(g_client);
    if (g_client->shutdown) g_client->shutdown(g_client->shutdownArg);
}
int fakejack_open() {
    std::lock_guard lock(g_mutex);
    return g_client != nullptr;
}
std::uint64_t fakejack_cycles() { return g_cycles.load(); }
Frames fakejack_buffer_size_request() { return g_bufferSizeRequest.load(); }
int fakejack_connections() { return g_connections.load(); }
const char* fakejack_latency_env() { return g_latencyEnv.c_str(); }
// The peak so far on one output port, or on all of them for "".
float fakejack_output_peak(const char* port) {
    std::lock_guard lock(g_mutex);
    if (*port == '\0') {
        float peak = g_client ? 0.0f : -1.0f;
        if (g_client)
            for (Port* p : g_client->ports)
                if (p->output) peak = std::max(peak, p->peak.load());
        return peak;
    }
    Port* p = findPort(port);
    return p ? p->peak.load() : -1.0f;
}

}  // extern "C"
