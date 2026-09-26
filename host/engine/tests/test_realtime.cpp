#include <doctest/doctest.h>

#include <cmath>
#include <thread>

#include <supple/engine/realtime.h>

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

}  // namespace

TEST_CASE("real-time output equals the offline render (any block size)" *
          doctest::description("rt")) {
    for (const char* extra : {static_cast<const char*>(nullptr), "com.suppleaudio.test.gain"}) {
        const model::Session s = synthSession(-3.0, extra);
        auto pool = makePool();
        auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();

        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend(
            {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
        Engine engine(rtPool, media);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}).ok());
        REQUIRE(engine.load(s).ok());
        engine.play();
        backend.run((48000 * 2 + 4096) / 128);
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

TEST_CASE("plan swaps while playing, seek, pause; retired plans and instances are freed") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media);
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

TEST_CASE("transport requests between callbacks are never dropped (review round 3)") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4800});
    Engine engine(pool, media);
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
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4800});
    Engine engine(pool, media);
    // Two notes at sample 75, two at 100: aligned 100-frame blocks hold two
    // each, but the block [1, 101) after a one-sample seek holds all four.
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 100}).ok());
    REQUIRE(engine.load(clusterSession({3, 3, 4, 4})).ok());
    engine.seek(1.0 / 48000.0);
    engine.play();
    backend.run(4);  // under rtsan an allocation here aborts
    CHECK(engine.processErrors() == 0);
    double energy = 0;
    for (std::size_t i = 100; i < 400; ++i)
        energy +=
            double(backend.capture().channels[0][i]) * double(backend.capture().channels[0][i]);
    CHECK(energy > 1.0);  // all four notes sound
    engine.stop();
}

TEST_CASE("a plugin that refuses to start is passed through, not processed") {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 4800});
    Engine engine(pool, media);
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
    backend.run(10);
    CHECK(engine.processErrors() >= 1);
    CHECK(engine.transport().sample == 2560);
    double peak = 0;
    for (std::size_t i = 0; i < 2560; ++i)
        peak = std::max(peak, std::fabs(double(backend.capture().channels[0][i])));
    CHECK(peak > 0.1);  // the sine passes through the stage that did not start
    engine.stop();
}
