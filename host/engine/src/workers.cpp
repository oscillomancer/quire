#include "workers.h"

#include <algorithm>
#include <bit>

#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif

namespace supple::engine::detail {

namespace {
inline void cpuRelax() SUPPLE_RT {
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#endif
}
}  // namespace

// --- ReadyQueue ------------------------------------------------------------------------

void ReadyQueue::init(std::size_t minCapacity) {
    const std::size_t cap = std::bit_ceil(std::max<std::size_t>(minCapacity, 2));
    cells_ = std::make_unique<Cell[]>(cap);
    for (std::size_t i = 0; i < cap; ++i) cells_[i].seq.store(i, std::memory_order_relaxed);
    mask_ = cap - 1;
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
}

bool ReadyQueue::push(std::uint32_t task) SUPPLE_RT {
    std::size_t pos = tail_.load(std::memory_order_relaxed);
    for (;;) {
        Cell& c = cells_[pos & mask_];
        const std::size_t seq = c.seq.load(std::memory_order_acquire);
        const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos);
        if (diff == 0) {
            if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                c.task = task;
                c.seq.store(pos + 1, std::memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false;  // full
        } else {
            pos = tail_.load(std::memory_order_relaxed);
        }
    }
}

bool ReadyQueue::pop(std::uint32_t& task) SUPPLE_RT {
    std::size_t pos = head_.load(std::memory_order_relaxed);
    for (;;) {
        Cell& c = cells_[pos & mask_];
        const std::size_t seq = c.seq.load(std::memory_order_acquire);
        const auto diff = static_cast<std::ptrdiff_t>(seq) - static_cast<std::ptrdiff_t>(pos + 1);
        if (diff == 0) {
            if (head_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                task = c.task;
                c.seq.store(pos + mask_ + 1, std::memory_order_release);
                return true;
            }
        } else if (diff < 0) {
            return false;  // empty
        } else {
            pos = head_.load(std::memory_order_relaxed);
        }
    }
}

// --- TaskGraph -------------------------------------------------------------------------

void TaskGraph::build(const std::vector<std::vector<std::uint32_t>>& deps) {
    const std::size_t n = deps.size();
    preds.assign(n, 0);
    succ.assign(n, {});
    for (std::size_t i = 0; i < n; ++i) {
        std::vector<std::uint32_t> d = deps[i];
        std::sort(d.begin(), d.end());
        d.erase(std::unique(d.begin(), d.end()), d.end());
        preds[i] = static_cast<std::uint32_t>(d.size());
        for (std::uint32_t from : d) succ[from].push_back(static_cast<std::uint32_t>(i));
    }
    pending = std::make_unique<std::atomic<std::uint32_t>[]>(std::max<std::size_t>(n, 1));
    ready.init(n);
}

// --- Workers ---------------------------------------------------------------------------

Workers::Workers(unsigned helpers) {
    threads_.reserve(helpers);
    for (unsigned i = 0; i < helpers; ++i) threads_.emplace_back([this] { loop(); });
}

Workers::~Workers() {
    quit_.store(true);
    jobSeq_.fetch_add(1);
    jobSeq_.notify_all();
    for (auto& t : threads_) t.join();
}

unsigned Workers::defaultHelpers() {
    const unsigned cores = std::max(1u, std::thread::hardware_concurrency());
    return cores > 3 ? cores - 3 : 0;  // cores - 2 workers, the caller being one
}

void Workers::help(TaskGraph& g, Fn fn, void* ctx) {
    unsigned idle = 0;
    while (g.remaining.load(std::memory_order_acquire) > 0) {
        std::uint32_t t = 0;
        if (!g.ready.pop(t)) {
            // Every ready task is taken; the rest wait on tasks in progress.
            if (++idle < 64)
                cpuRelax();
            else
                std::this_thread::yield();
            continue;
        }
        idle = 0;
        fn(ctx, t);
        for (std::uint32_t s : g.succ[t])
            if (g.pending[s].fetch_sub(1, std::memory_order_acq_rel) == 1) g.ready.push(s);
        g.remaining.fetch_sub(1, std::memory_order_acq_rel);
    }
}

void Workers::run(TaskGraph& g, Fn fn, void* ctx) {
    const std::uint32_t n = g.size();
    if (n == 0) return;
    for (std::uint32_t i = 0; i < n; ++i) g.pending[i].store(g.preds[i], std::memory_order_relaxed);
    g.remaining.store(n, std::memory_order_relaxed);
    for (std::uint32_t i = 0; i < n; ++i)
        if (g.preds[i] == 0) g.ready.push(i);
    if (threads_.empty()) {
        help(g, fn, ctx);
        return;
    }
    job_ = &g;
    fn_ = fn;
    ctx_ = ctx;
    const std::uint64_t seq = jobSeq_.fetch_add(1) + 1;  // publishes job_, fn_, ctx_
    jobSeq_.notify_all();
    help(g, fn, ctx);
    // Helpers that joined this run must leave it before the graph can go.
    doneSeq_.store(seq);
    while (active_.load() != 0) cpuRelax();
}

void Workers::loop() {
    std::uint64_t seen = 0;
    for (;;) {
        jobSeq_.wait(seen);
        const std::uint64_t seq = jobSeq_.load();
        if (quit_.load()) return;
        seen = seq;
        // Join only a run that is still current and not finished (the
        // submitter sets doneSeq_ then waits for active_ to drain).
        active_.fetch_add(1);
        if (jobSeq_.load() == seq && doneSeq_.load() != seq) help(*job_, fn_, ctx_);
        active_.fetch_sub(1);
    }
}

}  // namespace supple::engine::detail
