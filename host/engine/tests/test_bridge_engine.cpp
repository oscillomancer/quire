// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/engine/offline.h>
#include <algorithm>
#include <array>

using namespace supple;
using namespace supple::engine;

TEST_CASE("engine bridge failures retain scope, structured errors and restart ownership") {
    host::setBridgeExecutable(BRIDGE_PATH);
    for (double mode : {1.0, 2.0}) {
        CAPTURE(mode);
        model::EditTxn edit(model::Session::create({.console9 = false}), 1);
        model::Track track;
        track.name = "Bridge fault";
        track.parent = model::kMaster;
        const auto trackId = edit.create(track);
        edit.modify<model::Track>(model::kMaster,
                                  [&](auto& master) { master.children.push_back(trackId); });
        std::array<model::Id, 3> ids;
        for (std::size_t i = 0; i < ids.size(); ++i) {
            model::FxInstance fx;
            fx.plugin.id = i == 1 ? "com.suppleaudio.test.crash" : "com.suppleaudio.test.gain";
            fx.isolation = i == 2 ? model::Isolation::Dedicated : model::Isolation::SharedBridge;
            ids[i] = edit.create(fx);
            edit.modify<model::Track>(trackId, [&](auto& item) { item.chain.push_back(ids[i]); });
        }
        auto committed = edit.commit();
        REQUIRE(committed);
        const auto& session = *committed;
        InstancePool pool([](const model::PluginRef& ref) -> Result<host::PluginInfo> {
            const std::filesystem::path fixture = FIXTURE_CLAP_PATH;
            host::ScanOptions options;
            options.forceInProcess = true;
            auto plugins = host::scan(std::span(&fixture, 1), options);
            if (!plugins) return plugins.error();
            for (const auto& plugin : *plugins)
                if (plugin.id == ref.id) return plugin;
            return Error("engine.plugin_missing", "bridge fixture not found");
        });
        std::array<host::Instance*, 3> instances{};
        for (std::size_t i = 0; i < ids.size(); ++i) {
            auto loaded = pool.get(ids[i], *session.fx.find(ids[i]), session);
            REQUIRE_MESSAGE(loaded, (loaded ? "" : loaded.error().toString()));
            instances[i] = *loaded;
            REQUIRE(instances[i]->isolated());
            REQUIRE(instances[i]->activate(48000, 1, 64));
            REQUIRE(instances[i]->startProcessing());
        }
        const auto sharedPid = instances[0]->bridgePid();
        const auto dedicatedPid = instances[2]->bridgePid();
        CHECK(sharedPid == instances[1]->bridgePid());
        CHECK(sharedPid != dedicatedPid);
        std::array<float, 64> input{}, left{}, right{};
        input.fill(0.25f);
        const float* inputs[]{input.data(), input.data()};
        float* outputs[]{left.data(), right.data()};
        host::ProcessBlock block;
        block.inputs = inputs;
        block.outputs = outputs;
        block.inputChannels = block.outputChannels = 2;
        block.frames = 64;
        REQUIRE(instances[1]->setParam(1, mode));
        REQUIRE(instances[1]->process(block));
        CHECK(instances[0]->failed());
        CHECK(instances[1]->failed());
        CHECK_FALSE(instances[2]->failed());
        CHECK(std::all_of(left.begin(), left.end(), [](float value) { return value == 0; }));
        if (mode == 2)
            CHECK(instances[1]->failure().find("stopped responding") != std::string::npos);
        const auto failures = pool.failedErrors();
        REQUIRE(failures.size() == 2);
        for (const auto& [id, error] : failures) {
            CHECK((id == ids[0] || id == ids[1]));
            CHECK(error.code == "engine.bridge_failed");
            CHECK(error.path == "/fx/" + id.str());
            CHECK_FALSE(error.hint.empty());
        }
        REQUIRE(instances[2]->process(block));
        CHECK(left == input);
        for (auto* instance : instances) {
            instance->stopProcessing();
            instance->deactivate();
        }
        for (std::size_t i = 0; i < 2; ++i) pool.restart(ids[i]);
        for (std::size_t i = 0; i < 2; ++i) {
            auto restarted = pool.get(ids[i], *session.fx.find(ids[i]), session);
            REQUIRE(restarted);
            CHECK_FALSE((*restarted)->failed());
            CHECK((*restarted)->bridgePid() != sharedPid);
        }
        CHECK(pool.find(ids[0])->bridgePid() == pool.find(ids[1])->bridgePid());
        CHECK(pool.find(ids[2])->bridgePid() == dedicatedPid);
        CHECK(pool.failedErrors().empty());
    }
}
