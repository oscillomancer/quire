// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include "supple/engine/offline.h"

using namespace supple;
using namespace supple::engine;

TEST_CASE("undo to an FX without saved settings restores its factory state") {
    const std::filesystem::path fixture = FIXTURE_CLAP_PATH;
    const auto plugins = host::scan(std::span(&fixture, 1));
    REQUIRE(plugins);
    InstancePool pool([&](const model::PluginRef& ref) -> Result<host::PluginInfo> {
        for (const auto& plugin : *plugins)
            if (plugin.id == ref.id) return plugin;
        return Error("engine.plugin_missing", ref.id);
    });
    const auto session = model::Session::create();
    const model::Id id{model::Kind::Fx, 1};
    model::FxInstance original;

    SUBCASE("semantic patch") {
        original.plugin.id = "com.suppleaudio.test.gain";
        auto initial = pool.get(id, original, session);
        REQUIRE(initial);
        const auto factory = (*initial)->paramValue(0);
        REQUIRE(factory);
        auto changed = original;
        changed.patch =
            json::parse(R"({"schema":"supple.test-gain-patch/1.0","params":{"gain":-12}})").value();
        auto edited = pool.get(id, changed, session);
        REQUIRE(edited);
        CHECK((*edited)->paramValue(0).value() != *factory);
        auto restored = pool.get(id, original, session);
        REQUIRE(restored);
        CHECK((*restored)->paramValue(0).value() == *factory);
        CHECK(pool.get(id, original, session).value() == *restored);
    }

    SUBCASE("opaque state while the old instance is processing") {
        original.plugin.id = "com.suppleaudio.test.edge";
        auto initial = pool.get(id, original, session);
        REQUIRE(initial);
        const auto factory = (*initial)->tail();
        auto changed = original;
        changed.state = "sha256:changed";
        auto saved = session;
        saved.blobs[*changed.state] =
            std::make_shared<const model::Blob>(model::Blob{{0, 7, 0, 0}});
        auto edited = pool.get(id, changed, saved);
        REQUIRE(edited);
        CHECK((*edited)->tail() == 7);
        REQUIRE((*edited)->activate(48000, 1, 256));
        REQUIRE((*edited)->startProcessing());
        auto restored = pool.get(id, original, session);
        REQUIRE(restored);
        CHECK((*restored)->tail() == factory);
        CHECK(*restored != *edited);
        CHECK(pool.retired() == 1);
        CHECK((*edited)->processing());
        CHECK((*edited)->tail() == 7);
        (*edited)->stopProcessing();
    }
}
