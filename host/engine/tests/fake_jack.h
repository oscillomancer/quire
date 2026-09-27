// The test controls of the stand-in libjack (fake_jack.cpp). The backend
// dlopens the same file, so both share one copy of its state.
#pragma once

#include <dlfcn.h>

#include <cstdint>
#include <stdexcept>
#include <string>

struct FakeJack {
    static constexpr const char* kPath = FAKE_JACK_PATH;

    void* handle = ::dlopen(kPath, RTLD_NOW | RTLD_LOCAL);

    template <class F>
    F fn(const char* name) const {
        auto f = reinterpret_cast<F>(::dlsym(handle, name));
        if (!f) throw std::runtime_error(std::string("fake libjack lacks ") + name);
        return f;
    }

    void (*setPeriod)(std::uint32_t) = fn<void (*)(std::uint32_t)>("fakejack_set_period");
    void (*setRate)(std::uint32_t) = fn<void (*)(std::uint32_t)>("fakejack_set_rate");
    void (*xrun)() = fn<void (*)()>("fakejack_xrun");
    void (*stall)(int) = fn<void (*)(int)>("fakejack_stall");
    void (*failOpen)(int) = fn<void (*)(int)>("fakejack_fail_open");
    void (*setInput)(float) = fn<void (*)(float)>("fakejack_set_input");
    void (*shutdown)() = fn<void (*)()>("fakejack_shutdown");
    int (*open)() = fn<int (*)()>("fakejack_open");
    std::uint64_t (*cycles)() = fn<std::uint64_t (*)()>("fakejack_cycles");
    std::uint32_t (*bufferSizeRequest)() = fn<std::uint32_t (*)()>("fakejack_buffer_size_request");
    int (*connections)() = fn<int (*)()>("fakejack_connections");
    void (*midi)(int, std::uint32_t, int, int,
                 int) = fn<void (*)(int, std::uint32_t, int, int, int)>("fakejack_midi");
    const char* (*latencyEnv)() = fn<const char* (*)()>("fakejack_latency_env");
    float (*outputPeak)(const char*) = fn<float (*)(const char*)>("fakejack_output_peak");

    // Back to a healthy server at 48 kHz with the given period.
    void reset(std::uint32_t period = 1024) {
        stall(0);
        failOpen(0);
        setInput(0.0f);
        setRate(48000);
        setPeriod(period);
    }
};

inline FakeJack& fakeJack() {
    static FakeJack f;
    return f;
}
