// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#if defined(__linux__)
#include <unistd.h>
#endif

#include <array>
#include <cerrno>
#include <memory>
#include <thread>

#include "supple/base/main_thread_wake.h"

using namespace supple;

namespace supple {
struct MainThreadWakeTestAccess {
    static bool consumeEvent(MainThreadWake& wake) { return wake.event_.drain(); }
    struct Shutdown {
        std::atomic<int> stage{0};
        std::atomic<bool> resume{false};
        std::atomic<std::uint32_t> captured{0};
        std::atomic<unsigned> writes{0};
    };
    static std::unique_ptr<MainThreadWake> create(Shutdown& shutdown) {
        return std::unique_ptr<MainThreadWake>(new MainThreadWake(observe, &shutdown));
    }
    static void observe(void* context, MainThreadWake& wake, MainThreadWake::RelayStep step) {
        auto& shutdown = *static_cast<Shutdown*>(context);
        switch (step) {
            case MainThreadWake::RelayStep::BeforeCapture:
                if (shutdown.stage.load() == 0) {
                    shutdown.stage.store(1);
                    shutdown.stage.notify_one();
                    while (!shutdown.resume.load()) shutdown.resume.wait(false);
                }
                break;
            case MainThreadWake::RelayStep::Shutdown:
                shutdown.stage.store(2);
                shutdown.stage.notify_one();
                break;
            case MainThreadWake::RelayStep::AfterCapture:
                shutdown.captured.store(wake.sequence_.load(std::memory_order_acquire));
                // Rescue the old ordering from its otherwise indefinite next
                // wait. It still fails by reaching BeforeWrite after shutdown;
                // no timeout or elapsed-time assertion decides the result.
                wake.sequence_.fetch_add(1, std::memory_order_release);
                rt::wake(wake.sequence_);
                break;
            case MainThreadWake::RelayStep::BeforeWrite:
                ++shutdown.writes;
                break;
        }
    }
};
}  // namespace supple

namespace {
void notify(MainThreadWake& wake) SUPPLE_RT { wake.notify(); }

void ready(MainThreadWake& wake) {
    WaitItem fd{wake.waitable(), kWaitRead, 0};
    int result;
    do {
        result = waitFor(std::span(&fd, 1), -1);
    } while (result < 0 && errno == EINTR);
    REQUIRE(result == 1);
    REQUIRE(fd.ready == kWaitRead);
}
}  // namespace

TEST_CASE("main-thread wake notifications coalesce until main drains") {
    MainThreadWake wake;
    REQUIRE(wake.waitable().valid());
    CHECK_FALSE(wake.drain());
    for (int i = 0; i < 1000; ++i) notify(wake);
    ready(wake);
#if defined(__linux__)
    std::uint64_t count = 0;
    REQUIRE(::read(wake.fd(), &count, sizeof(count)) == sizeof(count));
    CHECK(count == 1);
#else
    REQUIRE(MainThreadWakeTestAccess::consumeEvent(wake));
#endif
    // Keep the pending bit set: no further relay write is possible, even if
    // this producer runs after the relay's first write completed.
    notify(wake);
    WaitItem fd{wake.waitable(), kWaitRead, 0};
    CHECK(waitFor(std::span(&fd, 1), 0) == 0);
}

TEST_CASE("main-thread wake shutdown between the quit check and sequence capture cannot park") {
    MainThreadWakeTestAccess::Shutdown shutdown;
    auto wake = MainThreadWakeTestAccess::create(shutdown);
    REQUIRE(wake->waitable().valid());
    notify(*wake);
    while (shutdown.stage.load() < 1) shutdown.stage.wait(0);
    // The relay is paused immediately before capture (after the quit check
    // in the old ordering). Let destruction publish its final sequence first.
    std::thread destroy([&] { wake.reset(); });
    for (int stage = shutdown.stage.load(); stage < 2; stage = shutdown.stage.load())
        shutdown.stage.wait(stage);
    shutdown.resume.store(true);
    shutdown.resume.notify_one();
    destroy.join();
    CHECK(shutdown.captured.load() == 2);  // notification, then shutdown
    CHECK(shutdown.writes.load() == 0);    // captured shutdown must exit before writing/waiting
}

TEST_CASE(
    "main-thread wake drain acquires coalesced publications and rearms requests during servicing") {
    MainThreadWake wake;
    REQUIRE(wake.waitable().valid());
    std::array<std::atomic<int>, 2> values{};
    for (int pass = 1; pass <= 100; ++pass) {
        values[0].store(pass, std::memory_order_relaxed);
        notify(wake);
        ready(wake);
        // An update made while the event is ready must survive coalescing.
        values[1].store(pass, std::memory_order_relaxed);
        notify(wake);
        REQUIRE(wake.drain());
        CHECK(values[0].load(std::memory_order_relaxed) == pass);
        CHECK(values[1].load(std::memory_order_relaxed) == pass);
        // A different producer arriving during main-thread servicing must
        // signal another pass, even while the relay returns from its write.
        std::thread producer([&] { notify(wake); });
        producer.join();
        ready(wake);
        REQUIRE(wake.drain());
        CHECK_FALSE(wake.drain());
    }
}
