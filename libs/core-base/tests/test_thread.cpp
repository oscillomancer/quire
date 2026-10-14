// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#if !defined(_WIN32)
#include <pthread.h>
#endif
#if defined(__linux__)
#include <sys/resource.h>
#endif

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>

#include <supple/base/thread.h>

using supple::ThreadPolicy;

namespace {
#if !defined(_WIN32)
std::string nameOf(pthread_t thread) {
    char name[16] = {};
    REQUIRE(::pthread_getname_np(thread, name, sizeof(name)) == 0);
    return name;
}
#endif
#if defined(__linux__)
// Lowers one soft limit for the scope (process-local; the hard limit is kept,
// so the soft value can be restored).
struct LimitScope {
    int resource;
    rlimit saved{};
    LimitScope(int r, rlim_t value) : resource(r) {
        REQUIRE(::getrlimit(resource, &saved) == 0);
        rlimit lowered = saved;
        lowered.rlim_cur = std::min(value, saved.rlim_max);
        REQUIRE(::setrlimit(resource, &lowered) == 0);
    }
    ~LimitScope() { ::setrlimit(resource, &saved); }
};
#endif
// A live thread that only waits for the end of the scope: changing its
// scheduling is harmless (an exited thread could not be scheduled).
struct ParkedThread {
    std::atomic<bool> done{false};
    std::thread thread{[this] { done.wait(false); }};
    ~ParkedThread() {
        done.store(true);
        done.notify_one();
        thread.join();
    }
};
bool realtimeAllowed() {
#if defined(__linux__)
    rlimit rtprio{}, rttime{};
    return ::getrlimit(RLIMIT_RTPRIO, &rtprio) == 0 && rtprio.rlim_cur >= 1 &&
           ::getrlimit(RLIMIT_RTTIME, &rttime) == 0 && rttime.rlim_cur == RLIM_INFINITY;
#else
    return false;
#endif
}
}  // namespace

#if !defined(_WIN32)
TEST_CASE("nameThread names the calling thread and cuts at 15 bytes") {
    std::string shortName, longName;
    std::thread([&] {
        supple::nameThread("audio-test");
        shortName = nameOf(::pthread_self());
        supple::nameThread("audio-a-very-long-name");
        longName = nameOf(::pthread_self());
    }).join();
    CHECK(shortName == "audio-test");
    CHECK(longName == "audio-a-very-lo");
}

#endif
TEST_CASE("thread policies are read, set, verified and named") {
    CHECK(supple::policyName(ThreadPolicy::Kind::None) == "none");
    CHECK(supple::policyName(ThreadPolicy::Kind::Normal) == "normal");
    CHECK(supple::policyName(ThreadPolicy::Kind::Fifo) == "fifo");
    CHECK(supple::policyName(ThreadPolicy::Kind::RoundRobin) == "rr");
    CHECK(supple::policyName(ThreadPolicy::Kind::High) == "high");
    ParkedThread idle;
    const auto handle = idle.thread.native_handle();
    ThreadPolicy before, fifo, rr, after;
    supple::Status setFifo = supple::okStatus(), setRr = supple::okStatus();
    before = supple::threadPolicy(handle);
    if (realtimeAllowed()) {
        setFifo = supple::setThreadPolicy(handle, {ThreadPolicy::Kind::Fifo, 1});
        fifo = supple::threadPolicy(handle);
        setRr = supple::setThreadPolicy(handle, {ThreadPolicy::Kind::RoundRobin, 1});
        rr = supple::threadPolicy(handle);
    }
#if defined(_WIN32)
    REQUIRE(supple::setThreadPolicy(handle, {ThreadPolicy::Kind::High, 0}));
    CHECK(supple::threadPolicy(handle) == ThreadPolicy{ThreadPolicy::Kind::High, 0});
#endif
    REQUIRE(supple::setThreadPolicy(handle, {ThreadPolicy::Kind::Normal, 7}));
    after = supple::threadPolicy(handle);
    CHECK(before == ThreadPolicy{ThreadPolicy::Kind::Normal, 0});
    CHECK(after == ThreadPolicy{ThreadPolicy::Kind::Normal, 0});  // Normal has no priority
    if (!realtimeAllowed()) {
        MESSAGE("no real-time scheduling permission: FIFO/RR application not checked");
        return;
    }
    CHECK_MESSAGE(setFifo, (setFifo ? "" : setFifo.error().toString()));
    CHECK(fifo == ThreadPolicy{ThreadPolicy::Kind::Fifo, 1});
    CHECK_MESSAGE(setRr, (setRr ? "" : setRr.error().toString()));
    CHECK(rr == ThreadPolicy{ThreadPolicy::Kind::RoundRobin, 1});
}

#if defined(__linux__)
TEST_CASE("real-time policies are refused without permission or with a CPU time limit") {
    ParkedThread idle;
    const auto handle = idle.thread.native_handle();
    supple::Status denied = supple::okStatus(), limited = supple::okStatus();
    ThreadPolicy deniedPolicy, limitedPolicy;
    {
        LimitScope rtprio(RLIMIT_RTPRIO, 0);
        denied = supple::setThreadPolicy(handle, {ThreadPolicy::Kind::Fifo, 1});
        deniedPolicy = supple::threadPolicy(handle);
        if (denied) (void)supple::setThreadPolicy(handle, {ThreadPolicy::Kind::Normal, 0});
    }
    {
        LimitScope rttime(RLIMIT_RTTIME, 200000);
        limited = supple::setThreadPolicy(handle, {ThreadPolicy::Kind::Fifo, 1});
        limitedPolicy = supple::threadPolicy(handle);
    }
    if (denied) {
        MESSAGE("the process may raise priorities beyond RLIMIT_RTPRIO (CAP_SYS_NICE)");
    } else {
        CHECK(denied.error().code == "thread.policy");
        CHECK(denied.error().message.find("RLIMIT_RTPRIO is 0") != std::string::npos);
        CHECK_FALSE(denied.error().hint.empty());
        CHECK(deniedPolicy == ThreadPolicy{ThreadPolicy::Kind::Normal, 0});
    }
    REQUIRE_FALSE(limited);
    CHECK(limited.error().code == "thread.rttime");
    CHECK_FALSE(limited.error().hint.empty());
    CHECK(limitedPolicy == ThreadPolicy{ThreadPolicy::Kind::Normal, 0});
}

#endif

#if defined(__APPLE__)
TEST_CASE("macOS QoS is applied and verified by the owning thread") {
    supple::Status high, normal;
    ThreadPolicy observed;
    std::thread([&] {
        high = supple::setThreadPolicy(pthread_self(), {ThreadPolicy::Kind::High, 0});
        observed = supple::threadPolicy(pthread_self());
        normal = supple::setThreadPolicy(pthread_self(), {ThreadPolicy::Kind::Normal, 0});
    }).join();
    CHECK_MESSAGE(high, (high ? "" : high.error().toString()));
    CHECK(observed == ThreadPolicy{ThreadPolicy::Kind::High, 0});
    CHECK_MESSAGE(normal, (normal ? "" : normal.error().toString()));
}
#endif
