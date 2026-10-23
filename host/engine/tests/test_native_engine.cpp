// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// The existing Linux equivalence contract, run with the native headless engine.
#include <doctest/doctest.h>
#include <algorithm>
#include <cmath>
#include "supple/engine/app.h"
#include "supple/engine/bench_probe.h"
#include "supple/engine/offline.h"
#include "supple/engine/realtime.h"
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
model::Session synthSession(double volumeDb = 0.0, const char* extraFx = nullptr,
                            unsigned tracks = 1,
                            model::Isolation isolation = model::Isolation::InProcess) {
    model::EditTxn t(model::Session::create({.console9 = false}), 1);
    for (unsigned channel = 0; channel < tracks; ++channel) {
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
            if (std::string_view(plugin) == "com.suppleaudio.test.gain") fx.isolation = isolation;
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
    }
    return t.commit().value();
}

double maxDiff(const std::vector<float>& a, std::size_t aFrom, const std::vector<float>& b,
               std::size_t n) {
    double worst = 0;
    for (std::size_t i = 0; i < n; ++i)
        worst = std::max(worst, std::fabs(double(a[aFrom + i]) - double(b[i])));
    return worst;
}

const EngineOptions kInCallback{.anticipative = false, .workers = 0};

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
TEST_CASE("native in-process engine output equals offline in both executors" *
          doctest::description("rt")) {
    for (const EngineOptions& options : {kInCallback, EngineOptions{.workers = 0},
                                         EngineOptions{.workers = 1}, EngineOptions{.workers = 7}})
        for (const char* extra : {static_cast<const char*>(nullptr), "com.suppleaudio.test.gain"}) {
            INFO("anticipative " << options.anticipative << ", workers " << options.workers);
            // Eight independent roots exercise every requested execution thread.
            model::Session s = synthSession(-18.0, extra, 8);
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
            BenchProbe probe(10000);
            probe.recording = true;
            auto measuredOptions = options;
            measuredOptions.benchProbe = &probe;
            Engine engine(rtPool, media, measuredOptions);
            REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
            REQUIRE(engine.load(s));
            engine.play();
            if (options.anticipative)
                runAhead(engine, backend, (48000 * 2 + 4096) / 128, 128);
            else
                backend.run((48000 * 2 + 4096) / 128);
            CHECK(engine.anticipationMisses() == 0);
            engine.stop();
            CHECK(probe.renderJob.dropped == 0);
            if (options.anticipative) {
                CHECK_FALSE(probe.renderJob.samples().empty());
                CHECK(probe.aheadObservations > 0);
                CHECK(probe.aheadLowWater >= 0);
            } else {
                CHECK(probe.renderJob.samples().empty());
                CHECK(probe.aheadObservations == 0);
            }
            const std::uint32_t L = engine.outputLatency();
            CHECK(L == (extra ? 64u : 0u));
            CHECK(engine.processErrors() == 0);
            const auto& cap = backend.capture();
            // The live output lags by the plan's latency; after that it is the offline render.
            CHECK(maxDiff(cap.channels[0], L, offline.audio.channels[0], 96000) == 0.0);
            CHECK(maxDiff(cap.channels[1], L, offline.audio.channels[1], 96000) == 0.0);
        }
}

TEST_CASE("native isolated engine output equals offline through shared and dedicated bridges") {
    host::setBridgeExecutable(BRIDGE_PATH);
    for (const auto isolation : {model::Isolation::SharedBridge, model::Isolation::Dedicated}) {
        const auto session = synthSession(-18, "com.suppleaudio.test.gain", 2, isolation);
        auto offlinePool = makePool();
        auto rendered = renderOffline(session, offlinePool, {.startSeconds = 0, .endSeconds = 0.1});
        REQUIRE_MESSAGE(rendered, (rendered ? "" : rendered.error().toString()));
        auto pool = makePool();
        MediaCache media;
        DummyBackend backend({.mode = DummyBackend::Mode::Manual, .captureFrames = 8192});
        Engine engine(pool, media, {.workers = 1});
        REQUIRE(engine.start(backend, {.sampleRate = 48000, .blockSize = 128}));
        REQUIRE(engine.load(session));
        engine.play();
        runAhead(engine, backend, 8192 / 128, 128);
        engine.stop();
        CHECK(engine.processErrors() == 0);
        CHECK(engine.outputLatency() == 64);
        CHECK(maxDiff(backend.capture().channels[0], 64, rendered->audio.channels[0], 4800) == 0);
        CHECK(maxDiff(backend.capture().channels[1], 64, rendered->audio.channels[1], 4800) == 0);
    }
}
