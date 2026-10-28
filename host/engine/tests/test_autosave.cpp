// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>

#include "../../../tests/file_platform.h"
#include "supple/base/files.h"
#include "supple/engine/app.h"

using namespace supple;
using namespace supple::engine;
namespace fs = std::filesystem;

TEST_CASE("autosave numbering preserves other files and cannot overflow") {
    struct Root {
        fs::path path =
            fs::temp_directory_path() / ("supple-autosave-" + std::to_string(test::processId()));
        Root() { fs::remove_all(path); }
        ~Root() { fs::remove_all(path); }
    } root;
    auto created = EngineApp::create({.workdir = root.path});
    REQUIRE(created);
    auto& app = **created;
    app.setFixedClock(1'000'000);
    REQUIRE(app.save("project.json"));
    const auto dir = app.autosaveDir();
    const auto now = fs::file_time_type::clock::now();
    fs::last_write_time(app.projectPath(), now - std::chrono::seconds(60));
    auto write = [&](const std::string& name) {
        REQUIRE(writeFileAtomic(dir / name, "preserved"));
        fs::last_write_time(dir / name, now);
    };
    auto save = [&] {
        auto transaction = app.beginTransaction({cmd::Origin::Type::Test, "unit", ""}, "Edit");
        REQUIRE(transaction);
        auto project = app.txn().project();
        project.name = "Changed";
        app.txn().setProject(project);
        REQUIRE(app.commitTransaction(*transaction));
        app.setFixedClock(1'060'000);
        app.tick();
    };
    SUBCASE("only numbered autosaves participate in discovery and rotation") {
        const std::vector<std::string> backups{"autosave-1-backup.json", "autosave-2.json.json",
                                               "autosave-999999999999999999999999.json"};
        for (const auto& name : backups) write(name);
        CHECK(app.newerAutosave(app.projectPath()).empty());
        for (int n = 1; n <= 6; ++n) write("autosave-" + std::to_string(n) + ".json");
        save();
        for (const auto& name : backups) {
            auto contents = readTextFile(dir / name);
            REQUIRE(contents);
            CHECK(*contents == "preserved");
        }
        CHECK_FALSE(fs::exists(dir / "autosave-1.json"));
        CHECK_FALSE(fs::exists(dir / "autosave-2.json"));
        CHECK(fs::exists(dir / "autosave-7.json"));
    }
    SUBCASE("the next index exceeds a signed 32-bit integer") {
        write("autosave-2147483647.json");
        save();
        CHECK(fs::exists(dir / "autosave-2147483648.json"));
        CHECK_FALSE(fs::exists(dir / "autosave-1.json"));
    }
    SUBCASE("an exhausted index reports a failure without writing or pruning") {
        write("autosave-18446744073709551615.json");
        save();
        CHECK_FALSE(fs::exists(dir / "autosave-1.json"));
        CHECK(app.nextServiceDelayMs() == 60'000);
        bool reported = false;
        for (const auto& [topic, value] : app.takeEvents())
            if (topic == "engine.error" && value.isString() &&
                value.asString().find("autosave.index") != std::string::npos)
                reported = true;
        CHECK(reported);
        auto contents = readTextFile(dir / "autosave-18446744073709551615.json");
        REQUIRE(contents);
        CHECK(*contents == "preserved");
    }
}
