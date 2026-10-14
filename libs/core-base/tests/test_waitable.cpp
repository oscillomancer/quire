// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <supple/base/waitable.h>
#include <array>
#include <thread>

TEST_CASE("native waitable event wakes a parked consumer and drains readiness") {
    supple::WakeEvent event, other;
    REQUIRE(event.waitable().valid());
    REQUIRE(other.waitable().valid());
    std::array<supple::WaitItem, 4> sources{
        {{event.waitable()}, {other.waitable()}, {event.waitable()}, {event.waitable(), 0}}};
    CHECK(supple::waitFor(sources, 0) == 0);
    std::thread producer([&] { event.notify(); });
    const int ready = supple::waitFor(sources, -1);
    producer.join();
    REQUIRE(ready == 2);
    CHECK(sources[0].ready == supple::kWaitRead);
    CHECK(sources[1].ready == 0);
    CHECK(sources[2].ready == supple::kWaitRead);
    CHECK(sources[3].ready == 0);
    REQUIRE(event.drain());
    CHECK_FALSE(event.drain());
    CHECK(supple::waitFor(sources, 0) == 0);
    event.notify();
    other.notify();
    CHECK(supple::waitFor(sources, 0) == 3);
    CHECK(event.drain());
    CHECK(other.drain());
}

TEST_CASE("a single native event drain coalesces a burst of notifications") {
    supple::WakeEvent event;
    REQUIRE(event.waitable().valid());
    for (int i = 0; i < 1000; ++i) event.notify();
    REQUIRE(event.drain());
    CHECK_FALSE(event.drain());
}
