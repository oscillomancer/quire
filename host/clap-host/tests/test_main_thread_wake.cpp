// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <cerrno>
#include <chrono>
#include <limits>
#include <thread>

#include "../src/main_thread_wake.h"

using namespace supple::host;

TEST_CASE("bridge timeout is infinite without timers and follows the nearest deadline") {
    using namespace std::chrono;
    const steady_clock::time_point now{seconds(100)};
    int timeout = -1;
    for (int due : {-1, -1}) timeout = nearestTimeout(timeout, due);
    CHECK(timeout == -1);
    timeout = nearestTimeout(timeout, timerTimeout(now + milliseconds(150), now));
    CHECK(timeout == 150);  // no fixed 10 ms cap
    timeout = nearestTimeout(timeout, timerTimeout(now + milliseconds(20), now));
    CHECK(timeout == 20);
    CHECK(nearestTimeout(timeout, -1) == 20);
    CHECK(timerTimeout(now, now) == 0);
    CHECK(timerTimeout(now - seconds(1), now) == 0);
    CHECK(timerTimeout(now + microseconds(1), now) == 1);
    CHECK(timerTimeout(now + milliseconds(20) + microseconds(1), now) == 21);
    CHECK(timerTimeout(now + milliseconds(UINT32_MAX), now) == std::numeric_limits<int>::max());
}

#include "../src/bridge_notifications.h"

TEST_CASE("bridge parent publications coalesce and ACK rearms without consuming work") {
    supple::host::bridge::NotificationLatch latch;
    CHECK_FALSE(latch.announce());
    latch.publish();
    CHECK(latch.announce());
    for (int i = 0; i < 100; ++i) latch.publish();
    CHECK_FALSE(latch.announce());
    const auto retained = latch.published;
    latch.acknowledge();  // these publications are in the subsequent snapshots
    CHECK(latch.published == retained);
    CHECK_FALSE(latch.announce());  // old retained work is not another publication
    latch.publish();                // producer arriving during the snapshot pulls
    CHECK(latch.announce());
    CHECK_FALSE(latch.announce());
    latch.acknowledge();
    CHECK_FALSE(latch.announce());  // queries, repaint-only timers and ACK do not publish
}

TEST_CASE("bridge snapshot caches preserve bounded outputs and ordered rescan clears") {
    using namespace supple::host::bridge;
    Instance::Notifications notes, first, next;
    first.paramsClear.push_back({7, CLAP_PARAM_CLEAR_AUTOMATIONS});
    REQUIRE(mergeNotifications(notes, first));
    next.paramsRescan = true;
    next.paramsRescanFlags = CLAP_PARAM_RESCAN_ALL;
    next.paramsClear.push_back({8, CLAP_PARAM_CLEAR_MODULATIONS});
    REQUIRE(mergeNotifications(notes, next));
    REQUIRE(notes.paramsClear.size() == 1);
    CHECK(notes.paramsClear.front().id == 8);
    CHECK(hasNotifications(notes));
    Instance::IdleOutputs pending, output;
    output.events.resize(Instance::IdleOutputs::kCapacity);
    output.events.front().value = 1;
    output.events.back().value = 2;
    mergeIdleOutputs(pending, output);
    output.events.resize(2);
    output.dropped = 3;
    mergeIdleOutputs(pending, output);
    CHECK(pending.events.size() == Instance::IdleOutputs::kCapacity);
    CHECK(pending.events.front().value == 1);
    CHECK(pending.events.back().value == 2);
    CHECK(pending.dropped == 5);
    Instance::EditorRequests editor;
    mergeEditorRequests(editor, {.resize = true, .width = 300, .height = 200});
    mergeEditorRequests(editor, {.resize = true, .show = true, .width = 500, .height = 400});
    CHECK(editor.resize);
    CHECK(editor.show);
    CHECK(editor.width == 500);
}

TEST_CASE("bridge audio quit cannot lose a wake before or during indefinite parking") {
    using namespace supple::host::bridge;
    Header header;
    SharedSignals signals;
    REQUIRE(signals.create());
    std::atomic<bool> quit{false};
    const auto last = header.request.load();
    requestAudioQuit(quit, header.request, signals);
    CHECK(quit.load(std::memory_order_acquire));
    CHECK(header.request.load(std::memory_order_acquire) != last);
    const int waited = signals.wait(header.request, last, -1), waitError = errno;
    CHECK(waited == -1);
    CHECK(waitError == EAGAIN);  // no blocking syscall when stop won before parking
    for (int cycle = 0; cycle < 50; ++cycle) {
        quit.store(false);
        std::atomic<bool> armed{false};
        const auto seen = header.request.load();
        std::thread audio([&] {
            armed.store(true, std::memory_order_release);
            armed.notify_one();
            while (!quit.load(std::memory_order_acquire)) signals.wait(header.request, seen, -1);
        });
        while (!armed.load(std::memory_order_acquire)) armed.wait(false);
        requestAudioQuit(quit, header.request, signals);
        audio.join();  // no timer, audio command, or elapsed-time assertion helps departure
        CHECK(quit.load());
    }
}
