#include <doctest/doctest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <future>
#include <thread>

#include <unistd.h>

#include <supple/audio/audio_file.h>
#include <supple/base/sha256.h>
#include <supple/engine/jack.h>
#include <supple/engine/realtime.h>
#include <supple/host/clap_host.h>

#include "fake_jack.h"
#include "meters.h"
#include "plan.h"

using namespace supple;
using namespace supple::engine;

namespace supple::engine {
// A main-thread transport write stalled halfway (preempted after the odd
// sequence store), and the executors' view of the epoch.
struct EngineTestAccess {
    static std::uint64_t beginWrite(Engine& e) {
        const std::uint64_t seq = e.xportSeq_.load();
        e.xportSeq_.store(seq + 1);
        return seq;
    }
    static void loopAndEpoch(Engine& e, std::int64_t start, std::int64_t end) {
        e.loopStart_.store(start);
        e.loopEnd_.store(end);
        e.loopOn_.store(true);
        e.epoch_.fetch_add(1);
    }
    static void endWrite(Engine& e, std::uint64_t seq) { e.xportSeq_.store(seq + 2); }
    static std::uint64_t epoch(const Engine& e) { return e.epoch_.load(); }
    static std::uint64_t rtEpoch(const Engine& e) { return e.rtEpoch_; }
    static LoopRange rtLoop(const Engine& e) { return e.loopC_; }
    static std::uint64_t rendererEpoch(const Engine& e) { return e.epochDone_.load(); }
    static LoopRange rendererLoop(const Engine& e) { return e.loopR_; }  // after rendererEpoch
};
}  // namespace supple::engine
using model::Id;
using model::kMaster;

namespace {

const std::filesystem::path kFixture = FIXTURE_CLAP_PATH;
const std::filesystem::path kVst3Fixture = FIXTURE_VST3_PATH;

InstancePool makePool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        auto list = host::scan(std::span<const std::filesystem::path>(&kFixture, 1));
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

InstancePool makeVstPool() {
    return InstancePool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        const std::array paths{kFixture, kVst3Fixture};
        auto list = host::scan(paths);
        if (!list) return std::move(list).error();
        for (auto& p : *list)
            if (p.id == ref.id) return p;
        return Error("engine.plugin_missing", "plugin " + ref.id + " not found");
    });
}

// A track playing a sine instrument: notes on beats 0, 1, 2, 3 (quarter notes).
model::Session synthSession(double volumeDb = 0.0, const char* extraFx = nullptr) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
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
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
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
            model::Session s = synthSession(-3.0, extra);
            model::EditTxn mode(s, 2);
            auto project = mode.project();
            project.console9 = true;
            project.seed = 24681357;
            mode.setProject(project);
            s = mode.commit().value();
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

TEST_CASE("a stretched take: real-time output equals the offline render" *
          doctest::description("rt")) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-rt-stretch-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(48000));
    for (std::size_t i = 0; i < 48000; ++i) {
        const double s = 0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000.0);
        tone.channels[0][i] = static_cast<float>(s);
        tone.channels[1][i] = static_cast<float>(s);
    }
    REQUIRE(audio::writeWav(dir / "media" / "tone.wav", tone).ok());

    // One audio item, half speed with the pitch preserved, 2 s long: the
    // stretcher builds this variant on the main thread when the plan loads.
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    model::Track tr;
    tr.name = "Audio";
    tr.parent = kMaster;
    const Id track = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
    const Id md = t.create(model::MediaSource{"tone.wav", "", "wav", 48000, 2, 48000});
    model::Take take;
    take.source = md;
    take.playrate = 0.5;
    take.preservePitch = true;
    take.pitchMode = "signalsmith";
    const Id tk = t.create(take);
    model::Item item;
    item.track = track;
    item.length = 4.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    const model::Session session = t.commit().value();

    auto pool = makePool();
    auto offline =
        renderOffline(session, pool, {.startSeconds = 0, .endSeconds = 2.0, .projectDir = dir});
    REQUIRE(offline);
    REQUIRE(offline->warnings.empty());
    double offlinePeak = 0;
    for (std::size_t i = 0; i < 96000; ++i)
        offlinePeak = std::max(offlinePeak, std::fabs(double(offline->audio.channels[0][i])));
    CHECK(offlinePeak > 0.3 * (0.472135955 / 0.70710678118654752));  // a stretched, audible take

    auto rtPool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
    Engine engine(rtPool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}).ok());
    REQUIRE(engine.load(session, dir).ok());
    engine.play();
    backend.run((48000 * 2 + 4096) / 128);
    engine.stop();
    const std::uint32_t L = engine.outputLatency();
    CHECK(engine.processErrors() == 0);
    CHECK(maxDiff(backend.capture().channels[0], L, offline->audio.channels[0], 96000) == 0.0);
    CHECK(maxDiff(backend.capture().channels[1], L, offline->audio.channels[1], 96000) == 0.0);
    std::filesystem::remove_all(dir);
}

TEST_CASE("VST3 effect in chain: real-time output equals offline" * doctest::description("rt")) {
    auto found = host::scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    const auto it = std::find_if(found->begin(), found->end(), [](const host::PluginInfo& p) {
        return p.name == "Test VST3 Gain";
    });
    REQUIRE(it != found->end());
    const model::Session session = synthSession(-3.0, it->id.c_str());
    auto offlinePool = makeVstPool();
    auto offline = renderOffline(session, offlinePool, {.startSeconds = 0, .endSeconds = 2.0});
    REQUIRE_MESSAGE(offline, (offline ? "" : offline.error().toString()));
    auto rtPool = makeVstPool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
    Engine engine(rtPool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}).ok());
    REQUIRE(engine.load(session).ok());
    engine.play();
    backend.run((48000 * 2 + 4096) / 128);
    engine.stop();
    CHECK(engine.processErrors() == 0);
    const auto latency = engine.outputLatency();
    CHECK(maxDiff(backend.capture().channels[0], latency, offline->audio.channels[0], 96000) ==
          0.0);
    CHECK(maxDiff(backend.capture().channels[1], latency, offline->audio.channels[1], 96000) ==
          0.0);
}

TEST_CASE("an automated VST3 parameter renders real time = offline" * doctest::description("rt")) {
    // The plug-in's parameter is automated in the plain domain every command
    // uses; the engine converts the lane into the normalized domain at compile
    // time (Instance::paramsNormalized), so the audio thread sends normalized
    // values and never calls the edit controller (the host fixture test asserts
    // that count is zero). This fixture's two domains coincide, so the check
    // that matters here is that the automation is applied and that both paths
    // agree to the sample.
    auto found = host::scan(std::span<const std::filesystem::path>(&kVst3Fixture, 1));
    REQUIRE(found);
    const auto it = std::find_if(found->begin(), found->end(), [](const host::PluginInfo& p) {
        return p.name == "Test VST3 Gain";
    });
    REQUIRE(it != found->end());
    model::Session session = synthSession(0.0, it->id.c_str());
    Id gainFx;
    bool placed = false;
    for (const auto& e : session.fx)
        if (e.value->plugin.id == it->id) {
            gainFx = e.id;
            placed = true;
        }
    REQUIRE(placed);
    {
        model::EditTxn t(session, 2);
        model::Envelope env;
        env.target = {gainFx, "Gain"};  // the fixture declares one parameter, "Gain"
        env.points = {{0.0, 0.9, model::PointShape::Linear, 0.0},
                      {2.0, 0.1, model::PointShape::Linear, 0.0}};
        t.create(env);
        session = t.commit().value();
    }
    auto offlinePool = makeVstPool();
    auto offline = renderOffline(session, offlinePool, {.startSeconds = 0, .endSeconds = 2.0});
    REQUIRE_MESSAGE(offline, (offline ? "" : offline.error().toString()));
    CHECK(offline->warnings.empty());
    auto rtPool = makeVstPool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
    Engine engine(rtPool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}).ok());
    REQUIRE(engine.load(session).ok());
    engine.play();
    backend.run((48000 * 2 + 4096) / 128);
    engine.stop();
    CHECK(engine.processErrors() == 0);
    const auto latency = engine.outputLatency();
    CHECK(maxDiff(backend.capture().channels[0], latency, offline->audio.channels[0], 96000) ==
          0.0);
    // And the envelope is what was rendered: 0.9 at the start (the lane holds
    // the last point after beat 2), 0.1 from beat 2 on.
    const auto& audio = offline->audio.channels[0];
    auto peak = [&](std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to && i < audio.size(); ++i)
            m = std::max(m, std::fabs(double(audio[i])));
        return m;
    };
    const double early = peak(4800, 9600), late = peak(72000, 76800);
    REQUIRE(early > 0.01);
    CHECK(late / early == doctest::Approx(0.1 / 0.9).epsilon(0.05));
}

TEST_CASE("a stage that leaves processing mid-ramp does not replay the ramp" *
          doctest::description("rt")) {
    // Review-14 A4. While a stage is not processed (it belongs to the other
    // executor) its applied wet level is dropped; the ramp in flight must be
    // dropped with it, or the block where it comes back applies the old ramp's
    // start (a step up to nearly full wet).
    model::Session s = synthSession(0.0, "com.suppleaudio.test.gain");
    auto pool = makePool();
    auto compiled = detail::compile(s, pool, 0.0, 256, 48000);
    REQUIRE(compiled);
    detail::Plan p = std::move(*compiled);
    REQUIRE(detail::prepare(p, s).ok());
    REQUIRE(p.nodes.size() >= 2);
    detail::Stage* stage = nullptr;
    for (detail::Stage& st : p.nodes[0].chain)
        if (st.instance && st.instance->info().id == "com.suppleaudio.test.gain") stage = &st;
    REQUIRE(stage != nullptr);
    REQUIRE(stage->instance->startProcessing());
    host::Transport transport;
    const void* other = &transport;  // an executor that does not own the instance
    stage->instance->setOwner(&p.nodes[0]);
    // A bypass ramp in flight: from 1.0 (wet) toward 0.0, 480 frames long.
    stage->wet = 0.0;
    stage->appliedWet = 1.0f;
    stage->rampFromWet = 1.0f;
    stage->rampToWet = 0.0f;
    stage->wetRampLeft = 480;
    REQUIRE(detail::processNode(p, 0, 0, 256, transport, other));  // not ours: pass through
    CHECK(static_cast<double>(stage->appliedWet) < 0.0);
    CHECK(stage->wetRampLeft == 0);  // the ramp went with the applied value
    stage->instance->setOwner(nullptr);
    REQUIRE(detail::processNode(p, 0, 0, 256, transport, nullptr));         // ours again
    CHECK(static_cast<double>(stage->appliedWet) == doctest::Approx(0.0));  // the new target
    stage->instance->stopProcessing();
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
    // The change ramps over 10 ms (480 samples) instead of stepping: over the
    // first period after the swap (184 samples at 261 Hz) the gain is still
    // above 0.8; after the ramp it is half.
    const double before = peakOf(0, 2560), after = peakOf(2560 + 480, 5120);
    CHECK(after / before == doctest::Approx(0.5).epsilon(0.01));
    CHECK(peakOf(2560, 2560 + 184) / before > 0.8);
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
        CHECK(peak >
              0.1 * (0.472135955 /
                     0.70710678118654752));  // the sine passes through the stage that did not start
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
    CHECK(peakOf(2304, 2560) > 0.1 * (0.472135955 / 0.70710678118654752));  // the first note sounds
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
    REQUIRE(engine.awaitRendererHeld());
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
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
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
    CHECK(peak(0, 5000, 10240) > 0.1 * (0.472135955 / 0.70710678118654752));
    CHECK(peak(1, 5000, 10240) > 0.1 * (0.472135955 / 0.70710678118654752));
    REQUIRE(crash->setParam(1, 1.0).ok());  // crash in its next process()
    runAhead(engine, backend, 200, 256);    // 51200 more frames
    CHECK(crash->failed());
    CHECK(pool.failed().size() == 1);
    CHECK(peak(0, 50000, 61440) >
          0.1 * (0.472135955 / 0.70710678118654752));  // the steady track plays on
    CHECK(peak(1, 50000, 61440) == 0.0);               // the crashed node is silent
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
            if (e.value->type == model::StripType::Channel) track = e.id;
        const auto dir =
            std::filesystem::temp_directory_path() / ("supple-rec-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        REQUIRE(engine.startRecording({{track, {0, 1}, dir / "take.wav", {}}}).ok());
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
        // The recorder hashed the file it closed, so the commit never reads the
        // recording back and the hash names the bytes on disk.
        CHECK(t.hash == sha256File(t.file).value());
        CHECK(t.hash.size() == 7 + 64);
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
        if (e.value->type == model::StripType::Channel) track = e.id;
    const auto dir =
        std::filesystem::temp_directory_path() / ("supple-rec-gap-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    REQUIRE(engine.startRecording({{track, {0}, dir / "take.wav", {}}}).ok());
    engine.play();
    runAhead(engine, backend, 20, 384);
    engine.holdRenderer(true);
    REQUIRE(engine.awaitRendererHeld());
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
        if (e.value->type == model::StripType::Channel) track = e.id;
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
    const double g = 0.5 * 0.472135955;
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
    model::Session s = withVocal(true);
    model::EditTxn mode(s, 10);
    auto project = mode.project();
    project.console9 = true;
    project.seed = 5723;
    mode.setProject(project);
    s = mode.commit().value();
    constexpr std::int64_t kCycles = 400;  // 102400 frames at 256
    audio::AudioData reference;
    std::optional<Id> vocalTrack;
    for (const auto& e : s.tracks)
        if (e.value->name == "Vocal") vocalTrack = e.id;
    REQUIRE(vocalTrack.has_value());
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
        // The live strip's meter reports the monitored input it has heard —
        // pushed by the callback in both modes (P5.3c).
        const auto bank = engine.meterBank();
        REQUIRE(bank != nullptr);
        const Engine::MeterClock clock = engine.meterClock();
        double vocal = 0.0;
        for (const MeterReading& r : bank->read(clock.epoch, clock.heardStream))
            if (r.track == *vocalTrack)
                vocal = std::max(
                    {vocal, static_cast<double>(r.peak[0]), static_cast<double>(r.peak[1])});
        CHECK(vocal > 0.05);
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

// --- the transport loop (ADR-0015) --------------------------------------------------------------

TEST_CASE("loop playback wraps without a gap: the output is the offline render, unrolled" *
          doctest::description("rt")) {
    // Loop 1 s .. 2 s (beats 2..4: two notes); play from 0 for 3.5 s. Blocks
    // of 256 and chunks of 1024 do not divide the loop, so wraps fall
    // mid-block and chunks are cut short at the loop end.
    constexpr std::int64_t kFrames = 168000;
    auto timelineOf = [](std::int64_t s) { return s < 96000 ? s : 48000 + (s - 96000) % 48000; };
    for (const EngineOptions& options :
         {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 3}})
        for (const char* extra : {static_cast<const char*>(nullptr), "com.suppleaudio.test.gain"}) {
            INFO("anticipative " << options.anticipative << ", workers " << options.workers
                                 << ", latency " << (extra != nullptr));
            const model::Session s = synthSession(-3.0, extra);
            auto pool = makePool();
            auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend(
                {.mode = DummyBackend::Mode::Manual, .captureFrames = kFrames + 4096});
            Engine engine(rtPool, media, options);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
            REQUIRE(engine.load(s).ok());
            engine.setLoop(1.0, 2.0, true);
            engine.play();
            const std::int64_t cycles = (kFrames + 4096) / 256;
            if (options.anticipative)
                runAhead(engine, backend, cycles, 256);
            else
                backend.run(cycles);
            CHECK(engine.anticipationMisses() == 0);
            CHECK(engine.processErrors() == 0);
            const TransportState t = engine.transport();
            CHECK(t.loop.active());
            CHECK(t.sample == timelineOf(cycles * 256));  // the listener's position wrapped
            engine.stop();
            const std::int64_t L = engine.outputLatency();
            const auto& cap = backend.capture();
            double worst = 0;
            std::int64_t at = -1;
            for (std::int64_t j = L; j < kFrames; ++j)
                for (int c = 0; c < 2; ++c) {
                    const double d = std::fabs(
                        double(cap.channels[c][static_cast<std::size_t>(j)]) -
                        double(offline.audio
                                   .channels[c][static_cast<std::size_t>(timelineOf(j - L))]));
                    if (d > worst) {
                        worst = d;
                        at = j;
                    }
                }
            INFO("worst at " << at);
            CHECK(worst == 0.0);
        }
}

TEST_CASE("a note held across the loop end is released at the wrap" * doctest::description("rt")) {
    // One note from 0 to 2 s; loop 0.5 s .. 1 s. The note started before the
    // loop, so after the first wrap nothing plays it again: silence.
    model::Session s = synthSession(0.0);
    {
        model::EditTxn t(s, 2);
        for (const auto& e : s.midi)
            t.modify<model::MidiSource>(e.id, [](model::MidiSource& m) {
                m.sequence.notes = {{0, 4 * 960, 0, 60, 100, 0}};
            });
        s = t.commit().value();
    }
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
        REQUIRE(engine.load(s).ok());
        engine.setLoop(0.5, 1.0, true);
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, 96000 / 256, 256);
        else
            backend.run(96000 / 256);
        engine.stop();
        const auto& L = backend.capture().channels[0];
        double before = 0, after = 0;
        for (std::size_t i = 30000; i < 47000; ++i)
            before = std::max(before, std::fabs(double(L[i])));
        for (std::size_t i = 49000; i < 90000; ++i)
            after = std::max(after, std::fabs(double(L[i])));
        CHECK(before > 0.1 * (0.472135955 / 0.70710678118654752));
        CHECK(after == 0.0);
    }
}

TEST_CASE("anticipative monitoring with a loop: identical to the in-callback reference" *
          doctest::description("rt")) {
    model::Session s = withVocal(true);
    model::EditTxn mode(s, 10);
    auto project = mode.project();
    project.console9 = true;
    project.seed = 5723;
    mode.setProject(project);
    s = mode.commit().value();
    constexpr std::int64_t kCycles = 400;  // 102400 frames: several passes of 0.5 .. 1.2 s
    audio::AudioData reference;
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 3}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend(rampInput(kCycles * 256));
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}).ok());
        REQUIRE(engine.load(s).ok());
        engine.setLoop(0.5, 1.2, true);
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, kCycles, 256);
        else
            backend.run(kCycles);
        CHECK(engine.anticipationMisses() == 0);
        CHECK(engine.processErrors() == 0);
        const std::int64_t played = kCycles * 256;
        CHECK(engine.transport().sample == 24000 + (played - 24000) % 33600);
        engine.stop();
        const auto& cap = backend.capture();
        if (!options.anticipative) {
            reference = cap;
        } else {
            CHECK(cap.channels[0] == reference.channels[0]);
            CHECK(cap.channels[1] == reference.channels[1]);
        }
    }
}

TEST_CASE("loop recording: one contiguous file, a run per pass, placed on the timeline" *
          doctest::description("rt")) {
    // Loop 0.5 s .. 1 s, recording from 0.25 s: the first run is heard from
    // 0.25 s, every later one from the loop start. The input counts device
    // frames, so the file shows exactly what was captured.
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual,
                              .input = [](float* const* in, std::uint32_t channels, std::uint32_t n,
                                          std::int64_t frame) {
                                  for (std::uint32_t c = 0; c < channels; ++c)
                                      for (std::uint32_t i = 0; i < n; ++i)
                                          in[c][i] = static_cast<float>(frame + i);
                              }});
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}).ok());
        const model::Session s = synthSession(0.0);
        REQUIRE(engine.load(s).ok());
        Id track;
        for (const auto& e : s.tracks)
            if (e.value->type == model::StripType::Channel) track = e.id;
        const auto dir = std::filesystem::temp_directory_path() /
                         ("supple-rec-loop-" + std::to_string(::getpid()));
        std::filesystem::remove_all(dir);
        engine.setLoop(0.5, 1.0, true);
        engine.seek(0.25);
        REQUIRE(engine.startRecording({{track, {0}, dir / "take.wav", {}}}).ok());
        engine.play();
        constexpr std::int64_t kCycles = 400;  // 102400 frames: 36000 + 24000 * 2 + 18400
        if (options.anticipative)
            runAhead(engine, backend, kCycles, 256);
        else
            backend.run(kCycles);
        auto takes = engine.stopRecording();
        REQUIRE(takes);
        const RecordedTake& t = (*takes)[0];
        CHECK(t.frames == kCycles * 256);
        CHECK(t.loop.start == 24000);
        CHECK(t.loop.end == 48000);
        REQUIRE(t.segments.size() == 4);
        const std::int64_t want[4][2] = {
            {0, 12000}, {36000, 24000}, {60000, 24000}, {84000, 24000}};
        for (int k = 0; k < 4; ++k) {
            CHECK(t.segments[static_cast<std::size_t>(k)].fileFrame == want[k][0]);
            CHECK(t.segments[static_cast<std::size_t>(k)].timeline == want[k][1]);
        }
        auto audio = audio::readAudio(t.file);
        REQUIRE(audio);
        REQUIRE(audio->frames() == static_cast<std::size_t>(t.frames));
        std::size_t breaks = 0;  // contiguous device input, nothing lost or doubled
        for (std::size_t i = 1; i < audio->frames(); ++i)
            if (audio->channels[0][i] != audio->channels[0][i - 1] + 1.0f) ++breaks;
        CHECK(breaks == 0);
        engine.stop();
        std::filesystem::remove_all(dir);
    }
}

// --- MIDI input (ADR-0016)
// ------------------------------------------------------------------------

namespace {
// A sine instrument track that monitors (and may record) every MIDI device.
model::Session keyboardSession(bool armed = false) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    model::Track tr;
    tr.name = "Keys";
    tr.parent = kMaster;
    const Id track = t.create(tr);
    t.modify<model::Track>(kMaster, [&](model::Track& m) { m.children.push_back(track); });
    model::FxInstance fx;
    fx.plugin.id = "com.suppleaudio.test.sine";
    const Id f = t.create(fx);
    model::Route in;
    in.from.midiDevice = "key";  // matches the "Keyboard" port
    in.to.track = track;
    in.midi = model::MidiRouting{1, 0};  // channel 1 only
    const Id r = t.create(in);
    t.modify<model::Track>(track, [&](model::Track& x) {
        x.chain.push_back(f);
        x.record.input = r;
        x.record.armed = armed;
        x.record.monitor = armed ? model::Monitor::Auto : model::Monitor::On;
    });
    return t.commit().value();
}

// Device MIDI at fixed device frames: {frame, status, data1, data2}.
DummyBackend::Options keyboard(std::vector<std::array<int, 4>> events, std::int64_t capture) {
    return {
        .mode = DummyBackend::Mode::Manual,
        .captureFrames = capture,
        .midiPorts = {"Keyboard", "Pads"},
        .midi = [events](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
            for (const auto& e : events)
                if (e[0] >= frame && e[0] < frame + n) {
                    MidiInputEvent m;
                    m.frame = static_cast<std::uint32_t>(e[0] - frame);
                    m.size = 3;
                    m.bytes[0] = static_cast<std::uint8_t>(e[1]);
                    m.bytes[1] = static_cast<std::uint8_t>(e[2]);
                    m.bytes[2] = static_cast<std::uint8_t>(e[3]);
                    out.push_back(m);
                }
        }};
}
}  // namespace

TEST_CASE("MIDI monitoring: a keyboard plays the instrument live, sample-accurately" *
          doctest::description("rt")) {
    // Note on at device frame 1000, off at 20000; a note on channel 2 and one
    // from the other port are filtered out by the route.
    const std::vector<std::array<int, 4>> events = {
        {1000, 0x90, 60, 100}, {5000, 0x91, 67, 100}, {20000, 0x80, 60, 0}, {25000, 0x90, 72, 100}};
    for (const bool playing : {false, true})
        for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
            INFO("playing " << playing << ", anticipative " << options.anticipative);
            auto pool = makePool();
            MediaCache media;
            auto opts = keyboard(events, 30720);
            opts.midi = [inner = opts.midi](std::vector<MidiInputEvent>& out, std::uint32_t n,
                                            std::int64_t frame) {
                inner(out, n, frame);
                for (auto& m : out)
                    if (m.bytes[1] == 72) m.port = 1;  // the note at 25000 comes from "Pads"
            };
            DummyBackend backend(opts);
            Engine engine(pool, media, options);
            REQUIRE(
                engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true})
                    .ok());
            REQUIRE(engine.load(keyboardSession()).ok());
            REQUIRE(engine.awaitPlan());  // everything is live: nothing renders ahead
            if (playing) engine.play();
            if (playing) engine.awaitTransport();
            backend.run(120);
            engine.stop();
            const auto& L = backend.capture().channels[0];
            auto peakIn = [&](std::size_t from, std::size_t to) {
                double m = 0;
                for (std::size_t i = from; i < to; ++i) m = std::max(m, std::fabs(double(L[i])));
                return m;
            };
            CHECK(peakIn(0, 1000) == 0.0);
            CHECK(peakIn(1000, 1010) > 0.0);  // starts at its frame
            CHECK(peakIn(2000, 19000) > 0.1 * (0.472135955 / 0.70710678118654752));
            CHECK(peakIn(20000, 30720) == 0.0);  // off at 20000; channel 2 and "Pads" never sound
        }
}

TEST_CASE("MIDI recording: events land on the timeline where they were heard, per loop pass" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        // Loop 0.5 s .. 1 s from 0: pass 0 is 0 .. 1 s, then passes of 0.5 s
        // (device frames 48000.. and 72000..). The same note in each pass.
        DummyBackend backend(keyboard({{30000, 0x90, 60, 90},
                                       {36000, 0x80, 60, 40},
                                       {54000, 0x90, 60, 90},
                                       {60000, 0x80, 60, 40},
                                       {78000, 0x90, 62, 90},
                                       {84000, 0x80, 62, 40},
                                       {90000, 0xb0, 1, 64}},
                                      0));
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true})
                    .ok());
        const model::Session s = keyboardSession(true);
        REQUIRE(engine.load(s).ok());
        Id track;
        for (const auto& e : s.tracks)
            if (e.value->type == model::StripType::Channel) track = e.id;
        engine.setLoop(0.5, 1.0, true);
        RecordTarget target;
        target.track = track;
        target.midi = RecordTarget::Midi{midiPortMask(engine.midiPorts(), "key"), 1, 0};
        REQUIRE(engine.startRecording({target}).ok());
        REQUIRE(engine.awaitPlan());  // everything is live: nothing renders ahead
        engine.play();
        engine.awaitTransport();
        backend.run(400);  // 102400 frames
        auto takes = engine.stopRecording();
        REQUIRE(takes);
        REQUIRE(takes->size() == 1);
        const RecordedTake& t = (*takes)[0];
        CHECK(t.isMidi);
        REQUIRE(t.midi.size() == 7);
        const std::int64_t want[7][2] = {{30000, 0}, {36000, 0}, {30000, 1}, {36000, 1},
                                         {30000, 2}, {36000, 2}, {42000, 2}};
        for (std::size_t i = 0; i < 7; ++i) {
            CHECK(t.midi[i].timeline == want[i][0]);
            CHECK(t.midi[i].run == static_cast<std::uint32_t>(want[i][1]));
        }
        engine.stop();
    }
}

TEST_CASE("turning MIDI monitoring off while a key is held releases the note" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}}) {
        INFO("anticipative " << options.anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend(keyboard({{500, 0x90, 60, 100}}, 40960));  // held, never released
        Engine engine(pool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true})
                    .ok());
        model::Session s = keyboardSession();
        REQUIRE(engine.load(s).ok());
        REQUIRE(engine.awaitPlan());
        backend.run(40);  // 10240 frames: the note sounds
        model::EditTxn t(s, 2);
        for (const auto& e : s.tracks)
            if (e.id != kMaster)
                t.modify<model::Track>(
                    e.id, [](model::Track& x) { x.record.monitor = model::Monitor::Off; });
        REQUIRE(engine.load(t.commit().value()).ok());
        REQUIRE(engine.awaitPlan());
        backend.run(120);
        engine.stop();
        const auto& L = backend.capture().channels[0];
        double before = 0, after = 0;
        for (std::size_t i = 2000; i < 10000; ++i)
            before = std::max(before, std::fabs(double(L[i])));
        for (std::size_t i = 30000; i < 40960; ++i)
            after = std::max(after, std::fabs(double(L[i])));
        CHECK(before > 0.1 * (0.472135955 / 0.70710678118654752));
        CHECK(after == 0.0);
    }
}

TEST_CASE("a monitored MIDI route change releases held notes" * doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0}})
        for (int change = 0; change < 3; ++change) {
            INFO("anticipative " << options.anticipative << ", change " << change);
            auto pool = makePool();
            MediaCache media;
            DummyBackend backend(keyboard({{500, 0x90, 60, 100}, {18000, 0x80, 60, 0}}, 30720));
            Engine engine(pool, media, options);
            REQUIRE(
                engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true})
                    .ok());
            const model::Session s = keyboardSession();
            REQUIRE(engine.load(s).ok());
            REQUIRE(engine.awaitPlan());
            backend.run(40);  // the key is down at the edit
            model::EditTxn t(s, 2);
            for (const auto& e : s.routes)
                if (e.value->from.midiDevice)
                    t.modify<model::Route>(e.id, [&](model::Route& route) {
                        if (change == 1) route.from.midiDevice = "pads";
                        if (change == 2) route.midi->fromChannel = 2;
                    });
            REQUIRE(engine.load(t.commit().value()).ok());
            REQUIRE(engine.awaitPlan());
            backend.run(80);
            engine.stop();
            const auto& audio = backend.capture().channels[0];
            auto peak = [&](std::size_t from, std::size_t to) {
                double p = 0;
                for (std::size_t i = from; i < to; ++i)
                    p = std::max(p, std::fabs(double(audio[i])));
                return p;
            };
            CHECK(peak(2000, 10000) > 0.1 * (0.472135955 / 0.70710678118654752));
            if (change == 0)
                CHECK(peak(12000, 17000) >
                      0.1 * (0.472135955 / 0.70710678118654752));  // unchanged route carries
            CHECK(peak(21000, 30000) == 0.0);  // the old device's off is filtered after edits
        }
}

TEST_CASE("a mute while playing fades over 10 ms instead of clicking" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, kInCallback);
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
    model::Session s = synthSession(0.0);
    REQUIRE(engine.load(s).ok());
    engine.play();
    backend.run(10);  // 2560 frames
    model::EditTxn t(s, 2);
    for (const auto& e : s.tracks)
        if (e.id != kMaster) t.modify<model::Track>(e.id, [](model::Track& x) { x.mute = true; });
    REQUIRE(engine.load(t.commit().value()).ok());
    backend.run(10);
    engine.stop();
    const auto& L = backend.capture().channels[0];
    auto peakOf = [&](std::size_t from, std::size_t to) {
        double m = 0;
        for (std::size_t i = from; i < to; ++i) m = std::max(m, std::fabs(double(L[i])));
        return m;
    };
    const double before = peakOf(0, 2560);
    CHECK(peakOf(2560, 2560 + 184) > 0.5 * before);  // still fading
    CHECK(peakOf(2560 + 480, 5120) == 0.0);          // silent after 10 ms
    double jump = 0;  // no sample-to-sample step bigger than the sine's own slope
    for (std::size_t i = 2400; i < 3200; ++i)
        jump = std::max(jump, std::fabs(double(L[i]) - double(L[i - 1])));
    CHECK(jump < 0.02);
}

TEST_CASE("wet and bypass edits retarget an active ramp without a step" *
          doctest::description("rt")) {
    for (const bool bypass : {false, true}) {
        INFO("bypass " << bypass);
        model::Session s = synthSession(0.0, "com.suppleaudio.test.gain");
        Id effect;
        {
            model::EditTxn t(s, 2);
            for (const auto& e : s.fx)
                if (e.value->plugin.id == "com.suppleaudio.test.gain") {
                    effect = e.id;
                    t.modify<model::FxInstance>(e.id, [](model::FxInstance& f) {
                        f.patch =
                            json::parse(
                                R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-12,"latency":"short"}})")
                                .value();
                    });
                }
            s = t.commit().value();
        }
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 8192});
        Engine engine(pool, media, kInCallback);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
        REQUIRE(engine.load(s).ok());
        engine.play();
        backend.run(10);
        auto edit = [&](bool turnOff, std::uint64_t version) {
            model::EditTxn t(s, version);
            t.modify<model::FxInstance>(effect, [&](model::FxInstance& f) {
                if (bypass)
                    f.bypass = turnOff;
                else
                    f.wet = turnOff ? 0.0 : 100.0;
            });
            s = t.commit().value();
            REQUIRE(engine.load(s).ok());
        };
        edit(true, 3);
        backend.run(1);  // 256 samples into the 480-sample ramp
        edit(false, 4);
        backend.run(12);
        engine.stop();
        const auto& audio = backend.capture().channels[0];
        double step = 0, baselineStep = 0;
        for (std::size_t i = 2000; i < 2500; ++i)
            baselineStep =
                std::max(baselineStep, std::fabs(double(audio[i]) - double(audio[i - 1])));
        for (std::size_t i = 2700; i < 2900; ++i)
            step = std::max(step, std::fabs(double(audio[i]) - double(audio[i - 1])));
        CHECK(step < baselineStep + 0.005);
        double early = 0, final = 0;
        for (std::size_t i = 2000; i < 2500; ++i)
            early = std::max(early, std::fabs(double(audio[i])));
        for (std::size_t i = 3800; i < 4300; ++i)
            final = std::max(final, std::fabs(double(audio[i])));
        CHECK(final == doctest::Approx(early).epsilon(0.02));
    }
}

TEST_CASE("a long, dense MIDI performance records without overflow (the writer drains)" *
          doctest::description("rt")) {
    // A controller sweep every 8 frames (6000 events/s) for over a second:
    // more than one ring holds, drained while recording.
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(
        {.mode = DummyBackend::Mode::Thread,
         .paced = true,
         .midiPorts = {"Keyboard"},
         .midi = [](std::vector<MidiInputEvent>& out, std::uint32_t n, std::int64_t frame) {
             for (std::uint32_t i = 0; i < n; i += 8) {
                 MidiInputEvent m;
                 m.frame = i;
                 m.size = 3;
                 m.bytes[0] = 0xb0;
                 m.bytes[1] = 1;
                 m.bytes[2] = static_cast<std::uint8_t>((frame + i) / 8 % 128);
                 out.push_back(m);
             }
         }});
    Engine engine(pool, media, kInCallback);
    REQUIRE(
        engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .midiInputs = true}).ok());
    const model::Session s = keyboardSession(true);
    REQUIRE(engine.load(s).ok());
    Id track;
    for (const auto& e : s.tracks)
        if (e.value->type == model::StripType::Channel) track = e.id;
    RecordTarget target;
    target.track = track;
    target.midi = RecordTarget::Midi{midiPortMask(engine.midiPorts(), "*"), 0, 0};
    REQUIRE(engine.startRecording({target}).ok());
    engine.play();
    std::this_thread::sleep_for(std::chrono::milliseconds(1200));
    auto takes = engine.stopRecording();
    REQUIRE(takes);
    const RecordedTake& t = (*takes)[0];
    CHECK(t.overflowFrames == 0);
    CHECK(t.midi.size() > 4096);
    engine.stop();
}

TEST_CASE("monitoring off while playing: the renderer takes over without a gap" *
          doctest::description("rt")) {
    // A monitored instrument track (live) playing short notes every ~1025
    // frames; monitoring switches off mid-play. The renderer takes the track
    // at its render horizon, before the callback has let go of the live
    // instance: a fresh instance keeps the notes coming (it used to pass the
    // track through for the render-ahead time, ~9600 silent frames).
    model::Session s = synthSession(0.0);
    {
        model::EditTxn t(s, 2);
        for (const auto& e : s.midi)
            t.modify<model::MidiSource>(e.id, [](model::MidiSource& m) {
                m.sequence.notes.clear();
                for (std::int64_t k = 0; k * 41 < 16 * 960; ++k)
                    m.sequence.notes.push_back({k * 41, 20, 0, 69, 100, 0});
                m.sequence.length = 16 * 960;
            });
        s = t.commit().value();
    }
    const model::Session monitored = monitoring(s, model::Monitor::On);
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 96000});
    Engine engine(pool, media, EngineOptions{.workers = 0});
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}).ok());
    REQUIRE(engine.load(monitored).ok());
    REQUIRE(engine.awaitPlan());
    engine.play();
    engine.awaitTransport();
    backend.run(100);              // 25600 frames, all live (nothing renders ahead)
    REQUIRE(engine.load(s).ok());  // monitoring off: the renderer takes the graph
    REQUIRE(engine.awaitPlan());
    runAhead(engine, backend, 250, 256);
    CHECK(engine.processErrors() == 0);
    CHECK(engine.transport().sample == 350 * 256);  // no replay from the epoch's start
    engine.stop();
    const auto& L = backend.capture().channels[0];
    std::size_t longest = 0, run = 0;
    for (std::size_t i = 20000; i < 88000; ++i) {
        run = std::fabs(double(L[i])) < 1e-4 ? run + 1 : 0;
        longest = std::max(longest, run);
    }
    CHECK(longest < 3000);  // the gaps between notes (~525) and one cut note, never ~9600
}

TEST_CASE("a transport write in progress never stalls the audio callback" *
          doctest::description("rt")) {
    // The main thread preempted inside a seek/loop write: the callback must
    // not wait for it (it keeps its epoch and loop for another cycle), and
    // both executors then take the new epoch with its own loop.
    for (const bool anticipative : {true, false}) {
        CAPTURE(anticipative);
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual});
        Engine engine(pool, media, EngineOptions{.anticipative = anticipative, .workers = 0});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256}).ok());
        REQUIRE(engine.load(synthSession(0.0)).ok());
        if (anticipative) REQUIRE(engine.awaitPlan());
        engine.setLoop(0.0, 1.0, true);
        engine.play();
        engine.awaitTransport();
        backend.run(8);
        const std::uint64_t before = EngineTestAccess::epoch(engine);
        const LoopRange old = EngineTestAccess::rtLoop(engine);
        CHECK(old.end == 48000);

        const std::uint64_t seq = EngineTestAccess::beginWrite(engine);
        EngineTestAccess::loopAndEpoch(engine, 4800, 24000);  // not published yet
        auto cycles = std::async(std::launch::async, [&] { backend.run(16); });
        const bool returned = cycles.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        if (!returned) EngineTestAccess::endWrite(engine, seq);  // let it finish, then fail
        cycles.wait();
        REQUIRE(returned);
        if (anticipative) CHECK(EngineTestAccess::rtEpoch(engine) == before);
        CHECK(EngineTestAccess::rtLoop(engine).end == old.end);

        EngineTestAccess::endWrite(engine, seq);
        backend.run(1);
        const LoopRange now = EngineTestAccess::rtLoop(engine);
        CHECK(now.start == 4800);
        CHECK(now.end == 24000);
        if (anticipative) {
            CHECK(EngineTestAccess::rtEpoch(engine) == before + 1);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
            while (EngineTestAccess::rendererEpoch(engine) != before + 1 &&
                   std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            REQUIRE(EngineTestAccess::rendererEpoch(engine) == before + 1);
            CHECK(EngineTestAccess::rendererLoop(engine).start == 4800);
            CHECK(EngineTestAccess::rendererLoop(engine).end == 24000);
        }
        engine.stop();
    }
}

TEST_CASE("a keyed plug-in plays in real time as offline, rendered ahead or live (ADR-0018)" *
          doctest::description("rt")) {
    // synthSession's track (a sine and a 64-sample latent gain) keys a key mix
    // on a second track. Monitoring that track makes it live: the key then
    // crosses from the renderer to the callback through a frontier ring.
    for (const bool monitored : {false, true})
        for (const EngineOptions& options :
             {kInCallback, EngineOptions{.workers = 0}, EngineOptions{.workers = 3}}) {
            INFO("monitored " << monitored << ", anticipative " << options.anticipative
                              << ", workers " << options.workers);
            model::Session s = synthSession(-3.0, "com.suppleaudio.test.gain");
            {
                model::EditTxn t(s, 2);
                auto project = t.project();
                project.console9 = true;
                project.seed = 8901;
                t.setProject(project);
                Id source;
                for (const auto& e : s.tracks)
                    if (e.id != kMaster) source = e.id;
                model::Track tr;
                tr.name = "Keyed";
                tr.parent = kMaster;
                const Id keyed = t.create(tr);
                t.modify<model::Track>(kMaster,
                                       [&](model::Track& m) { m.children.push_back(keyed); });
                model::FxInstance fx;
                fx.plugin.id = "com.suppleaudio.test.keymix";
                const Id f = t.create(fx);
                t.modify<model::Track>(keyed, [&](model::Track& x) { x.chain.push_back(f); });
                model::Route key;
                key.from.track = source;
                key.to.fx = f;
                t.create(key);
                model::Track bus;
                bus.type = model::StripType::Buss;
                bus.parent = kMaster;
                const Id busId = t.create(bus);
                t.modify<model::Track>(kMaster, [&](auto& m) {
                    std::erase(m.children, keyed);
                    m.children.push_back(busId);
                });
                t.modify<model::Track>(keyed, [&](auto& child) { child.parent = busId; });
                t.modify<model::Track>(busId,
                                       [&](auto& group) { group.children.push_back(keyed); });
                model::Route send;
                send.from.track = source;
                send.to.track = busId;
                send.tap = model::Tap::PreFader;
                send.volumeDb = -6;
                t.create(send);
                s = t.commit().value();
            }
            if (monitored) s = monitoring(s);  // the last track: the keyed one
            auto pool = makePool();
            auto offline = renderOffline(s, pool, {.startSeconds = 0, .endSeconds = 2.0}).value();
            auto rtPool = makePool();
            MediaCache media;
            DummyBackend backend(
                {.mode = DummyBackend::Mode::Manual, .captureFrames = 48000 * 2 + 4096});
            Engine engine(rtPool, media, options);
            REQUIRE(
                engine.start(backend, {.sampleRate = 48000, .blockSize = 128, .inputs = 2}).ok());
            REQUIRE(engine.load(s).ok());
            if (options.anticipative) REQUIRE(engine.awaitPlan());
            engine.play();
            if (options.anticipative && !monitored)
                runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
            else if (options.anticipative) {
                engine.awaitTransport();
                for (int i = 0; i < (48000 * 2 + 4096) / 128; i += 16) {
                    REQUIRE(engine.awaitRendered(4096));
                    backend.run(16);
                }
            } else {
                backend.run((48000 * 2 + 4096) / 128);
            }
            engine.stop();
            CHECK(engine.processErrors() == 0);
            const std::uint32_t L = engine.outputLatency();
            const auto& cap = backend.capture();
            CHECK(maxDiff(cap.channels[0], L, offline.audio.channels[0], 96000) == 0.0);
            CHECK(maxDiff(cap.channels[1], L, offline.audio.channels[1], 96000) == 0.0);
            double energy = 0;
            for (std::size_t i = 0; i < 96000; ++i)
                energy += static_cast<double>(offline.audio.channels[0][i]) *
                          static_cast<double>(offline.audio.channels[0][i]);
            CHECK(energy > 1.0);
        }
}

TEST_CASE("multiple masters: real-time main output is independent of later roots" *
          doctest::description("rt")) {
    model::EditTxn t(synthSession(), 2);
    model::Track master;
    master.type = model::StripType::Master;
    const Id alt = t.create(master);
    auto project = t.project();
    project.masters.push_back(alt);
    t.setProject(project);
    model::FxInstance gain;
    gain.plugin.id = "com.suppleaudio.test.gain";
    gain.patch =
        json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"latency":"short"}})")
            .value();
    const Id fx = t.create(gain);
    t.modify<model::Track>(alt, [&](auto& tr) { tr.chain.push_back(fx); });
    const auto s = t.commit();
    REQUIRE(s);
    auto pool = makePool();
    auto offline = renderOffline(*s, pool, {.startSeconds = 0, .endSeconds = 0.1});
    REQUIRE(offline);
    CHECK(offline->outputLatency == 0);
    auto other = renderOffline(*s, pool, {.master = alt, .startSeconds = 0, .endSeconds = 0.1});
    REQUIRE(other);
    CHECK(other->outputLatency == 64);
    for (const auto& options : {kInCallback, EngineOptions{.workers = 3}}) {
        auto rtPool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 8192});
        Engine engine(rtPool, media, options);
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}).ok());
        REQUIRE(engine.load(*s).ok());
        engine.play();
        if (options.anticipative)
            runAhead(engine, backend, 64, 128);
        else
            backend.run(64);
        CHECK(engine.outputLatency() == 0);
        CHECK(engine.processErrors() == 0);
        engine.stop();
        CHECK(maxDiff(backend.capture().channels[0], 0, offline->audio.channels[0], 4800) == 0);
    }
}

TEST_CASE("a master key is legal and its downstream node follows the live executor") {
    model::EditTxn t(monitoring(synthSession()), 2);
    model::Track alt;
    alt.type = model::StripType::Master;
    const Id root = t.create(alt);
    auto project = t.project();
    project.masters.push_back(root);
    t.setProject(project);
    model::FxInstance keyed;
    keyed.plugin.id = "com.suppleaudio.test.keymix";
    const Id fx = t.create(keyed);
    t.modify<model::Track>(root, [&](auto& tr) { tr.chain.push_back(fx); });
    model::Route key;
    key.from.track = kMaster;
    key.to.fx = fx;
    t.create(key);
    model::Route hw;
    hw.from.track = kMaster;
    hw.to.hwChannels = std::vector<int>{1, 2};
    t.create(hw);
    auto s = t.commit();
    REQUIRE(s);
    auto pool = makePool();
    auto plan = detail::compile(*s, pool, 0, 128);
    REQUIRE(plan);
    CHECK(plan->nodes[plan->mainMaster].live);
    auto other = std::find_if(plan->nodes.begin(), plan->nodes.end(),
                              [&](const auto& n) { return n.id == root; });
    REQUIRE(other != plan->nodes.end());
    CHECK(other->live);
}

// The meters (P5.3c, engine.md §2.3): a strip's reading covers what the
// listener has *heard*. With anticipation the renderer is up to `render_ahead`
// ahead of the playhead, so its numbers must be held back until the callback
// has played them.
TEST_CASE("meters report what has been heard, not what has been rendered" *
          doctest::description("rt")) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-engine-meters-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    // A -6 dBFS sine item (amplitude 0.5) on one track, a silent track beside it.
    audio::AudioData tone;
    tone.sampleRate = 48000;
    tone.channels.assign(2, std::vector<float>(96000));
    for (std::size_t i = 0; i < tone.channels[0].size(); ++i) {
        const double s = 0.5 * std::sin(2 * 3.141592653589793 * 440.0 * double(i) / 48000.0);
        tone.channels[0][i] = static_cast<float>(s);
        tone.channels[1][i] = static_cast<float>(s);
    }
    REQUIRE(audio::writeWav(dir / "media" / "tone.wav", tone).ok());

    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    model::Track sine;
    sine.name = "Sine";
    sine.parent = kMaster;
    const Id sineTrack = t.create(sine);
    model::Track quiet;
    quiet.name = "Quiet";
    quiet.parent = kMaster;
    const Id quietTrack = t.create(quiet);
    t.modify<model::Track>(kMaster, [&](model::Track& m) {
        m.children.push_back(sineTrack);
        m.children.push_back(quietTrack);
    });
    const Id md = t.create(model::MediaSource{"tone.wav", "", "wav", 48000, 2, 96000});
    model::Take take;
    take.source = md;
    const Id tk = t.create(take);
    model::Item item;
    item.track = sineTrack;
    item.length = 2.0;
    item.takes = {tk};
    item.activeTake = tk;
    t.create(item);
    const model::Session session = t.commit().value();

    auto pool = makePool();
    MediaCache media;
    DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 48000});
    Engine engine(pool, media, EngineOptions{.workers = 0});  // anticipative
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}).ok());
    REQUIRE(engine.load(session, dir).ok());
    const auto bank = engine.meterBank();
    REQUIRE(bank != nullptr);
    const Id master = kMaster;
    // One read per call is one GUI frame: the readings cover the audio heard
    // since the previous frame, so a test must not read twice and expect both.
    struct Levels {
        double peak = -1.0, rms = -1.0;
    };
    auto frame = [&] {
        const Engine::MeterClock clock = engine.meterClock();
        return bank->read(clock.epoch, clock.heardStream);
    };
    auto levelOf = [](const std::vector<MeterReading>& readings, model::Id which) {
        Levels out;
        for (const MeterReading& r : readings)
            if (r.track == which) {
                out.peak = std::max(static_cast<double>(r.peak[0]), static_cast<double>(r.peak[1]));
                out.rms = std::max(static_cast<double>(r.rms[0]), static_cast<double>(r.rms[1]));
            }
        return out;
    };

    engine.play();
    // The renderer is 200 ms ahead of the playhead and the callback has pulled
    // nothing, so nothing has been heard yet: every meter is silent.
    REQUIRE(engine.awaitRendered(9600));
    CHECK(engine.cycles() == 0);
    const auto before = frame();
    CHECK(levelOf(before, sineTrack).peak == 0.0);
    CHECK(levelOf(before, quietTrack).peak == 0.0);
    CHECK(levelOf(before, master).peak == 0.0);

    // The callback pulls 200 ms: the sine is heard, the silent strip is not.
    runAhead(engine, backend, 9600 / 128, 128);
    CHECK(engine.cycles() >= 9600 / 128);
    const auto after = frame();
    CHECK(levelOf(after, quietTrack).peak == 0.0);
    CHECK(levelOf(after, quietTrack).rms == 0.0);
    CHECK(levelOf(after, sineTrack).peak > 0.2);
    // A steady sine: the RMS is the peak over √2, so a strip's peak and energy
    // are of the same signal (a strip's post is its encoded Channel level; the
    // master's is the decoded mix).
    CHECK(levelOf(after, sineTrack).rms * std::sqrt(2.0) ==
          doctest::Approx(levelOf(after, sineTrack).peak).epsilon(0.01));
    // What is heard: the master's meter is exactly the level an offline render
    // of the same span writes — metering never touches the signal.
    auto offline =
        renderOffline(session, pool, {.startSeconds = 0, .endSeconds = 0.2, .projectDir = dir});
    REQUIRE(offline);
    double offlinePeak = 0.0;
    for (float sample : offline->audio.channels[0])
        offlinePeak = std::max(offlinePeak, std::fabs(static_cast<double>(sample)));
    CHECK(levelOf(after, master).peak == doctest::Approx(offlinePeak).epsilon(1e-6));
    CHECK_FALSE(bank->droppedRenderer());  // the reader kept up
    engine.stop();
    std::filesystem::remove_all(dir);
}

// A monitored input is heard while the transport is stopped (nothing is
// rendered ahead of it, so there is no play position to wait for): its meter
// shows it, and the master's meter is exactly what the device was given.
TEST_CASE("meters show a monitored input while the transport is stopped" *
          doctest::description("rt")) {
    auto pool = makePool();
    MediaCache media;
    DummyBackend backend(constantInput(4096));
    Engine engine(pool, media, kInCallback);  // no anticipation: the callback meters
    REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 256, .inputs = 2}).ok());
    REQUIRE(engine.load(monitoring(synthSession(-6.0206))).ok());  // its track monitors 1-2
    backend.run(4);                                                // stopped: input only

    const auto bank = engine.meterBank();
    REQUIRE(bank != nullptr);
    const Engine::MeterClock clock = engine.meterClock();
    const auto readings = bank->read(clock.epoch, clock.heardStream);
    double monitorPeak = -1.0, masterPeak = -1.0;
    for (const MeterReading& r : readings) {
        const double peak =
            std::max(static_cast<double>(r.peak[0]), static_cast<double>(r.peak[1]));
        if (r.track == kMaster)
            masterPeak = peak;
        else
            monitorPeak = std::max(monitorPeak, peak);
    }
    CHECK(monitorPeak > 0.0);  // the monitored input reached the strip
    const auto& cap = backend.capture();
    double devicePeak = 0.0;
    for (float sample : cap.channels[0])
        devicePeak = std::max(devicePeak, std::fabs(static_cast<double>(sample)));
    CHECK(masterPeak == doctest::Approx(devicePeak).epsilon(1e-6));
    engine.stop();
}
