// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <atomic>
#include <functional>
#include <future>
#include <memory>
#include "supple/base/stop.h"

// Consumers may import both the vendor and a component with its own detail.
namespace stop_consumer::detail {
inline constexpr bool visible = true;
}
using namespace supple;
using namespace stop_consumer;
static_assert(detail::visible);
TEST_CASE("joining jobs publish cancellation, invoke once, unregister and join") {
    std::atomic<bool> done{false};
    std::atomic<int> calls{0};
    supple::StopToken saved;
    {
        supple::JoiningThread job([&](supple::StopToken token) {
            while (!token.stop_requested()) std::this_thread::yield();
            done.store(true, std::memory_order_release);
        });
        saved = job.get_stop_token();
        CHECK(saved.stop_possible());
        {
            supple::StopCallback removed(saved, [&] { calls.fetch_add(100); });
        }
        supple::StopCallback callback(saved, [&] { calls.fetch_add(1); });
        CHECK(job.request_stop());
        CHECK_FALSE(job.request_stop());
        CHECK(saved.stop_requested());
        CHECK(calls.load() == 1);
        supple::StopCallback immediate(
            saved, [owned = std::make_unique<int>(2), &calls] { calls.fetch_add(*owned); });
        CHECK(calls.load() == 3);
    }
    CHECK(done.load(std::memory_order_acquire));
    CHECK(saved.stop_requested());
    CHECK_FALSE(supple::StopToken{}.stop_possible());
}
TEST_CASE("cancellation callback destruction waits for in-flight delivery") {
    std::promise<void> entered, release;
    auto proceed = release.get_future();
    supple::JoiningThread job([](supple::StopToken token) {
        while (!token.stop_requested()) std::this_thread::yield();
    });
    std::atomic<bool> removed{false}, removedBeforeCallbackReturn{false};
    auto callback =
        std::make_unique<supple::StopCallback<std::function<void()>>>(job.get_stop_token(), [&] {
            entered.set_value();
            proceed.wait();
            removedBeforeCallbackReturn.store(removed.load(std::memory_order_acquire));
        });
    std::thread requester([&] { job.request_stop(); });
    entered.get_future().wait();
    std::thread remover([&] {
        callback.reset();
        removed.store(true, std::memory_order_release);
    });
    release.set_value();
    remover.join();
    requester.join();
    job.join();
    CHECK(removed.load(std::memory_order_acquire));
    CHECK_FALSE(removedBeforeCallbackReturn.load());
}
TEST_CASE("joining job move assignment cancels and joins its previous worker") {
    std::atomic<int> finished{0};
    auto work = [&](supple::StopToken token) {
        while (!token.stop_requested()) std::this_thread::yield();
        finished.fetch_add(1);
    };
    {
        supple::JoiningThread first(work), second(work);
        first = std::move(second);
        CHECK(finished.load() == 1);
        CHECK_FALSE(second.joinable());
    }
    CHECK(finished.load() == 2);
}

TEST_CASE("stop callbacks may unregister themselves and joined sources expire") {
    supple::JoiningThread job([] {});
    const auto token = job.get_stop_token();
    using Callback = supple::StopCallback<std::function<void()>>;
    std::unique_ptr<Callback> registration;
    bool called = false;
    registration = std::make_unique<Callback>(token, [&] {
        called = true;
        registration.reset();
    });
    CHECK(job.request_stop());
    CHECK(called);
    CHECK_FALSE(registration);
    job.join();
    supple::StopToken expired;
    {
        supple::JoiningThread finished([] {});
        expired = finished.get_stop_token();
        finished.join();
        CHECK(expired.stop_possible());
    }
    CHECK_FALSE(expired.stop_possible());
    CHECK_FALSE(expired.stop_requested());
}

TEST_CASE("a cancellation callback can remove a later delivery") {
    supple::JoiningThread job([] {});
    using Callback = supple::StopCallback<std::function<void()>>;
    std::unique_ptr<Callback> first, second;
    int calls = 0;
    first = std::make_unique<Callback>(job.get_stop_token(), [&] {
        ++calls;
        second.reset();
    });
    second = std::make_unique<Callback>(job.get_stop_token(), [&] {
        ++calls;
        first.reset();
    });
    CHECK(job.request_stop());
    CHECK(calls == 1);
    job.join();
}
