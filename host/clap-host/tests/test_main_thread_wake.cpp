#include <doctest/doctest.h>

#include <chrono>
#include <limits>

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
