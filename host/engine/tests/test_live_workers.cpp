// The worker pool (ADR-0012): every task runs once, after its dependencies,
// whatever the thread count.
#include <doctest/doctest.h>

#include <atomic>
#include <latch>
#include <random>
#include <semaphore>
#include <vector>

#include "live_workers.h"

using namespace supple::engine::detail;

namespace {

struct Run {
    std::vector<std::vector<std::uint32_t>> deps;
    std::unique_ptr<std::atomic<int>[]> done;  // times each task ran
    std::atomic<int> violations{0};
    int spin = 0;
};

void task(void* ctx, std::uint32_t t) SUPPLE_RT {
    auto* r = static_cast<Run*>(ctx);
    for (std::uint32_t d : r->deps[t])
        if (r->done[d].load(std::memory_order_acquire) != 1) r->violations.fetch_add(1);
    volatile int sink = 0;
    for (int i = 0; i < r->spin; ++i) sink = sink + i;  // overlap tasks a little
    r->done[t].fetch_add(1, std::memory_order_release);
}

}  // namespace

TEST_CASE("live pool: runs every task once, after its dependencies") {
    std::mt19937 rng(7);
    for (unsigned helpers : {0u, 1u, 3u, 7u}) {
        LiveWorkers workers(helpers);
        CHECK(workers.helpers() == helpers);
        for (int graph = 0; graph < 40; ++graph) {
            Run r;
            const std::uint32_t n = 1 + rng() % 60;
            r.deps.resize(n);
            for (std::uint32_t i = 1; i < n; ++i)
                for (std::uint32_t k = rng() % 4; k > 0; --k) r.deps[i].push_back(rng() % i);
            r.spin = static_cast<int>(rng() % 2000);
            TaskGraph g;
            g.build(r.deps);
            for (int rep = 0; rep < 5; ++rep) {  // a graph is reused block after block
                r.done = std::make_unique<std::atomic<int>[]>(n);
                workers.run(g, &task, &r);
                for (std::uint32_t i = 0; i < n; ++i) REQUIRE(r.done[i].load() == 1);
            }
            CHECK(r.violations.load() == 0);
        }
    }
}

TEST_CASE("live pool: an empty graph and a chain") {
    LiveWorkers workers(3);
    TaskGraph empty;
    empty.build({});
    workers.run(empty, &task, nullptr);  // returns at once
    Run r;
    r.deps = {{}, {0}, {1}, {2}, {3}};
    TaskGraph g;
    g.build(r.deps);
    r.done = std::make_unique<std::atomic<int>[]>(5);
    workers.run(g, &task, &r);
    CHECK(r.violations.load() == 0);
    for (int i = 0; i < 5; ++i) CHECK(r.done[i].load() == 1);
}

TEST_CASE("live pool: preempted enrollment and departure retain graph and context") {
    for (auto pause : {LiveWorkers::Event::Join, LiveWorkers::Event::Leave}) {
        struct Gate {
            LiveWorkers::Event pause;
            std::atomic<bool> claimed{false}, joining{false};
            std::binary_semaphore entered{0}, resume{0};
        } gate{pause};
        LiveWorkers pool(7, {[](void* p, LiveWorkers::Event e) {
                                 auto& x = *static_cast<Gate*>(p);
                                 if (e == x.pause && !x.claimed.exchange(true)) {
                                     x.entered.release();
                                     x.resume.acquire();
                                 }
                                 if (e == LiveWorkers::Event::JoinWait) x.joining.store(true);
                             },
                             &gate});
        Run r;
        r.deps = {{}, {}, {0, 1}};
        r.done = std::make_unique<std::atomic<int>[]>(3);
        auto graph = std::make_unique<TaskGraph>();
        graph->build(r.deps);
        std::atomic<bool> returned{false};
        std::thread submit([&] {
            pool.run(*graph, task, &r);
            returned = true;
        });
        gate.entered.acquire();
        while (!gate.joining.load()) std::this_thread::yield();
        CHECK_FALSE(returned.load());
        gate.resume.release();
        submit.join();
        CHECK(returned.load());
        for (unsigned i = 0; i < 3; ++i) CHECK(r.done[i] == 1);
        graph.reset();
        for (unsigned width : {8u, 1u, 2u, 16u, 4u}) {
            Run next;
            next.deps.resize(width);
            next.done = std::make_unique<std::atomic<int>[]>(width);
            TaskGraph replacement;
            replacement.build(next.deps);
            pool.run(replacement, task, &next);
            for (unsigned i = 0; i < width; ++i) CHECK(next.done[i] == 1);
        }
    }
}

TEST_CASE("live pool: publication before parking is not lost; chains wake nobody") {
    struct Gate {
        std::atomic<bool> first{true};
        std::atomic<unsigned> joins{0}, calls{0};
        std::binary_semaphore entered{0}, resume{0};
    } gate;
    LiveWorkers pool(1, {[](void* p, LiveWorkers::Event e) {
                             auto& x = *static_cast<Gate*>(p);
                             if (e == LiveWorkers::Event::Idle && x.first.exchange(false)) {
                                 x.entered.release();
                                 x.resume.acquire();
                             }
                             if (e == LiveWorkers::Event::Join) ++x.joins;
                         },
                         &gate});
    gate.entered.acquire();
    TaskGraph chain;
    chain.build({{}, {0}, {1}});
    auto count = [](void* p, std::uint32_t) { ++static_cast<Gate*>(p)->calls; };
    pool.run(chain, count, &gate);
    CHECK(gate.calls == 3);
    CHECK(gate.joins == 0);
    TaskGraph parallel;
    parallel.build({{}, {}});
    std::thread submit([&] { pool.run(parallel, count, &gate); });
    while (gate.calls != 5) std::this_thread::yield();
    // Publication already happened; the helper hasn't entered futex wait yet.
    gate.resume.release();
    submit.join();
    CHECK(gate.joins == 1);
    CHECK(gate.calls == 5);
}

TEST_CASE("live pool: a delayed node publishes state once before its dependent") {
    struct State {
        std::atomic<bool> rootEntered{false}, helperJoined{false}, resume{false};
        int rootState = 0, dependentState = 0;
        std::atomic<unsigned> roots{0};
    } state;
    LiveWorkers pool(1, {[](void* p, LiveWorkers::Event e) {
                             if (e == LiveWorkers::Event::Join)
                                 static_cast<State*>(p)->helperJoined = true;
                         },
                         &state});
    TaskGraph graph;
    graph.build({{}, {}, {0, 1}});
    std::atomic<bool> returned{false};
    std::thread submit([&] {
        pool.run(
            graph,
            [](void* p, std::uint32_t taskIndex) SUPPLE_RT {
                auto& s = *static_cast<State*>(p);
                if (taskIndex == 0) {
                    s.rootEntered = true;
                    while (!s.resume.load()) supple::rt::relax();
                    s.rootState = 123;
                }
                if (taskIndex < 2)
                    ++s.roots;
                else
                    s.dependentState = s.rootState + static_cast<int>(s.roots.load());
            },
            &state);
        returned = true;
    });
    while (!state.rootEntered.load() || !state.helperJoined.load()) std::this_thread::yield();
    CHECK_FALSE(returned.load());
    state.resume = true;
    submit.join();
    CHECK(state.roots == 2);
    CHECK(state.dependentState == 125);
}
