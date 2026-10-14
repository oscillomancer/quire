// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <array>
#include <cstdlib>
#include "portaudio_api.h"
#include "supple/engine/offline.h"
#include "supple/engine/portaudio.h"
#include "thread_startup.h"
using namespace supple;
using namespace supple::engine;
namespace {
struct Driver {
    static inline Driver* current;
    PaHostApiInfo host{1, paCoreAudio, "Test API", 2, 0, 1};
    std::array<PaDeviceInfo, 2> devices{{{2, "Capture", 0, 2, 0, .001, 0, .01, 0, 48000},
                                         {2, "Playback", 0, 0, 2, 0, .002, 0, .02, 48000}}};
    PaStreamInfo info{1, .001, .002, 48000};
    PaStreamCallback* callback = nullptr;
    PaStreamFinishedCallback* finished = nullptr;
    void* context = nullptr;
    PaStreamParameters input{}, output{};
    PaError openError = paNoError, startError = paNoError, formatError = paFormatIsSupported;
    int active = 1, initialized = 0, closed = 0;
    std::thread audio;
    std::atomic<unsigned> audioRequest{0};
    std::array<std::array<float, 4096>, 2> in{}, out{};
    std::array<float*, 2> inputs{in[0].data(), in[1].data()}, outputs{out[0].data(), out[1].data()};
    Driver() { current = this; }
    ~Driver() {
        if (audio.joinable()) audio.join();
        current = nullptr;
    }
    int drive(unsigned long frames, PaStreamCallbackFlags flags = 0) {
        return callback(input.channelCount ? inputs.data() : nullptr, outputs.data(), frames,
                        nullptr, flags, context);
    }
    static const detail::PortAudioApi api;
};
const detail::PortAudioApi Driver::api{
    []() -> PaError {
        ++current->initialized;
        return paNoError;
    },
    []() -> PaError {
        --current->initialized;
        return paNoError;
    },
    [](PaError) -> const char* { return "fake driver failure"; },
    [] { return 1; },
    [] { return 0; },
    [](int index) -> const PaHostApiInfo* { return index == 0 ? &current->host : nullptr; },
    [] { return 2; },
    [](int index) -> const PaDeviceInfo* {
        return index >= 0 && index < 2 ? &current->devices[static_cast<unsigned>(index)] : nullptr;
    },
    [](const PaStreamParameters*, const PaStreamParameters*, double) {
        return current->formatError;
    },
    [](PaStream** stream, const PaStreamParameters* input, const PaStreamParameters* output, double,
       unsigned long, PaStreamFlags flags, PaStreamCallback* callback, void* context) -> PaError {
        CHECK(flags == (paClipOff | paDitherOff));
        current->input = input ? *input : PaStreamParameters{};
        current->output = *output;
        current->callback = callback;
        current->context = context;
        if (current->openError != paNoError) return current->openError;
        *stream = reinterpret_cast<PaStream*>(current);
        return paNoError;
    },
    [](PaStream*, PaStreamFinishedCallback* callback) -> PaError {
        current->finished = callback;
        return paNoError;
    },
    [](PaStream*) -> PaError { return current->startError; },
    [](PaStream*) -> PaError { return paNoError; },
    [](PaStream*) -> PaError {
        if (current->audio.joinable()) current->audio.join();
        ++current->closed;
        return paNoError;
    },
    [](PaStream*) -> const PaStreamInfo* { return &current->info; },
    [](PaStream*) {
        current->audioRequest.fetch_add(1, std::memory_order_release);
        current->audioRequest.notify_one();
        return current->active;
    },
    nullptr,
    false,
    false};
InstancePool pool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        const std::filesystem::path path = FIXTURE_CLAP_PATH;
        auto plugins = host::scan(std::span<const std::filesystem::path>(&path, 1));
        if (!plugins) return std::move(plugins).error();
        for (auto& plugin : *plugins)
            if (plugin.id == ref.id) return plugin;
        return Error("test.plugin", "fixture missing");
    });
}
model::Session session() {
    model::EditTxn edit(model::Session::create({.console9 = true}), 1);
    for (int n = 0; n < 8; ++n) {
        model::Track track;
        track.name = "Instrument";
        track.parent = model::kMaster;
        track.volumeDb = -24;
        const auto tr = edit.create(track);
        edit.modify<model::Track>(model::kMaster,
                                  [&](auto& master) { master.children.push_back(tr); });
        for (const char* id : {"com.suppleaudio.test.sine", "com.suppleaudio.test.gain"}) {
            model::FxInstance fx;
            fx.plugin.id = id;
            if (std::string_view(id) == "com.suppleaudio.test.gain")
                fx.patch =
                    json::parse(
                        R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
                        .value();
            const auto f = edit.create(fx);
            edit.modify<model::Track>(tr, [&](auto& t) { t.chain.push_back(f); });
        }
        model::MidiSource midi;
        midi.sequence.ppq = 960;
        midi.sequence.length = 960;
        midi.sequence.notes.push_back({0, 480, 0, static_cast<std::uint8_t>(60 + n), 100, 0});
        model::Take take;
        take.source = edit.create(midi);
        model::Item item;
        item.track = tr;
        item.length = 1;
        item.takes = {edit.create(take)};
        item.activeTake = item.takes[0];
        edit.create(item);
    }
    return edit.commit().value();
}
}  // namespace
TEST_CASE("PortAudio selection, planar chunks, latency and callback status without hardware") {
    Driver driver;
    PortAudioBackend backend(
        {.hostApi = "Test API", .inputDevice = "Capture", .outputDevice = "Playback"}, Driver::api);
    auto devices = backend.driverDevices();
    REQUIRE(devices);
    REQUIRE(devices->size() == 2);
    CHECK((*devices)[1].defaultOutput);
    CHECK(driver.initialized == 0);
    unsigned calls = 0, frames = 0, finals = 0, largest = 0;
    REQUIRE(backend.start({.blockSize = 64, .inputs = 2},
                          [&](const float* const* in, float* const* out, std::uint32_t n) {
                              if (!n) {
                                  ++finals;
                                  return;
                              }
                              ++calls;
                              largest = std::max(largest, n);
                              for (unsigned c = 0; c < 2; ++c)
                                  for (unsigned i = 0; i < n; ++i) out[c][i] = in[c][i];
                              frames += n;
                          }));
    // Fill outside the RT callback.
    for (unsigned i = 0; i < 211; ++i) driver.in[0][i] = float(i) / 211.0f;
    driver.drive(211, paOutputUnderflow | paInputOverflow);
    CHECK(calls == 4);
    CHECK(largest == 64);
    CHECK(frames == 211);
    CHECK(driver.out[0] == driver.in[0]);
    auto status = backend.status();
    CHECK(status.period == 211);
    CHECK(status.xruns == 1);  // one affected callback, even with two flags
    CHECK(status.inputLatency == 48);
    CHECK(status.outputLatency == 96);
    driver.info.sampleRate = 44100;
    CHECK(backend.status().restartReason == "sample rate changed");
    driver.finished(driver.context);
    CHECK(backend.status().restartReason == "device lost");
    backend.stop();
    CHECK(finals == 1);
    CHECK(driver.initialized == 0);
    CHECK(driver.closed == 1);
}
TEST_CASE("PortAudio failed selection and stream startup release resources") {
    Driver driver;
    PortAudioBackend missing({.hostApi = "unknown"}, Driver::api);
    auto bad = missing.start({}, [](auto, auto, auto) {});
    REQUIRE_FALSE(bad);
    CHECK(bad.error().path == "/host_api");
    CHECK_FALSE(bad.error().hint.empty());
    CHECK(driver.initialized == 0);
    PortAudioBackend backend({}, Driver::api);
    auto midi = backend.start({.midiInputs = true}, [](auto, auto, auto) {});
    REQUIRE_FALSE(midi);
    CHECK(midi.error().path == "/midi_inputs");
    CHECK(driver.initialized == 0);
    driver.startError = paDeviceUnavailable;
    CHECK_FALSE(backend.start({}, [](auto, auto, auto) {}));
    CHECK(driver.closed == 1);
    CHECK(driver.initialized == 0);
    driver.startError = paNoError;
    driver.formatError = paInvalidSampleRate;
    CHECK_FALSE(backend.start({}, [](auto, auto, auto) {}));
    CHECK(driver.initialized == 0);
    driver.formatError = paNoError;
    CHECK_FALSE(backend.start({.inputs = 3}, [](auto, auto, auto) {}));
    CHECK(driver.initialized == 0);
}
TEST_CASE("PortAudio fake device output is bit-identical to offline in both executors") {
    const auto model = session();
    auto offlinePool = pool();
    auto offline = renderOffline(model, offlinePool, {.startSeconds = 0, .endSeconds = .1});
    REQUIRE(offline);
    for (bool ahead : {false, true})
        for (int workers : {0, 1, 7}) {
            INFO(ahead, workers);
            Driver driver;
            PortAudioBackend backend({}, Driver::api);
            auto rtPool = pool();
            MediaCache media;
            Engine engine(rtPool, media, {.anticipative = ahead, .workers = workers});
            REQUIRE(engine.start(backend, {.blockSize = 64}));
            REQUIRE(engine.load(model));
            engine.play();

            std::array<std::vector<float>, 2> captured;
            const std::size_t needed = offline->audio.frames() + engine.outputLatency();
            while (captured[0].size() < needed) {
                if (ahead) REQUIRE(engine.awaitRendered(4096));
                // Odd periods exercise the final short chunk as well as full blocks.
                driver.drive(211);
                for (unsigned c = 0; c < 2; ++c)
                    captured[c].insert(captured[c].end(), driver.out[c].begin(),
                                       driver.out[c].begin() + 211);
            }
            CHECK(engine.anticipationMisses() == 0);
            const auto offset = engine.outputLatency();
            engine.stop();
            CHECK(engine.processErrors() == 0);
            for (unsigned c = 0; c < 2; ++c)
                CHECK(std::equal(offline->audio.channels[c].begin(),
                                 offline->audio.channels[c].end(), captured[c].begin() + offset));
        }
}
#if defined(__APPLE__) || defined(_WIN32)
TEST_CASE("real native audio device is opt-in") {
    const char* enabled = std::getenv("SUPPLE_AUDIO_DEVICE_TEST");
    if (!enabled || std::string_view(enabled) != "1") return;
    PortAudioBackend backend;
    REQUIRE(backend.start({.blockSize = 128}, [](auto, float* const* out, std::uint32_t n) {
        if (n)
            for (unsigned c = 0; c < 2; ++c) std::fill_n(out[c], n, 0.0f);
    }));
    std::this_thread::sleep_for(std::chrono::seconds(2));
    backend.stop();
}
#endif

TEST_CASE("PortAudio stop makes its final callback on the device thread and joins it") {
    Driver driver;
    auto api = Driver::api;
    api.threaded = true;
    PortAudioBackend backend({}, api);
    unsigned finals = 0;
    std::thread::id finalThread;
    REQUIRE(backend.start({}, [&](auto, auto, std::uint32_t frames) {
        if (!frames) {
            ++finals;
            finalThread = std::this_thread::get_id();
        }
    }));
    driver.audio = std::thread([&] {
        driver.audioRequest.wait(0, std::memory_order_acquire);
        driver.drive(128);
    });
    const auto deviceThread = driver.audio.get_id();
    backend.stop();
    CHECK(finals == 1);
    CHECK(finalThread == deviceThread);
    CHECK_FALSE(driver.audio.joinable());
    backend.stop();
    CHECK(finals == 1);
}
TEST_CASE("native helper setup is self-applied and rolls back all helpers on refusal") {
    struct PolicyBackend : DummyBackend {
        PolicyBackend() : DummyBackend({.mode = DummyBackend::Mode::Manual}) {}
        std::thread::id owner = std::this_thread::get_id();
        std::atomic<unsigned> prepared{0}, released{0}, wrongThread{0};
        bool refuse = false;
        Status prepareCurrentThread(bool) override {
            if (std::this_thread::get_id() == owner) ++wrongThread;
            const auto n = prepared.fetch_add(1);
            if (refuse && n == 0) return Error("thread.test", "forced refusal");
            return okStatus();
        }
        void releaseCurrentThread() override { ++released; }
    };
    for (bool refuse : {false, true}) {
        PolicyBackend backend;
        backend.refuse = refuse;
        detail::ThreadStartup setup(2, &backend, true);
        std::array<std::thread, 2> helpers;
        std::atomic<unsigned> working{0};
        for (unsigned i = 0; i < 2; ++i)
            helpers[i] = std::thread([&, i] {
                const bool ready = setup.enter(i);
                detail::ThreadStartup::Cleanup cleanup{ready ? &backend : nullptr};
                if (ready) ++working;
            });
        auto configured = setup.finish();
        CHECK(configured.ok() == !refuse);
        for (auto& helper : helpers) helper.join();
        CHECK(backend.prepared == 2);
        CHECK(backend.released == 2);
        CHECK(backend.wrongThread == 0);
        CHECK(working == (refuse ? 0 : 2));
    }
}

TEST_CASE("native background policy leaves two usable CPUs free") {
    const unsigned cpus = allowedCpuCount();
    // EngineOptions caps helpers at 64; above 66 CPUs every legal pool fits.
    if (cpus > 66) return;
    struct CapacityBackend : DummyBackend {
        CapacityBackend() : DummyBackend({.mode = DummyBackend::Mode::Manual}) {}
        std::atomic<unsigned> prepared{0};
        bool nativeThreadPolicy() const override { return true; }
        unsigned liveThreadCapacity() const override { return 0; }
        Status prepareCurrentThread(bool) override {
            ++prepared;
            return okStatus();
        }
    } backend;
    auto instances = pool();
    MediaCache media;
    Engine engine(instances, media, {.workers = static_cast<int>(cpus > 1 ? cpus - 2 : 0)});
    REQUIRE(engine.start(backend, {}));
    CHECK(backend.prepared == 0);
    CHECK(engine.scheduling().renderer.kind == ThreadPolicy::Kind::Normal);
    engine.stop();
}
