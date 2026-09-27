// The JACK backend against a stand-in libjack (fake_jack.cpp): no server and
// no device. The real server is exercised only on request:
//   SUPPLE_AUDIO_DEVICE_TEST=1 engine.tests -tc='*real JACK*'
#include <doctest/doctest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>

#include <supple/engine/jack.h>

#include "fake_jack.h"

using namespace supple;
using namespace supple::engine;

namespace {

// Records what the backend passes to the engine's callback.
struct Recorder {
    std::atomic<std::uint64_t> calls{0}, frames{0};
    std::atomic<std::uint32_t> maxFrames{0}, lastFrames{0};
    std::atomic<int> finals{0};
    std::atomic<bool> finalOnAudioThread{false};
    std::atomic<std::thread::id> audioThread{};
    std::atomic<float> firstInput{0.0f};

    Backend::Callback callback() {
        return [this](const float* const* in, float* const* out, std::uint32_t n) {
            if (n == 0) {
                finals.fetch_add(1);
                finalOnAudioThread = audioThread.load() == std::this_thread::get_id();
                return;
            }
            audioThread = std::this_thread::get_id();
            calls.fetch_add(1);
            frames.fetch_add(n);
            lastFrames = n;
            if (n > maxFrames.load()) maxFrames = n;
            if (in) firstInput = in[0][0];
            for (std::uint32_t i = 0; i < n; ++i) out[0][i] = out[1][i] = 0.25f;
        };
    }
};

JackBackend::Options fakeOptions() {
    JackBackend::Options o;
    o.library = FakeJack::kPath;
    o.clientName = "supple";
    return o;
}

bool waitFor(const std::function<bool()>& pred, double seconds = 5.0) {
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(seconds);
    while (!pred()) {
        if (std::chrono::steady_clock::now() > end) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
}

}  // namespace

TEST_CASE("a missing libjack is a clean error") {
    JackBackend::Options o;
    o.library = "libsupple-no-such-jack.so.0";
    JackBackend backend(o);
    Recorder rec;
    auto st = backend.start(BackendConfig{}, rec.callback());
    REQUIRE_FALSE(st);
    CHECK(st.error().code == "backend.unavailable");
    CHECK_FALSE(JackBackend::available(o.library));
    CHECK(JackBackend::available(FakeJack::kPath));
    backend.stop();  // harmless when not started
}

TEST_CASE("an unreachable server is a clean error") {
    FakeJack& fake = fakeJack();
    fake.reset();
    fake.failOpen(1);
    JackBackend backend(fakeOptions());
    Recorder rec;
    auto st = backend.start(BackendConfig{}, rec.callback());
    REQUIRE_FALSE(st);
    CHECK(st.error().code == "backend.open");
    CHECK(st.error().message.find("server failed") != std::string::npos);
    fake.reset();
}

TEST_CASE("the backend runs the engine in blocks, whatever the graph's period") {
    FakeJack& fake = fakeJack();
    fake.reset(1024);
    ::unsetenv("PIPEWIRE_LATENCY");
    JackBackend backend(fakeOptions());
    Recorder rec;
    BackendConfig cfg;
    cfg.blockSize = 256;
    REQUIRE(backend.start(cfg, rec.callback()));
    // The latency request reached the client and left the environment clean.
    CHECK(std::string(fake.latencyEnv()) == "256/48000");
    CHECK(std::getenv("PIPEWIRE_LATENCY") == nullptr);
    CHECK(fake.connections() == 2);
    CHECK(backend.config().sampleRate == 48000.0);
    CHECK(backend.config().blockSize == 256);

    REQUIRE(waitFor([&] { return rec.calls.load() >= 40; }));
    CHECK(rec.maxFrames.load() == 256);
    CHECK(backend.stats().period == 1024);
    CHECK(backend.stats().outputLatency == 512);
    CHECK(static_cast<double>(fake.outputPeak("supple:out_1")) == doctest::Approx(0.25));

    // A smaller period passes through; an odd one is split with a remainder.
    fake.setPeriod(300);
    REQUIRE(waitFor([&] { return backend.stats().period == 300; }));
    const auto before = rec.calls.load();
    REQUIRE(waitFor([&] { return rec.calls.load() > before + 20; }));
    CHECK(rec.maxFrames.load() == 256);
    fake.setPeriod(64);
    REQUIRE(waitFor([&] { return backend.stats().period == 64; }));
    REQUIRE(waitFor([&] { return rec.lastFrames.load() == 64; }));

    backend.stop();
    CHECK(rec.finals.load() == 1);
    CHECK(rec.finalOnAudioThread.load());
    CHECK(fake.open() == 0);
    fake.reset();
}

TEST_CASE("a user's PIPEWIRE_LATENCY wins; forceBufferSize asks for the period") {
    FakeJack& fake = fakeJack();
    fake.reset(1024);
    ::setenv("PIPEWIRE_LATENCY", "512/48000", 1);
    auto o = fakeOptions();
    o.forceBufferSize = true;
    JackBackend backend(o);
    Recorder rec;
    BackendConfig cfg;
    cfg.blockSize = 64;
    REQUIRE(backend.start(cfg, rec.callback()));
    CHECK(std::string(fake.latencyEnv()) == "512/48000");
    CHECK(std::string(std::getenv("PIPEWIRE_LATENCY")) == "512/48000");
    CHECK(fake.bufferSizeRequest() == 64);
    backend.stop();
    ::unsetenv("PIPEWIRE_LATENCY");
    fake.reset();
}

TEST_CASE("inputs reach the callback") {
    FakeJack& fake = fakeJack();
    fake.reset(128);
    fake.setInput(0.5f);
    JackBackend backend(fakeOptions());
    Recorder rec;
    BackendConfig cfg;
    cfg.inputs = 2;
    REQUIRE(backend.start(cfg, rec.callback()));
    REQUIRE(waitFor([&] { return rec.firstInput.load() == 0.5f; }));
    backend.stop();
    fake.reset();
}

TEST_CASE("xruns are counted and a rate change is reported") {
    FakeJack& fake = fakeJack();
    fake.reset();
    JackBackend backend(fakeOptions());
    Recorder rec;
    REQUIRE(backend.start(BackendConfig{}, rec.callback()));
    CHECK_FALSE(backend.stats().rateChanged);  // the registration call carries the same rate
    fake.xrun();
    fake.xrun();
    fake.xrun();
    CHECK(backend.stats().xruns == 3);
    fake.setRate(44100);
    CHECK(backend.stats().rateChanged);
    backend.stop();
    fake.reset();
}

TEST_CASE("the backend reports the graph's rate, not the requested one") {
    FakeJack& fake = fakeJack();
    fake.reset();
    fake.setRate(44100);
    JackBackend backend(fakeOptions());
    Recorder rec;
    BackendConfig cfg;
    cfg.sampleRate = 48000.0;
    REQUIRE(backend.start(cfg, rec.callback()));
    CHECK(backend.config().sampleRate == 44100.0);
    backend.stop();
    fake.reset();
}

TEST_CASE("stop closes the client when no cycle comes") {
    FakeJack& fake = fakeJack();
    fake.reset();
    auto o = fakeOptions();
    o.stopTimeoutSeconds = 0.05;
    JackBackend backend(o);
    Recorder rec;
    REQUIRE(backend.start(BackendConfig{}, rec.callback()));
    REQUIRE(waitFor([&] { return rec.calls.load() > 0; }));
    fake.stall(1);  // returns with no cycle in flight
    backend.stop();
    CHECK(rec.finals.load() == 0);  // no final callback: the engine recovers itself
    CHECK(fake.open() == 0);
    fake.reset();
}

TEST_CASE("a server shutdown is reported and stop does not wait") {
    FakeJack& fake = fakeJack();
    fake.reset();
    auto o = fakeOptions();
    o.stopTimeoutSeconds = 60.0;  // must not be spent waiting for a final cycle
    JackBackend backend(o);
    Recorder rec;
    REQUIRE(backend.start(BackendConfig{}, rec.callback()));
    REQUIRE(waitFor([&] { return rec.calls.load() > 0; }));
    fake.shutdown();
    CHECK(backend.stats().serverGone);
    const auto t0 = std::chrono::steady_clock::now();
    backend.stop();
    CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(30));
    CHECK(rec.finals.load() == 0);
    CHECK(fake.open() == 0);
    fake.reset();
}

TEST_CASE("the backend restarts") {
    FakeJack& fake = fakeJack();
    fake.reset();
    JackBackend backend(fakeOptions());
    for (int round = 0; round < 3; ++round) {
        Recorder rec;
        REQUIRE(backend.start(BackendConfig{}, rec.callback()));
        CHECK_FALSE(backend.start(BackendConfig{}, rec.callback()));  // already running
        REQUIRE(waitFor([&] { return rec.calls.load() > 0; }));
        backend.stop();
        CHECK(rec.finals.load() == 1);
    }
}

// Opens the real server for two seconds of silence at a 64-frame block and
// reports what the graph granted. Opt-in: it uses the audio device.
TEST_CASE("real JACK: silence at 64 frames" *
          doctest::skip(std::getenv("SUPPLE_AUDIO_DEVICE_TEST") == nullptr)) {
    JackBackend::Options o;
    o.clientName = "supple-device-test";
    JackBackend backend(o);
    Recorder rec;
    BackendConfig cfg;
    cfg.blockSize = 64;
    const Backend::Callback record = rec.callback();
    auto st = backend.start(cfg, [&](const float* const* in, float* const* out, std::uint32_t n) {
        record(in, out, n);
        for (std::uint32_t i = 0; i < n; ++i) out[0][i] = out[1][i] = 0.0f;  // silence
    });
    REQUIRE_MESSAGE(st, (st ? "" : st.error().toString()));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto s = backend.stats();
    MESSAGE("rate " << backend.config().sampleRate << " Hz, period " << s.period
                    << " frames, output latency " << s.outputLatency << " frames, " << s.cycles
                    << " cycles, " << s.xruns << " xruns");
    backend.stop();
    CHECK(s.cycles > 0);
    CHECK(rec.maxFrames.load() <= 64);
    CHECK(rec.finals.load() == 1);
    CHECK(rec.finalOnAudioThread.load());
}
