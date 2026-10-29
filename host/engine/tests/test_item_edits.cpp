// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include "supple/engine/app.h"
#include "supple/model/item_edits.h"
#include "supple/model/schemas.h"

using namespace supple;
using namespace supple::engine;
namespace {
const cmd::Origin origin{cmd::Origin::Type::Test, "item-edits", {}};
struct Fixture {
    std::mutex mutex;
    std::condition_variable done;
    bool finished = false;
    std::filesystem::path dir =
        std::filesystem::temp_directory_path() /
        ("item-edits-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    cmd::Registry registry;
    std::unique_ptr<EngineApp> app;
    std::unique_ptr<cmd::Dispatcher> bus;
    Fixture() {
        AppOptions options;
        options.workdir = dir;
        options.pluginPath = {FIXTURE_CLAP_PATH};
        options.jobFinishedHook = [&] {
            std::lock_guard lock(mutex);
            finished = true;
            done.notify_all();
        };
        auto created = EngineApp::create(options, {.console9 = false});
        REQUIRE(created);
        app = std::move(*created);
        registerEngineCommands(registry);
        bus = std::make_unique<cmd::Dispatcher>(registry, *app);
        app->setupFixture(*bus, "song_with_audio");
    }
    ~Fixture() {
        bus.reset();
        app.reset();
        std::filesystem::remove_all(dir);
    }
    Result<json::Value> call(const char* command, std::string_view args = "{}") {
        return bus->execute({command, *json::parse(args), origin});
    }
    json::Value run(const char* command, std::string_view args = "{}") {
        auto result = call(command, args);
        REQUIRE_MESSAGE(result, (result ? "" : result.error().toString()));
        return *result;
    }
    json::Value finish(const json::Value& job) {
        {
            std::unique_lock lock(mutex);
            done.wait(lock, [&] { return finished; });
        }
        bus->flushJobEvents();
        auto status = bus->jobs().status(job.find("job")->asString());
        REQUIRE(status);
        REQUIRE_MESSAGE((*status)["status"].asString() == "done", json::writeCompact(*status));
        return (*status)["result"];
    }
};
}  // namespace

TEST_CASE("consolidation bakes item and take FX with silence in gaps, leaving track FX live") {
    Fixture f;
    f.run("item.set",
          R"({"item":"it_1","length":0.5,"gain_db":-3,"fade_in":0.01,"fade_out":0.02})");
    f.run("fx.add", R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain"})");
    f.run("fx.param.set", R"({"fx":"fx_1","values":{"gain":6}})");
    f.run("fx.add", R"({"take":"tk_1","plugin":"com.suppleaudio.test.gain"})");
    f.run("fx.param.set", R"({"fx":"fx_2","values":{"gain":-6}})");
    f.run("item.duplicate", R"({"item":"it_1","to":2})");
    const auto original = f.app->head();
    OfflineSettings settings{.endSeconds = 1.25,
                             .threads = 1,
                             .track = model::Id{model::Kind::Track, 1},
                             .tap = model::Tap::PreFader};
    auto full = f.app->renderSession(original, settings);
    REQUIRE(full);
    model::EditTxn source(original, original.version);
    source.modify<model::Track>({model::Kind::Track, 1}, [](auto& track) { track.chain.clear(); });
    source.remove<model::FxInstance>({model::Kind::Fx, 1});
    auto isolated = source.commit();
    REQUIRE(isolated);
    auto expected = f.app->renderSession(*isolated, settings);
    REQUIRE(expected);
    const auto result = f.finish(f.run("item.consolidate", R"({"items":["it_1","it_2"]})"));
    REQUIRE(result.find("files")->size() == 1);
    auto audio = audio::readAudio(result.find("files")->asArray()[0].asString());
    REQUIRE(audio);
    CHECK(bool(audio->channels == expected->audio.channels));
    CHECK(audio->channels[0][24000] == 0);
    REQUIRE(f.app->head().items.size() == 1);
    const auto& item = *f.app->head().items.find({model::Kind::Item, 1});
    const auto& take = *f.app->head().takes.find(*item.activeTake);
    CHECK(take.chain.empty());
    CHECK(item.fadeIn.length == 0);
    CHECK(item.gainDb == 0);
    CHECK(f.app->head().tracks.find({model::Kind::Track, 1})->chain.size() == 1);
    auto after = f.app->renderSession(f.app->head(), settings);
    REQUIRE(after);
    CHECK(bool(after->audio.channels == full->audio.channels));
    CHECK(f.app->head().version == original.version + 1);
    REQUIRE(f.app->undo());
    CHECK(f.app->head().items.size() == 2);
    REQUIRE(f.app->redo());
    CHECK(f.app->head().items.size() == 1);
    CHECK(std::filesystem::exists(result.find("files")->asArray()[0].asString()));
}

TEST_CASE("consolidation partitions mixed items by track and active kind") {
    Fixture f;
    f.run("item.add", R"({"track":"tr_1","notation":"1: C4:w"})");
    f.run("track.add");
    f.run("item.add", R"({"track":"tr_2","notation":"1: D4:w"})");
    f.run("item.add", R"({"track":"tr_1","position":4,"notation":"1: E4:w"})");
    const std::vector<model::Id> ids{{model::Kind::Item, 1},
                                     {model::Kind::Item, 2},
                                     {model::Kind::Item, 3},
                                     {model::Kind::Item, 4}};
    auto groups = model::consolidationGroups(f.app->head(), ids);
    REQUIRE(groups);
    REQUIRE(groups->size() == 3);
    CHECK((*groups)[0].items.size() == 2);
    CHECK_FALSE((*groups)[0].audio);
    CHECK((*groups)[1].items.size() == 1);
    CHECK((*groups)[1].audio);
    CHECK((*groups)[2].track == model::Id{model::Kind::Track, 2});
    auto result = f.finish(f.run("item.consolidate", R"({"items":["it_1","it_2","it_3","it_4"]})"));
    CHECK(result["items"].size() == 3);
    CHECK(f.app->head().items.size() == 3);
    CHECK(model::itemLengthBeats(f.app->head(),
                                 *f.app->head().items.find({model::Kind::Item, 2})) == 8);
}
