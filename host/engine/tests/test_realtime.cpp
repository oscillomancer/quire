#include <doctest/doctest.h>

#include <chrono>
#include <cmath>
#include <thread>

#include <unistd.h>

#include <supple/audio/audio_file.h>
#include <supple/engine/jack.h>
#include <supple/engine/realtime.h>
#include <supple/host/clap_host.h>

#include "fake_jack.h"

using namespace supple;
using namespace supple::engine;
using model::Id;
using model::kMaster;

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;

InstancePool makePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

// A track playing a sine instrument: notes on beats 0, 1, 2, 3 (quarter notes).
model::Session synthSession(double volumeDb = 0.0, const char* extraFx = nullptr) {
    model::EditTxn t(model::Session::create(), 1);
    model::Track tr;
    tr.name = "Synth";
    tr.parent = kMaster;
    tr.volumeDb = volumeDb;
    const Id track = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
    for (const char* plugin : {"com.suppleaudio.test.sine", extraFx}) {
        if (!plugin) continue;
        model::FxInstance fx;
        fx.plugin.id = plugin;
        if (std::string(plugin) == "com.suppleaudio.test.gain")
            fx.patch =
                json::parse(
                    R"({"schema": "supple.test-gain-patch/1.0", "params": {"latency": "short"}})")
                    .value();
        const Id f = t.create(fx);
        t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(f); });
    }
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 4 * 960;
    for (int i = 0; i < 4; ++i)
        src.sequence.notes.push_back(
            {i * 960, 480, 0, static_cast<std::uint8_t>(60 + 4 * i), 100, 0});
    const Id mi = t.create(src);
    model::Take take;
    take.source = mi;
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 4.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    return t.commit().value();
}

// A sine track whose notes start at the given ticks (ppq 960, 120 bpm: 25 samples a tick).
model::Session clusterSession(std::initializer_list<int> ticks) {
    model::EditTxn t(model::Session::create(), 1);
    model::Track tr;
    tr.name = "Synth";
    tr.parent = kMaster;
    const Id track = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.sine";
    const Id f = t.create(fx);
    t.modify<model::Track>(track, [&](model::Track& x) { x.chain.push_back(f); });
    model::MidiSource src;
    src.sequence.ppq = 960;
    src.sequence.length = 4 * 960;
    std::uint8_t key = 60;
    for (int tick : ticks) src.sequence.notes.push_back({tick, 960, 0, key++, 100, 0});
    const Id mi = t.create(src);
    model::Take take;
    take.source = mi;
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 4.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    return t.commit().value();
}

double maxDiff(const std::vector<float>& a, std::size_t aFrom, const std::vector<float>& b,
               std::size_t n) {
    double worst = 0;
    for (std::size_t i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(double(a[aFrom + i]) - double(b[i])));
    return worst;
}

const EngineOptions kInCallback{.anticipative = false};

// Anticipative: drive a manual backend `cycles` times, letting the renderer
// keep ahead (a device's callbacks would be paced by its clock).
void runAhead(Engine& engine, DummyBackend& backend, std::int64_t cycles, std::uint32_t block) {
    while (cycles > 0) {
        REQUIRE(engine.awaitRendered(4096));
        const std::int64_t n =
            std::min<std::int64_t>(cycles, std::max<std::int64_t>(1, 4096 / block));
        backend.run(n);
        cycles -= n;
    }
}

}  // namespace

TEST_CASE("real-time output equals the offline render (any block size)" *
          doctest::description("rt")) {
    for (const EngineOptions& options :
         {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 3}})
        for (const char* extra : {static_cast<const char*>(nullptr), "com.suppleaudio.test.gain"}) {
            INFO("anticipative " << options.anticipative << ", workers " << options.workers);
            const model::Session s = synthSession(-3.0, extra);
            auto pool = makePool();
            auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();

            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend(
                {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
            Engine engine(rtPool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}).ok());
            REQUIRE(engine.load(s).ok());
            engine.play();
            if (options.anticipative)
                runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
            else
                backend.run((48000 * 2 + 4096) / 128);
            CHECK(engine.anticipationMisses() == 0);
            engine.stop();
            const std::uint32_t L = engine.outputLatency();
            CHECK(L == (extra ? 64u : 0u));
            CHECK(engine.processErrors() == 0);
            const auto& cap = backend.capture();
            // The live output lags by the plan's latency; after that it is the offline render.
            CHECK(maxDiff(cap.channels[0], L, offline.audio.channels[0], 96000) == 0.0);
            CHECK(maxDiff(cap.channels[1], L, offline.audio.channels[1], 96000) == 0.0);
        }
}

TEST_CASE(
    "in the callback: plan swaps while playing, seek, pause; retired plans and instances are "
    "freed") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
    REQUIRE(engine.load(synthSession(0.0)).ok());
    engine.play();
    backend.run(10);
    CHECK(engine.transport().playing);
    CHECK(engine.transport().sample == 2560);
    // A new version (-6 dB) replaces the plan at the next cycle; the instance is kept.
    REQUIRE(engine.load(synthSession(-6.0206)).ok());
    CHECK(pool.size() == 1);  // same fx id and plugin: the instance (and its state) is kept
    backend.run(10);
    engine.service();
    const auto& cap = backend.capture();
    auto peakOf = [&](std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to; ++i)
            m = std::max(m, std::fabs(double(cap.channels[0][i])));
        return m;
    };
    const double before = peakOf(0, 2560), after = peakOf(2560, 5120);
    CHECK(after / before == doctest::Approx(0.5).epsilon(0.01));
    // Seek to beat 2 (1.0 s): the output there matches an offline render at that point.
    engine.seek(1.0);
    backend.run(1);
    CHECK(engine.transport().sample == 48000 + 256);
    engine.pause();
    backend.run(2);
    CHECK_FALSE(engine.transport().playing);
    CHECK(engine.transport().sample == 48000 + 256);  // paused: the position holds
    engine.stop();
}

TEST_CASE("threaded backend: plan swaps from the main thread race-free" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Thread});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 64}).ok());
    REQUIRE(engine.load(synthSession(0.0)).ok());
    engine.play();
    for (int i = 0; i < 20; ++i) {
        REQUIRE(engine.load(synthSession(-0.5 * i)).ok());
        while (backend.cycles() < 20 * (i + 1)) std::this_thread::yield();
        engine.service();
        if (i % 7 == 3) engine.seek(0.25 * i);
    }
    engine.stop();
    CHECK(engine.processErrors() == 0);
    CHECK(backend.cycles() >= 400);
}

TEST_CASE(
    "in the callback: transport requests between callbacks are never dropped (review round 3)") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4800});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
    REQUIRE(engine.load(synthSession(0.0)).ok());
    for (int i = 0; i < 40; ++i) {  // 120 requests, more than any queue held
        engine.play();
        engine.seek(0.5);
        engine.pause();
    }
    engine.seek(1.0);
    engine.play();
    backend.run(1);
    CHECK(engine.transport().playing);
    CHECK(engine.transport().sample == 48000 + 256);
    for (int i = 0; i < 70; ++i) engine.play();
    engine.pause();
    backend.run(1);
    CHECK_FALSE(engine.transport().playing);
    CHECK(engine.transport().sample == 48000 + 256);
    engine.stop();
}

TEST_CASE("a seek off the block grid never grows event storage on the audio thread" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.renderBlock = 100}}) {
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4800});
        Engine engine(pool, media, options);
        // Two notes at sample 75, two at 100: aligned 100-frame blocks hold two
        // each, but the block [1, 101) after a one-sample seek holds all four.
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 100}).ok());
        REQUIRE(engine.load(clusterSession({3, 3, 4, 4})).ok());
        engine.seek(1.0 / 48000.0);
        engine.play();
        if (options.anticipative) REQUIRE(engine.awaitRendered(400));
        backend.run(4);  // under rtsan an allocation here aborts
        CHECK(engine.processErrors() == 0);
        double energy = 0;
        for (std::size_t i = 100; i < 400; ++i)
            energy +=
                double(backend.capture().channels[0][i]) * double(backend.capture().channels[0][i]);
        CHECK(energy > 1.0);  // all four notes sound
        engine.stop();
    }
}

TEST_CASE("a plugin that refuses to start is passed through, not processed") {
    for (const EngineOptions& options : {kInCallback, EngineOptions{}}) {
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4800});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
        model::Session s = synthSession(0.0, "com.suppleaudio.test.edge");
        model::EditTxn t(s, 2);
        for (const auto& e : s.fx)
            if (e.value->plugin.id == "com.suppleaudio.test.edge")
                t.modify<model::FxInstance>(
                    e.id, [](model::FxInstance& f) { f.state = "sha256:edge-refuses-start"; });
        t.putBlob("sha256:edge-refuses-start",
                  std::make_shared<const model::Blob>(model::Blob{{1, 0, 0, 0}}));
        REQUIRE(engine.load(t.commit().value()).ok());
        engine.play();
        if (options.anticipative) REQUIRE(engine.awaitRendered(2560));
        backend.run(10);
        CHECK(engine.processErrors() >= 1);
        CHECK(engine.transport().sample == 2560);
        double peak = 0;
        for (std::size_t i = 0; i < 2560; ++i)
            peak = std::max(peak, std::fabs(double(backend.capture().channels[0][i])));
        CHECK(peak > 0.1);  // the sine passes through the stage that did not start
        engine.stop();
    }
}

TEST_CASE("stop() stops plugins on the audio thread even with second-long gaps between callbacks" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    // 8192 frames at 8 kHz: a callback about once a second, paced like a device.
    DummyBackend backend({.mode = DummyBackend::Mode::Thread, .paced = true});
    Engine engine(pool, media);
    model::Session s = synthSession(0.0);
    model::Project project = *s.project;
    project.sampleRate = 8000;
    s.project = std::make_shared<const model::Project>(project);
    REQUIRE(engine.start(backend, {.sampleRate = 8000, .blockSize = 8192}).ok());
    REQUIRE(engine.load(s).ok());
    engine.play();
    while (backend.cycles() < 1) std::this_thread::yield();
    engine.stop();  // lands mid-gap: no timeout, no stop from this thread
    CHECK(engine.lastStopOnAudioThread());
    CHECK(engine.processErrors() == 0);
}

TEST_CASE("a manual backend's stop is the audio thread's stop") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
    REQUIRE(engine.load(synthSession(0.0)).ok());
    engine.play();
    backend.run(2);
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());
}

TEST_CASE("the engine plays through the JACK backend (a stand-in libjack)" *
          doctest::description("rt")) {
    FakeJack& fake = fakeJack();
    fake.reset(1024);  // the graph's period is four engine blocks
    auto pool = makePool();
    MediaCache media;
    JackBackend::Options o;
    o.library = FakeJack::kPath;
    JackBackend backend(o);
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
    REQUIRE(engine.load(synthSession(0.0)).ok());
    engine.play();
    for (int i = 0; i < 5000 && fake.outputPeak("supple:out_1") <= 0.01f; ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(fake.outputPeak("supple:out_1") > 0.01f);
    CHECK(engine.transport().sample >= 256);
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());
    CHECK(engine.processErrors() == 0);
    fake.reset();
}

TEST_CASE("the engine survives the JACK server going away") {
    FakeJack& fake = fakeJack();
    fake.reset(256);
    auto pool = makePool();
    MediaCache media;
    JackBackend::Options o;
    o.library = FakeJack::kPath;
    JackBackend backend(o);
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
    REQUIRE(engine.load(synthSession(0.0)).ok());
    engine.play();
    const auto until = fake.cycles() + 5;
    while (fake.cycles() < until) std::this_thread::yield();
    fake.shutdown();
    CHECK(backend.stats().serverGone);
    engine.stop();  // no final callback: the plugins are stopped on this thread
    CHECK_FALSE(engine.lastStopOnAudioThread());
    CHECK_FALSE(engine.running());
    fake.reset();
}

TEST_CASE("the engine runs a session at the device's rate, not the project's") {
    // A 48 kHz project on a 44.1 kHz device: notes land at the same seconds
    // (0.25 s long, one per beat at 120 BPM) and the plugin runs at 44.1 kHz.
    for (bool anticipative : {false, true}) {
        CAPTURE(anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 44100});
        EngineOptions options;
        options.anticipative = anticipative;
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 44100, .blockSize = 147}).ok());
        const model::Session s = synthSession(0.0);
        REQUIRE(s.project->sampleRate == 48000.0);
        REQUIRE(engine.load(s).ok());
        engine.play();
        if (anticipative)
            runAhead(engine, backend, 44100 / 147, 147);
        else
            backend.run(44100 / 147);
        engine.stop();
        const auto& L = backend.capture().channels[0];
        REQUIRE(L.size() >= 44100);
        auto energy = [&](std::size_t from, std::size_t to) {
            double e = 0;
            for (std::size_t i = from; i < to; ++i) e += double(L[i]) * double(L[i]);
            return e;
        };
        CHECK(energy(0, 11000) > 1.0);       // note 1: 0 to 0.25 s
        CHECK(energy(11030, 22045) == 0.0);  // silence to beat 1
        CHECK(energy(22055, 33000) > 1.0);   // note 2 from 0.5 s = 22050
        CHECK(engine.transport().seconds == doctest::Approx(1.0));
    }
}

TEST_CASE(
    "anticipative: pause silences at once, seek plays no stale audio, the position is the "
    "listener's" *
    doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
    REQUIRE(engine.load(synthSession(0.0)).ok());
    engine.play();
    engine.awaitTransport();
    CHECK(engine.transport().playing);
    CHECK(engine.transport().sample == 0);
    runAhead(engine, backend, 10, 256);
    CHECK(engine.transport().sample == 2560);
    const auto& cap = backend.capture();
    auto peakOf = [&](std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to; ++i)
            m = std::max(m, std::fabs(double(cap.channels[0][i])));
        return m;
    };
    CHECK(peakOf(2304, 2560) > 0.1);  // the first note sounds
    engine.pause();
    backend.run(1);  // the rendered audio is dropped: silent at once
    CHECK(peakOf(2560, 2816) == 0.0);
    engine.awaitTransport();
    CHECK_FALSE(engine.transport().playing);
    CHECK(engine.transport().sample == 2560);  // the listener's position, not the renderer's
    engine.play();                             // resumes there
    engine.awaitTransport();
    runAhead(engine, backend, 4, 256);
    CHECK(engine.transport().sample == 2560 + 1024);
    engine.seek(1.0);  // while playing
    engine.awaitTransport();
    CHECK(engine.transport().sample == 48000);
    runAhead(engine, backend, 1, 256);
    CHECK(engine.transport().sample == 48000 + 256);
    CHECK(engine.anticipationMisses() == 0);
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());  // the renderer stopped the plugins
}

TEST_CASE("anticipative: an edit takes effect after the audio already rendered, without a gap" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
    REQUIRE(engine.load(synthSession(0.0)).ok());
    engine.play();
    runAhead(engine, backend, 10, 256);
    REQUIRE(engine.awaitRendered(1 << 20));  // 200 ms (9600 frames) ahead of 2560
    REQUIRE(engine.load(synthSession(-6.0206)).ok());
    runAhead(engine, backend, 150, 256);
    engine.service();
    const auto& cap = backend.capture();
    auto peakOf = [&](std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to; ++i)
            m = std::max(m, std::fabs(double(cap.channels[0][i])));
        return m;
    };
    // Note 1 (0 .. 12000) was rendered before the edit; note 2 (24000 ..) after.
    CHECK(peakOf(24000, 36000) / peakOf(2560, 12000) == doctest::Approx(0.5).epsilon(0.01));
    CHECK(engine.anticipationMisses() == 0);
    CHECK(pool.size() == 1);  // the instance was kept
    engine.stop();
}

TEST_CASE("anticipative: a renderer that falls behind counts misses and plays silence") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
    REQUIRE(engine.load(synthSession(0.0)).ok());
    engine.play();
    runAhead(engine, backend, 4, 256);
    engine.holdRenderer(true);
    backend.run(100);  // 25600 frames: more than was rendered ahead
    CHECK(engine.anticipationMisses() > 0);
    double tail = 0;
    for (std::size_t i = 25000; i < 26624; ++i)
        tail = std::max(tail, std::fabs(double(backend.capture().channels[0][i])));
    CHECK(tail == 0.0);
    engine.holdRenderer(false);
    runAhead(engine, backend, 10, 256);  // and it catches up
    engine.stop();
}

TEST_CASE("anticipative on a paced device: edits and transport from the main thread race-free" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Thread, .paced = true});
    Engine engine(pool, media, {.renderBlock = 512, .renderAheadMs = 50, .workers = 3});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}).ok());
    REQUIRE(engine.load(synthSession(0.0, "com.suppleaudio.test.gain")).ok());
    engine.play();
    for (int i = 0; i < 40; ++i) {
        const auto until = backend.cycles() + 4;
        while (backend.cycles() < until) std::this_thread::yield();
        switch (i % 5) {
            case 0:
                REQUIRE(engine.load(synthSession(-0.5 * i, "com.suppleaudio.test.gain")).ok());
                break;
            case 1:
                engine.seek(0.1 * i);
                break;
            case 2:
                engine.pause();
                break;
            case 3:
                engine.play();
                break;
            default:
                engine.service();
        }
    }
    engine.stop();
    CHECK(engine.processErrors() == 0);
    CHECK(engine.lastStopOnAudioThread());
}

TEST_CASE("anticipative: the renderer stops the plugins when the JACK server goes away") {
    FakeJack& fake = fakeJack();
    fake.reset(256);
    auto pool = makePool();
    MediaCache media;
    JackBackend::Options o;
    o.library = FakeJack::kPath;
    JackBackend backend(o);
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
    REQUIRE(engine.load(synthSession(0.0)).ok());
    engine.play();
    const auto until = fake.cycles() + 5;
    while (fake.cycles() < until) std::this_thread::yield();
    fake.shutdown();
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());
    CHECK_FALSE(engine.running());
    fake.reset();
}

// Phase 4 acceptance (engine.md §5): a crashing plugin in dedicated isolation
// does not take down the engine; only its node goes silent.
TEST_CASE("isolation: a plugin crashing in its bridge silences its node, the session plays on" *
          doctest::description("rt")) {
    host::setBridgeExecutable(BRIDGE_PATH);
    model::EditTxn t(model::Session::create(), 1);
    Id tracks[2];
    for (int k = 0; k < 2; ++k) {
        model::Track tr;
        tr.name = k ? "Isolated" : "Steady";
        tr.parent = kMaster;
        tr.pan = k ? 100.0 : -100.0;  // steady left, isolated right
        tracks[k] = t.create(tr);
        t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(tracks[k]); });
        std::vector<const char*> chain = {"com.suppleaudio.test.sine"};
        if (k) chain.push_back("com.suppleaudio.test.crash");
        for (const char* plugin : chain) {
            model::FxInstance fx;
            fx.plugin.id = plugin;
            if (k) fx.isolation = model::Isolation::Dedicated;
            const Id f = t.create(fx);
            t.modify<model::Track>(tracks[k], [&](model::Track& x) { x.chain.push_back(f); });
        }
        model::MidiSource src;
        src.sequence.ppq = 960;
        src.sequence.length = 8 * 960;
        src.sequence.notes.push_back(
            {0, 8 * 960, 0, static_cast<std::uint8_t>(60 + 7 * k), 100, 0});
        const Id mi = t.create(src);
        model::Take take;
        take.source = mi;
        const Id tk = t.create(take);
        model::Item item;
        item.track = tracks[k];
        item.length = 8.0;
        item.takes = {tk};
        item.activeTake = tk;
        t.create(item);
    }
    const model::Session s = t.commit().value();
    Id crashFx;
    for (const auto& e : s.fx)
        if (e.value->plugin.id == "com.suppleaudio.test.crash") crashFx = e.id;

    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000});
    Engine engine(pool, media);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
    REQUIRE(engine.load(s).ok());
    host::Instance* crash = pool.find(crashFx);
    REQUIRE(crash);
    CHECK(crash->isolated());
    engine.play();
    runAhead(engine, backend, 40, 256);  // 10240 frames: both tracks sound
    const auto& cap = backend.capture();
    auto peak = [&](int ch, std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to; ++i)
            m = std::max(m, std::fabs(double(cap.channels[static_cast<std::size_t>(ch)][i])));
        return m;
    };
    CHECK(peak(0, 5000, 10240) > 0.1);
    CHECK(peak(1, 5000, 10240) > 0.1);
    REQUIRE(crash->setParam(1, 1.0).ok());  // crash in its next process()
    runAhead(engine, backend, 200, 256);    // 51200 more frames
    CHECK(crash->failed());
    CHECK(pool.failed().size() == 1);
    CHECK(peak(0, 50000, 61440) > 0.1);   // the steady track plays on
    CHECK(peak(1, 50000, 61440) == 0.0);  // the crashed node is silent
    CHECK(engine.processErrors() == 0);
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());
}

TEST_CASE("plugin automation: real-time output equals the offline render" *
          doctest::description("rt")) {
    model::Session s = synthSession(0.0, "com.suppleaudio.test.gain");
    Id gainFx;
    for (const auto& e : s.fx)
        if (e.value->plugin.id == "com.suppleaudio.test.gain") gainFx = e.id;
    model::EditTxn t(s, 2);
    model::Envelope env;
    env.target = {gainFx, "gain"};
    env.points = {{0.0, 0.0, model::PointShape::Linear, 0.0},
                  {2.0, -30.0, model::PointShape::Slow, 0.0},
                  {3.0, 6.0, model::PointShape::Square, 0.0}};
    t.create(env);
    s = t.commit().value();
    auto pool = makePool();
    auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 3}}) {
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}).ok());
        REQUIRE(engine.load(s).ok());
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
        else
            backend.run((48000 * 2 + 4096) / 128);
        engine.stop();
        const std::uint32_t L = engine.outputLatency();
        const auto& cap = backend.capture();
        CHECK(maxDiff(cap.channels[0], L, offline.audio.channels[0], 96000) == 0.0);
    }
}

TEST_CASE("recording: every input frame lands in the take, placed where it was heard" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                              .input = [](float* const* in, std::uint32_t channels, std::uint32_t n,
                                          std::int64_t frame) {
                                  for (std::uint32_t c = 0; c < channels; ++c)
                                      for (std::uint32_t i = 0; i < n; ++i)
                                          in[c][i] = static_cast<float>((frame + i) % 1000) /
                                                     1000.0f * (c ? -1.0f : 1.0f);
                              }});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}).ok());
        const model::Session s = synthSession(0.0);
        REQUIRE(engine.load(s).ok());
        Id track;
        for (const auto& e : s.tracks)
            if (e.id != kMaster) track = e.id;
        const auto dir =
            std::filesystem::temp_directory_path() / ("supple-rec-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        REQUIRE(engine.startRecording({{track, {0, 1}, dir / "take.wav"}}).ok());
        CHECK(engine.recording());
        engine.seek(1.0);
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, 100, 256);
        else
            backend.run(100);
        auto takes = engine.stopRecording();
        REQUIRE(takes);
        REQUIRE(takes->size() == 1);
        const RecordedTake& t = (*takes)[0];
        CHECK(t.startSample == 48000);  // no device or plan latency here
        CHECK(t.frames == 100 * 256);
        CHECK(t.overflowFrames == 0);
        auto audio = audio::readAudio(t.file);
        REQUIRE(audio);
        REQUIRE(audio->frames() == static_cast<std::size_t>(t.frames));
        int breaks = 0;
        for (std::size_t i = 1; i < audio->frames(); ++i) {
            const double d = double(audio->channels[0][i]) - double(audio->channels[0][i - 1]);
            if (std::fabs(d - 0.001) > 1e-5 && std::fabs(d + 0.999) > 1e-5)
                ++breaks;  // wraps at 1000
            if (audio->channels[1][i] != -audio->channels[0][i]) ++breaks;
        }
        CHECK(breaks == 0);  // contiguous: nothing lost, nothing doubled
        engine.stop();
        std::filesystem::remove_all(dir);
    }
}

TEST_CASE("recording through a playback dropout keeps the take on the timeline" *
          doctest::description("rt")) {
    // The renderer stalls mid-take (a 384-frame block against 1024-frame
    // chunks, so one cycle is half played). Input that went with played
    // audio is recorded; input during the dropout had no timeline position
    // and is counted as lost, never recorded as if it had been heard.
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Manual,
         .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
             for (std::uint32_t c = 0; c < channels; ++c)
                 for (std::uint32_t i = 0; i < n; ++i) in[c][i] = 0.5f;
         }});
    Engine engine(pool, media, EngineOptions{});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 384, .inputs = 2}).ok());
    const model::Session s = synthSession(0.0);
    REQUIRE(engine.load(s).ok());
    Id track;
    for (const auto& e : s.tracks)
        if (e.id != kMaster) track = e.id;
    const auto dir =
        std::filesystem::temp_directory_path() / ("supple-rec-gap-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    REQUIRE(engine.startRecording({{track, {0}, dir / "take.wav"}}).ok());
    engine.play();
    runAhead(engine, backend, 20, 384);
    engine.holdRenderer(true);
    backend.run(80);  // 30720 frames: more than was rendered ahead
    engine.holdRenderer(false);
    runAhead(engine, backend, 20, 384);
    CHECK(engine.anticipationMisses() > 0);
    auto takes = engine.stopRecording();
    REQUIRE(takes);
    const RecordedTake& t = (*takes)[0];
    CHECK(t.startSample == 0);
    CHECK(t.overflowFrames > 0);
    CHECK(t.frames + static_cast<std::int64_t>(t.overflowFrames) == 120 * 384);
    CHECK(t.frames == engine.transport().sample);  // exactly what was heard
    engine.stop();
    std::filesystem::remove_all(dir);
}

namespace {
// `s` with its first track monitoring device inputs 1-2 (monitor on).
model::Session monitoring(model::Session s, model::Monitor mode = model::Monitor::On,
                          bool armed = false) {
    model::EditTxn t(s, 9);
    Id track;
    for (const auto& e : s.tracks)
        if (e.id != kMaster) track = e.id;
    model::Route r;
    r.from.hwChannels = std::vector<int>{1, 2};
    r.to.track = track;
    const Id in = t.create(r);
    t.modify<model::Track>(track, [&](model::Track& x) {
        x.record.input = in;
        x.record.monitor = mode;
        x.record.armed = armed;
    });
    return t.commit().value();
}

// Device inputs: a constant 0.5 on input 1 and -0.25 on input 2.
DummyBackend::Options constantInput(std::int64_t captureFrames) {
    return {.mode = DummyBackend::Mode::Manual,
            .captureFrames = captureFrames,
            .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t) {
                for (std::uint32_t i = 0; i < n; ++i) {
                    in[0][i] = 0.5f;
                    if (channels > 1) in[1][i] = -0.25f;
                }
            }};
}
}  // namespace

TEST_CASE("monitoring in the callback: the input is heard through the track, stopped or playing" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(constantInput(48000));
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}).ok());
    // The sine track, monitoring; its notes are silent at 1.75 s (between notes).
    REQUIRE(engine.load(monitoring(synthSession(-6.0206))).ok());
    backend.run(4);  // stopped: input only, through the fader (-6 dB) and centre pan (-3 dB)
    const auto& cap = backend.capture();
    const double g = 0.5 * 0.70710678;
    CHECK(double(cap.channels[0][600]) == doctest::Approx(0.5 * g).epsilon(1e-4));
    CHECK(double(cap.channels[1][600]) == doctest::Approx(-0.25 * g).epsilon(1e-4));
    CHECK(engine.transport().sample == 0);  // stopped: time stands still
    engine.seek(1.75);
    engine.play();
    backend.run(4);
    CHECK(double(cap.channels[0][1024 + 600]) == doctest::Approx(0.5 * g).epsilon(1e-4));
    CHECK(engine.transport().sample == static_cast<std::int64_t>(1.75 * 48000) + 1024);
    engine.stop();
}

namespace {
// The sine track plus a "Vocal" track (a gain plugin at -6 dB) that monitors
// device inputs 1-2 when `monitored`.
model::Session withVocal(bool monitored) {
    model::Session s = synthSession(0.0);
    model::EditTxn t(s, 8);
    model::Track tr;
    tr.name = "Vocal";
    tr.parent = kMaster;
    tr.pan = 30.0;
    const Id vocal = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(vocal); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.gain";
    fx.patch =
        json::parse(R"({"schema": "supple.test-gain-patch/1.0", "params": {"gain": -6}})").value();
    const Id f = t.create(fx);
    t.modify<model::Track>(vocal, [&](model::Track& x) { x.chain.push_back(f); });
    model::Route r;
    r.from.hwChannels = std::vector<int>{1, 2};
    r.to.track = vocal;
    const Id in = t.create(r);
    t.modify<model::Track>(vocal, [&](model::Track& x) {
        x.record.input = in;
        x.record.monitor = monitored ? model::Monitor::On : model::Monitor::Off;
    });
    return t.commit().value();
}

// Device input: a slow ramp on input 1, its negative on input 2.
DummyBackend::Options rampInput(std::int64_t captureFrames) {
    return {
        .mode = DummyBackend::Mode::Manual,
        .captureFrames = captureFrames,
        .input = [](float* const* in, std::uint32_t channels, std::uint32_t n, std::int64_t frame) {
            for (std::uint32_t i = 0; i < n; ++i) {
                const float v = static_cast<float>((frame + i) % 4800) / 4800.0f - 0.5f;
                in[0][i] = v;
                if (channels > 1) in[1][i] = -v;
            }
        }};
}
}  // namespace

TEST_CASE("anticipative monitoring: live input and rendered playback, identical to the reference" *
          doctest::description("rt")) {
    const model::Session s = withVocal(true);
    constexpr std::int64_t kCycles = 400;  // 102400 frames at 256
    audio::AudioData reference;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 3}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend(rampInput(kCycles * 256));
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}).ok());
        REQUIRE(engine.load(s).ok());
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, kCycles, 256);
        else
            backend.run(kCycles);
        CHECK(engine.anticipationMisses() == 0);
        CHECK(engine.processErrors() == 0);
        CHECK(engine.transport().sample == kCycles * 256);
        engine.stop();
        CHECK(engine.lastStopOnAudioThread());
        const auto& cap = backend.capture();
        double live = 0;
        for (std::size_t i = 60000; i < 70000; ++i)
            live = std::max(live, std::fabs(double(cap.channels[1][i])));
        CHECK(live > 0.05);  // the ramp, through the vocal chain
        if (!options.anticipative) {
            reference = cap;
        } else {
            CHECK(cap.channels[0] == reference.channels[0]);
            CHECK(cap.channels[1] == reference.channels[1]);
        }
    }
}

TEST_CASE("anticipative monitoring: heard while stopped; arming mid-play hands the plugin over" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(rampInput(96000));
    Engine engine(pool, media, {.workers = 2});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}).ok());
    REQUIRE(engine.load(withVocal(true)).ok());
    REQUIRE(engine.awaitPlan());
    backend.run(20);  // stopped: the vocal is heard, nothing else plays
    const auto& cap = backend.capture();
    double stopped = 0;
    for (std::size_t i = 2560; i < 5120; ++i)
        stopped = std::max(stopped, std::fabs(double(cap.channels[0][i])));
    CHECK(stopped > 0.05);
    CHECK(engine.transport().sample == 0);
    // Play, then turn monitoring off and on again while playing: the gain
    // plugin moves between the callback and the renderer.
    engine.play();
    runAhead(engine, backend, 40, 256);
    for (bool monitored : {false, true, false, true}) {
        REQUIRE(engine.load(withVocal(monitored)).ok());
        runAhead(engine, backend, 60, 256);
        engine.service();
    }
    CHECK(engine.processErrors() == 0);
    CHECK(engine.transport().sample == (40 + 4 * 60) * 256);
    engine.stop();
    CHECK(engine.lastStopOnAudioThread());
}
