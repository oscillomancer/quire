#include <doctest/doctest.h>
#include <unistd.h>
#include "session.h"
#include "supple/base/files.h"

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
