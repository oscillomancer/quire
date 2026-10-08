// The worker pool (ADR-0012): every task runs once, after its dependencies,
// whatever the thread count.
#include <doctest/doctest.h>

#include <atomic>
#include <latch>
#include <random>
#include <semaphore>
#include <vector>

#include "workers.h"

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

TEST_CASE("the worker pool runs every task once, after its dependencies") {
    std::mt19937 rng(7);
    for (unsigned helpers : {0u, 1u, 3u, 7u}) {
        Workers workers(helpers);
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

TEST_CASE("an empty graph and a chain") {
    Workers workers(3);
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

TEST_CASE("chain cover bounds participation without hiding unequal-depth parallelism") {
    TaskGraph g;
    g.build({});
    CHECK(g.parallelism == 0);
    g.build({{}, {0}, {1}, {2}, {3}});
    CHECK(g.parallelism == 1);
    g.build({{}, {0}, {0}, {1, 2}, {3}});
    CHECK(g.parallelism == 2);
    // A slow root beside a short prefix that fans out later: each level has
    // at most two tasks, but root 0 and leaves 3/4 can overlap.
    g.build({{}, {}, {1}, {2}, {2}, {0, 3, 4}});
    CHECK(g.parallelism == 3);

    struct Counts {
        std::atomic<unsigned> joins{0}, calls{0};
    } counts;
    Workers pool(7, {[](void* p, Workers::Event e) {
                         if (e == Workers::Event::Join) ++static_cast<Counts*>(p)->joins;
                     },
                     &counts});
    auto count = [](void* p, std::uint32_t) { ++static_cast<Counts*>(p)->calls; };
    for (unsigned width : {1u, 2u, 4u, 16u}) {
        std::vector<std::vector<std::uint32_t>> deps(width * 32);
        for (unsigned i = width; i < deps.size(); ++i) deps[i] = {i - width};
        g.build(deps);
        CHECK(g.parallelism == width);
        counts.joins = 0;
        counts.calls = 0;
        pool.run(g, count, &counts);
        CHECK(counts.joins == std::min(7u, width - 1));
        CHECK(counts.calls == deps.size());
    }
}

TEST_CASE("selected helpers own the graph through preempted join and departure") {
    for (const auto pause : {Workers::Event::Join, Workers::Event::Leave}) {
        struct Gate {
            Workers::Event pause;
            std::atomic<bool> claimed{false};
            std::binary_semaphore entered{0}, resume{0}, joining{0};
            std::atomic<bool> joinSignaled{false};
        } gate{pause};
        Workers pool(7, {[](void* p, Workers::Event e) {
                             auto& x = *static_cast<Gate*>(p);
                             if (e == x.pause && !x.claimed.exchange(true)) {
                                 x.entered.release();
                                 x.resume.acquire();
                             }
                             if (e == Workers::Event::JoinWait && !x.joinSignaled.exchange(true))
                                 x.joining.release();
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
        gate.joining.acquire();
        CHECK_FALSE(returned.load());
        gate.resume.release();
        submit.join();
        CHECK(returned.load());
        for (unsigned i = 0; i < 3; ++i) CHECK(r.done[i] == 1);
        graph.reset();  // reclaim immediately; no late helper may touch it
        // A new graph/context on the same pool, including previously unused
        // slots. Repeated destruction also exercises fast successive jobs.
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

TEST_CASE("completion between empty-queue observation and parking cannot lose its wake") {
    struct Race {
        std::atomic<bool> taskClaimed{false}, waitClaimed{false}, completed{false};
        std::atomic<unsigned> calls{0};
        std::binary_semaphore taskResume{0}, waiting{0}, waitResume{0}, completion{0};
    } race;
    Workers pool(1, {[](void* p, Workers::Event e) {
                         auto& x = *static_cast<Race*>(p);
                         if (e == Workers::Event::Wait && !x.waitClaimed.exchange(true)) {
                             x.waiting.release();
                             x.waitResume.acquire();
                         }
                         if ((e == Workers::Event::Leave || e == Workers::Event::JoinWait) &&
                             !x.completed.exchange(true))
                             x.completion.release();
                     },
                     &race});
    TaskGraph g;
    g.build({{}, {}});
    std::thread submit([&] {
        pool.run(
            g,
            [](void* p, std::uint32_t) {
                auto& x = *static_cast<Race*>(p);
                if (!x.taskClaimed.exchange(true)) x.taskResume.acquire();
                ++x.calls;
            },
            &race);
    });
    race.waiting.acquire();
    race.taskResume.release();
    race.completion.acquire();  // remaining == 0 and its notification already happened
    CHECK(race.calls == 2);
    race.waitResume.release();  // the stale wait must return without another notification
    submit.join();
}

TEST_CASE("a fan-out after a serial prefix wakes every parked participant") {
    struct Fanout {
        std::latch parked{7}, leaves{8};
        std::atomic<unsigned> waits{0}, calls{0};
    } fanout;
    Workers pool(7, {[](void* p, Workers::Event e) {
                         auto& x = *static_cast<Fanout*>(p);
                         if (e == Workers::Event::Wait && x.waits.fetch_add(1) < 7)
                             x.parked.count_down();
                     },
                     &fanout});
    TaskGraph g;
    std::vector<std::vector<std::uint32_t>> deps(10);
    for (unsigned i = 1; i <= 8; ++i) {
        deps[i] = {0};
        deps[9].push_back(i);
    }
    g.build(deps);
    pool.run(
        g,
        [](void* p, std::uint32_t t) {
            auto& x = *static_cast<Fanout*>(p);
            if (t == 0)
                x.parked.wait();
            else if (t <= 8)
                x.leaves.arrive_and_wait();
            ++x.calls;
        },
        &fanout);
    CHECK(fanout.calls == 10);
}
