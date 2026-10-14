// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <unistd.h>
#include "fake_jack.h"
#include "measurements.h"
#include "session.h"
#include "supple/audio/audio_file.h"
#include "supple/base/files.h"
#include "supple/engine/jack.h"

using namespace supple;
using namespace supple::engine;

TEST_CASE("capacity session uses command defaults, stock chain topology and notation loop") {
    auto text = readTextFile(BENCH_RECIPE);
    REQUIRE(text);
    auto parsed = json::parse(*text);
    REQUIRE(parsed);
    auto recipe = *parsed;
    // Keep builder tests independent of stock artifacts and their DSP cost.
    recipe["chain"] = json::parse(R"([{"plugin":"com.suppleaudio.test.gain"},
        {"plugin":"com.suppleaudio.test.gain"},{"plugin":"com.suppleaudio.test.gain"}])")
                          .value();
    recipe["master"] = json::parse(R"({"plugin":"com.suppleaudio.test.gain"})").value();
    struct Workdir {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("supple-capacity-test-" + std::to_string(getpid()));
        ~Workdir() { std::filesystem::remove_all(path); }
    } workdir;
    auto app = EngineApp::create({.pluginPath = {FIXTURE_CLAP_PATH}, .workdir = workdir.path});
    REQUIRE(app);
    CHECK_FALSE(bench::buildSession(**app, recipe, 0));
    REQUIRE(bench::buildSession(**app, recipe, 2));
    const auto& s = (*app)->head();
    CHECK(s.tracks.size() == 4);
    CHECK(s.fx.size() == 10);
    CHECK(s.items.size() == 2);
    CHECK(s.tracks.find(model::kMaster)->chain.size() == 1);
    int monitored = 0;
    for (const auto& track : s.tracks) {
        if (track.id == model::kMaster) continue;
        CHECK(track.value->parent == model::kMaster);
        CHECK(track.value->parentSend.enabled);
        CHECK(track.value->chain.size() == 3);
        if (track.value->record.armed) {
            ++monitored;
            CHECK(track.value->record.monitor == model::Monitor::On);
            REQUIRE(track.value->record.input);
            CHECK(s.routes.find(*track.value->record.input) != nullptr);
        }
    }
    CHECK(monitored == 1);
    CHECK((*app)->loop().enabled);
    CHECK((*app)->loop().start == 0);
    CHECK((*app)->loop().end == 8);
    for (const auto& source : s.midi) {
        CHECK(source.value->sequence.length == 16 * 960);
        CHECK(source.value->sequence.notes.size() == 32);
    }
    auto exported = bench::exportSession(**app);
    REQUIRE(exported);
    CHECK((*exported)["tracks"].size() == 3);
    CHECK((*exported)["master"].size() == 1);
    auto again = bench::exportSession(**app);
    REQUIRE(again);
    CHECK(json::writeCanonical(*exported) == json::writeCanonical(*again));
}

TEST_CASE("bench probe bounds storage and reports nearest-rank durations separately from xruns") {
    BenchProbe probe(1000);
    {
        BenchProbe::Timer timer(&probe, true, 128);  // disabled: no sample or clock read
    }
    CHECK(probe.callback.samples().empty());
    auto collect = [&]() SUPPLE_RT {
        for (int i = 1000; i > 0; --i) probe.callback.append(i, 48);
        probe.callback.append(9999, 48);  // dropped, not included in the distribution
        probe.observeAhead(7);            // disabled
        probe.recording = true;
        probe.observeAhead(512);
        probe.observeAhead(128);
        probe.observeAhead(-1);  // underrun clamps to zero
    };
    collect();
    CHECK(probe.aheadObservations == 3);
    CHECK(probe.aheadLowWater == 0);
    auto stats = bench::summarize(probe.callback, 48000);
    CHECK(stats["samples"].asInt() == 1000);
    CHECK(stats["dropped"].asInt() == 1);
    CHECK(stats["p50_us"].asDouble() == 500);
    CHECK(stats["p99_us"].asDouble() == 990);
    CHECK(stats["p999_us"].asDouble() == 999);
    CHECK(stats["max_us"].asDouble() == 1000);
    CHECK(stats["mean_us"].asDouble() == 500.5);
    CHECK(stats["over_budget"].asInt() == 0);  // equality is not over budget
    CHECK(bench::summarize(probe.renderJob, 48000)["p50_us"].isNull());
    BenchProbe tiny(1);
    tiny.callback.append(1001, 48);
    CHECK(bench::summarize(tiny.callback, 48000)["over_budget"].asInt() == 1);
}

TEST_CASE("bench effect fixture uses requested identity, live state and serial chain length") {
    struct Workdir {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("supple-effect-test-" + std::to_string(getpid()));
        ~Workdir() { std::filesystem::remove_all(path); }
    } workdir;
    std::filesystem::create_directories(workdir.path);
    audio::AudioData signal{48000,
                            std::vector<std::vector<float>>(2, std::vector<float>(192000, .1f))};
    const auto input = workdir.path / "input.wav";
    REQUIRE(audio::writeWav(input, signal));
    auto app = EngineApp::create({.pluginPath = {FIXTURE_CLAP_PATH}, .workdir = workdir.path});
    REQUIRE(app);
    REQUIRE(bench::buildEffectSession(**app, "com.suppleaudio.test.gain", input, 2, true, 3));
    const auto& s = (*app)->head();
    CHECK(s.fx.size() == 6);
    CHECK(s.items.size() == 2);
    CHECK((*app)->loop().end == 4);
    for (const auto& t : s.tracks) {
        if (t.id == model::kMaster) continue;
        CHECK(t.value->record.armed);
        CHECK(t.value->record.monitor == model::Monitor::On);
        CHECK(t.value->volumeDb == -18);
        CHECK(t.value->chain.size() == 3);
    }
    CHECK_FALSE(bench::buildEffectSession(**app, "missing", input, 0, false, 1));
    CHECK_FALSE(bench::buildEffectSession(**app, "missing", input, 1, false, 0));
}

TEST_CASE("bench stops sampling when a service tick retires the engine after device loss") {
    auto& fake = fakeJack();
    fake.reset();
    JackBackend backend({.library = FAKE_JACK_PATH, .connectOutputs = false});
    struct Workdir {
        std::filesystem::path path = std::filesystem::temp_directory_path() /
                                     ("supple-bench-restart-" + std::to_string(getpid()));
        ~Workdir() { std::filesystem::remove_all(path); }
    } workdir;
    auto app = EngineApp::create({.pluginPath = {FIXTURE_CLAP_PATH}, .workdir = workdir.path});
    REQUIRE(app);
    REQUIRE((*app)->startEngine(backend, {.sampleRate = 48000, .blockSize = 128}));
    const auto* expected = (*app)->engine();
    REQUIRE(expected != nullptr);
    CHECK(bench::serviceTick(**app, expected));
    fake.shutdown();
    CHECK_FALSE(bench::serviceTick(**app, expected));
    CHECK((*app)->engine() == nullptr);
    (*app)->stopEngine();  // do not let another tick start a replacement engine
    fake.reset();
}
