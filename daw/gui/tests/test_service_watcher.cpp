// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <clap/clap.h>
#include <doctest/doctest.h>
#include <algorithm>
#include <cerrno>
#include <condition_variable>
#include <mutex>
#include "service_watcher.h"
using namespace supple;

TEST_CASE("GUI watches native events once until readiness is consumed and rearmed") {
    WakeEvent source;
    REQUIRE(source.waitable().valid());
    std::mutex mutex;
    std::condition_variable changed;
    unsigned wakes = 0;
    auto watcher = gui::FdWatcher::create([&] {
        std::lock_guard lock(mutex);
        ++wakes;
        changed.notify_all();
    });
    REQUIRE(watcher);
    (*watcher)->watch({{source.waitable(), CLAP_POSIX_FD_READ, 7}});
    source.notify();
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return wakes == 1; });
    }
    auto ready = (*watcher)->takeReady();
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].fd == source.waitable());
    CHECK(ready[0].owner == 7);
    CHECK(ready[0].flags == CLAP_POSIX_FD_READ);
    CHECK(source.drain());
    (*watcher)->watch({{source.waitable(), CLAP_POSIX_FD_READ, 8}});
    source.notify();
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return wakes == 2; });
    }
    ready = (*watcher)->takeReady();
    REQUIRE(ready.size() == 1);
    CHECK(ready[0].owner == 8);
    watcher->reset();       // parked worker must wake and join, without a deadline
    CHECK(source.drain());  // watcher only borrows the native source
}

TEST_CASE("GUI discards stale readiness after a registration changes during the wait") {
    WakeEvent source;
    std::mutex mutex;
    std::condition_variable changed;
    bool entered = false, release = false, waitedAgain = false;
    auto watcher = gui::FdWatcher::create(
        [] {},
        [&](std::span<WaitItem> items) {
            std::unique_lock lock(mutex);
            const auto found = std::find_if(items.begin(), items.end(), [&](const WaitItem& item) {
                return item.source == source.waitable();
            });
            if (found != items.end() && !entered) {
                entered = true;
                changed.notify_all();
                changed.wait(lock, [&] { return release; });
                found->ready = kWaitRead;
                return 1;
            }
            if (entered && release) {
                waitedAgain = true;
                changed.notify_all();
            }
            lock.unlock();
            return waitFor(items, -1);
        });
    REQUIRE(watcher);
    (*watcher)->watch({{source.waitable(), CLAP_POSIX_FD_READ, 1}});
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return entered; });
    }
    (*watcher)->watch({{source.waitable(), CLAP_POSIX_FD_READ, 2}});
    {
        std::unique_lock lock(mutex);
        release = true;
        changed.notify_all();
        changed.wait(lock, [&] { return waitedAgain; });
    }
    CHECK((*watcher)->takeReady().empty());
}

TEST_CASE("a permanent native GUI wait failure is reported once and stops the worker") {
    std::mutex mutex;
    std::condition_variable changed;
    bool woke = false;
    unsigned calls = 0;
    auto watcher = gui::FdWatcher::create(
        [&] {
            std::lock_guard lock(mutex);
            woke = true;
            changed.notify_all();
        },
        [&](std::span<WaitItem>) {
            ++calls;
            errno = EBADF;
            return -1;
        });
    REQUIRE(watcher);
    {
        std::unique_lock lock(mutex);
        changed.wait(lock, [&] { return woke; });
    }
    REQUIRE((*watcher)->error());
    CHECK((*watcher)->error()->code == "gui.editor.fds");
    watcher->reset();
    CHECK(calls == 1);
}
