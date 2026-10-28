// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <condition_variable>
#include <mutex>
#include "supple/base/files.h"
#include "supple/json/parse.h"
#include "test_support.h"
#include "ui_persistence.h"
using namespace supple;

TEST_CASE("UI sidecar lookup errors leave the file protected without throwing") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-ui-lookup-" + std::to_string(testProcessId()));
    std::filesystem::create_directories(dir);
    // A sidecar name can exceed the filesystem's component limit even when
    // the project name fits (250 bytes plus .json is 255; .ui.json is 258).
    const auto project = dir / (std::string(250, 'x') + ".json");
    std::error_code ec;
    (void)std::filesystem::exists(gui::UiPersistence::fileFor(project), ec);
    REQUIRE(ec);
    {
        gui::UiPersistence p;
        CHECK_NOTHROW(p.tick(project));
        CHECK_FALSE(p.nextDeadline());
        CHECK(p.flush(project));  // protected, so no write is attempted
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("UI state is saved beside the project and restored when it is opened") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-ui-state-" + std::to_string(testProcessId()));
    std::filesystem::create_directories(dir);
    const auto project = dir / "song.json";
    double zoom = 40;
    bool panel = false;
    auto make = [&](gui::UiPersistence& p) {
        p.add(
            "view",
            [&] {
                json::Value v = json::Value::object();
                v["zoom"] = zoom;
                return v;
            },
            [&](const json::Value& v) { zoom = v.find("zoom")->asDouble(); });
        p.add(
            "panels",
            [&] {
                json::Value v = json::Value::object();
                v["agent"] = panel;
                return v;
            },
            [&](const json::Value& v) { panel = v.find("agent")->asBool(); });
    };
    {
        gui::UiPersistence p;
        make(p);
        p.tick(project);  // nothing on disk yet
        zoom = 160;
        panel = true;
        REQUIRE(p.flush(project));
        CHECK(std::filesystem::exists(dir / "song.ui.json"));
        p.tick({});  // an unsaved project writes nothing
    }
    zoom = 1;
    panel = false;
    {
        gui::UiPersistence p;
        make(p);
        p.tick(project);  // opening the project restores its UI state
        CHECK(zoom == 160);
        CHECK(panel);
    }
    std::filesystem::remove_all(dir);
}

TEST_CASE("UI state written in the background never overtakes a later flush") {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-ui-async-" + std::to_string(testProcessId()));
    std::filesystem::create_directories(dir);
    const auto project = dir / "song.json";
    double zoom = 40;
    {
        gui::UiPersistence p;
        p.add(
            "view",
            [&] {
                json::Value v = json::Value::object();
                v["zoom"] = zoom;
                return v;
            },
            [&](const json::Value& v) { zoom = v.find("zoom")->asDouble(); });
        p.tick(project);
        zoom = 80;
        // Run the armed deadline explicitly: no wall-clock pass/fail decision.
        REQUIRE(p.nextDeadline());
        p.tick(project, 0, *p.nextDeadline());
        zoom = 120;
        REQUIRE(p.flush(project));  // after the pending write: the newest state wins
    }
    auto bytes = readFile(dir / "song.ui.json");
    REQUIRE(bytes);
    const auto doc = json::parse(std::string(bytes->begin(), bytes->end()));
    REQUIRE(doc);
    CHECK(doc->find("view")->find("zoom")->asDouble() == 120);
    std::filesystem::remove_all(dir);
}

TEST_CASE("UI sidecar compares once after input and wakes main after writing") {
    using Clock = std::chrono::steady_clock;
    const auto now = Clock::time_point(std::chrono::seconds(10));
    const auto dir = std::filesystem::temp_directory_path() /
                     ("supple-ui-deadline-" + std::to_string(testProcessId()));
    std::filesystem::create_directories(dir);
    const auto project = dir / "song.json";
    std::mutex mutex;
    std::condition_variable completed;
    bool written = false;
    int value = 0;
    {
        gui::UiPersistence p([&] {
            std::lock_guard lock(mutex);
            written = true;
            completed.notify_one();
        });
        p.add("value", [&] { return json::Value(value); }, [](const json::Value&) {});
        p.tick(project, 0, now);
        REQUIRE(p.nextDeadline());
        CHECK(*p.nextDeadline() == now + std::chrono::seconds(1));
        p.arm(now + std::chrono::milliseconds(500));
        CHECK(*p.nextDeadline() == now + std::chrono::seconds(1));
        value = 1;
        p.tick(project, 0, now + std::chrono::milliseconds(999));
        CHECK_FALSE(std::filesystem::exists(gui::UiPersistence::fileFor(project)));
        p.tick(project, 0, *p.nextDeadline());
        CHECK_FALSE(p.nextDeadline());
        {
            std::unique_lock lock(mutex);
            completed.wait(lock, [&] { return written; });
        }
        p.tick(project, 0, now + std::chrono::hours(1));
        CHECK_FALSE(p.nextDeadline());
        p.arm(now + std::chrono::seconds(2));
        p.tick(project, 0, *p.nextDeadline());  // unchanged bytes: no new deadline
        CHECK_FALSE(p.nextDeadline());
    }
    std::filesystem::remove_all(dir);
}
