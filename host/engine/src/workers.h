// Internal: the engine's worker pool (ADR-0012, engine.md §2.2). It runs a
// task graph — one render block of a plan, one task per node — on its
// threads and the submitting thread together: a task starts once the tasks
// it depends on have finished; ready tasks wait in a bounded lock-free queue.
// Results do not depend on the thread count because every task reads only
// finished inputs and writes only its own state.
#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "supple/base/rt.h"

namespace supple::engine::detail {

// A bounded multi-producer multi-consumer queue of task indices (Vyukov's
// design: one sequence number per cell). Capacity is a power of two.
class ReadyQueue {
  public:
    void init(std::size_t minCapacity);
    bool push(std::uint32_t task) SUPPLE_RT;
    bool pop(std::uint32_t& task) SUPPLE_RT;

  private:
    struct Cell {
        std::atomic<std::size_t> seq{0};
        std::uint32_t task = 0;
    };
    std::unique_ptr<Cell[]> cells_;
    std::size_t mask_ = 0;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

// The dependency structure of a plan's nodes and the per-run counters.
// Built on the main thread; run by one Workers::run at a time.
struct TaskGraph {
    std::vector<std::uint32_t> preds;              // how many tasks each waits for
    std::vector<std::vector<std::uint32_t>> succ;  // the tasks waiting for each
    std::unique_ptr<std::atomic<std::uint32_t>[]> pending;
    ReadyQueue ready;
    std::atomic<std::uint32_t> remaining{0};

    // `deps[i]` lists the tasks task i reads from; every one must be < i.
    void build(const std::vector<std::vector<std::uint32_t>>& deps);
    std::uint32_t size() const { return static_cast<std::uint32_t>(preds.size()); }
};

class Workers {
  public:
    using Fn = void (*)(void* ctx, std::uint32_t task);

    // `helpers` threads besides the caller (0: the caller runs everything).
    explicit Workers(unsigned helpers);
    ~Workers();
    Workers(const Workers&) = delete;
    Workers& operator=(const Workers&) = delete;

    unsigned helpers() const { return static_cast<unsigned>(threads_.size()); }
    // The default helper count: every core but two, less the caller.
    static unsigned defaultHelpers();

    // Runs every task of `g` (fn(ctx, task) on any of the threads) and
    // returns when all have finished. One run at a time.
    void run(TaskGraph& g, Fn fn, void* ctx);

  private:
    void loop();
    static void help(TaskGraph& g, Fn fn, void* ctx);

    std::vector<std::thread> threads_;
    std::atomic<std::uint64_t> jobSeq_{0};  // bumped per run; helpers wait on it
    std::atomic<std::uint64_t> doneSeq_{0};
    std::atomic<unsigned> active_{0};  // helpers inside the current run
    std::atomic<bool> quit_{false};
    TaskGraph* job_ = nullptr;
    Fn fn_ = nullptr;
    void* ctx_ = nullptr;
};

}  // namespace supple::engine::detail
