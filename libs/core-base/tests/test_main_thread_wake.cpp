#include <doctest/doctest.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <thread>

#include "supple/base/main_thread_wake.h"

using namespace supple;

namespace {
void notify(MainThreadWake& wake) SUPPLE_RT { wake.notify(); }

void ready(MainThreadWake& wake) {
    pollfd fd{wake.fd(), POLLIN, 0};
    int result;
    do {
        result = ::poll(&fd, 1, -1);
    } while (result < 0 && errno == EINTR);
    REQUIRE(result == 1);
    REQUIRE(fd.revents == POLLIN);
}
}  // namespace

TEST_CASE("main-thread wake notifications coalesce until main drains") {
    MainThreadWake wake;
    REQUIRE(wake.fd() >= 0);
    CHECK_FALSE(wake.drain());
    for (int i = 0; i < 1000; ++i) notify(wake);
    ready(wake);
    std::uint64_t count = 0;
    REQUIRE(::read(wake.fd(), &count, sizeof(count)) == sizeof(count));
    CHECK(count == 1);
    // Keep the pending bit set: no further relay write is possible, even if
    // this producer runs after the relay's first write completed.
    notify(wake);
    pollfd fd{wake.fd(), POLLIN, 0};
    CHECK(::poll(&fd, 1, 0) == 0);
}

TEST_CASE(
    "main-thread wake drain acquires coalesced publications and rearms requests during servicing") {
    MainThreadWake wake;
    REQUIRE(wake.fd() >= 0);
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
