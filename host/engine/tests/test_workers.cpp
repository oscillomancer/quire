// The worker pool (ADR-0012): every task runs once, after its dependencies,
// whatever the thread count.
#include <doctest/doctest.h>

#include <atomic>
#include <random>
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

void task(void* ctx, std::uint32_t t) {
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
