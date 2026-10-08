#include <doctest/doctest.h>
#include <poll.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <limits>
#include <thread>

#include "../src/main_thread_wake.h"

using namespace supple;
using namespace supple::host;

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

TEST_CASE("bridge audio notifications coalesce until main drains") {
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

TEST_CASE("bridge drain acquires coalesced publications and rearms requests during servicing") {
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
