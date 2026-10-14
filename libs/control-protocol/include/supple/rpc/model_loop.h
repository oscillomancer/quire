// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace supple::rpc {

namespace detail {
// The wake pipe a timer signals to interrupt the loop's wait (shared so a
// timer can outlive a cancel from another thread without holding the loop).
// The pipe is closed with the last holder (the loop, or a timer cancelling
// from another thread while the loop goes away), never under a notify.
struct Wake {
    int readFd = -1, writeFd = -1;
    Wake() = default;
    Wake(const Wake&) = delete;
    Wake& operator=(const Wake&) = delete;
    ~Wake();
    void notify() const;
    // Called by notify() too, from any thread (ModelLoop::setWaker).
    std::function<void()> waker;
};
struct TimerState {
    std::function<void()> fn;
    std::chrono::steady_clock::duration interval{};
    std::chrono::steady_clock::time_point next{};
    bool once = false;
    std::atomic<bool> cancelled{false};
    std::weak_ptr<Wake> wake;
};
struct FdState {
    int fd = -1;
    short events = 0;
    std::function<void(short)> fn;
    std::atomic<bool> cancelled{false};
    std::weak_ptr<Wake> wake;
};
// The poll timeout for a wait of `remaining`: rounded up to whole milliseconds,
// so a wait never ends before its deadline (a positive sub-millisecond
// remainder is 1 ms, not a busy 0 ms poll), and clamped to [0, INT_MAX].
int waitMilliseconds(std::chrono::steady_clock::duration remaining);
}  // namespace detail

// A cancellable periodic timer on the model thread (ModelLoop::every).
// Destroying or cancelling it stops it; it is safe to cancel from any thread.
//
// Cancellation guarantee. On the model thread (a posted task, or any timer
// callback, including another timer's) cancel() takes effect at once: the
// callback is not invoked again, even when it is already due in the current
// pass, so one callback may destroy another timer's owner. From another
// thread, cancel() does not wait: at most one invocation that already passed the model
// thread's final cancellation check may still start or finish. To free state a callback uses from
// another thread, cancel on the model thread (post the cancel) or wait for that invocation another
// way.
class Timer {
  public:
    Timer() = default;
    ~Timer() { cancel(); }
    Timer(Timer&& other) noexcept = default;
    Timer& operator=(Timer&& other) noexcept;
    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;
    void cancel();
    bool active() const { return state_ && !state_->cancelled.load(std::memory_order_acquire); }

  private:
    friend class ModelLoop;
    explicit Timer(std::shared_ptr<detail::TimerState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::TimerState> state_;
};

// A level-triggered fd watch. Same cancellation guarantee as Timer. Cancel
// on the model thread before closing/reusing the fd or destroying its owner.
class FdWatch {
  public:
    FdWatch() = default;
    ~FdWatch() { cancel(); }
    FdWatch(FdWatch&&) noexcept = default;
    FdWatch& operator=(FdWatch&&) noexcept;
    FdWatch(const FdWatch&) = delete;
    FdWatch& operator=(const FdWatch&) = delete;
    void cancel();
    bool active() const { return state_ && !state_->cancelled.load(std::memory_order_acquire); }

  private:
    friend class ModelLoop;
    explicit FdWatch(std::shared_ptr<detail::FdState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::FdState> state_;
};

// Executes posted socket requests on the headless host's model thread, and runs
// periodic timers between them (a service tick).
class ModelLoop {
  public:
    ModelLoop();
    ~ModelLoop();
    ModelLoop(const ModelLoop&) = delete;
    ModelLoop& operator=(const ModelLoop&) = delete;
    // Runs `fn` on the model thread every `interval` (the first time one
    // interval from now), never concurrently with a posted task. The loop's
    // wait is bounded by the nearest timer, so there is no busy wait. May be
    // called from any thread. Intervals below 1 ms are 1 ms; an interval
    // beyond the clock's range never comes due.
    Timer every(std::chrono::milliseconds interval, std::function<void()> fn);
    // One invocation at an absolute monotonic deadline. Already due deadlines
    // run on the next pass, never inline. May be registered from any thread.
    Timer at(std::chrono::steady_clock::time_point deadline, std::function<void()> fn);
    // `events` and callback readiness flags use poll(2)'s POLLIN/POLLOUT/etc.
    FdWatch watch(int fd, short events, std::function<void(short)> fn);
    void post(std::function<void()> task);
    void run();  // blocks until SIGINT or SIGTERM

    // Embedding in another event loop (the GUI's): `waker` is called, from any
    // thread, whenever the loop has new work (a post, a timer change); it must
    // only wake the owner (e.g. push an SDL event). Set it before other threads
    // use the loop. Signals only make wakeFd() readable; an embedded owner
    // watches that fd too, so shutdown wakes even without a timer.
    void setWaker(std::function<void()> waker);
    // Borrowed read descriptor for embedding: watch for POLLIN, then pump().
    // Only pump/run drains it. Stop watching before destroying the loop.
    int wakeFd() const;
    // Runs the due timers and the posted tasks once, without blocking.
    // Returns the milliseconds until the nearest timer (detail::waitMilliseconds:
    // rounded up, at most INT_MAX), or -1 when there is none.
    int pump();
    // SIGINT or SIGTERM arrived.
    bool stopRequested() const;

  private:
    struct Impl;
    Impl* impl_;
};
}  // namespace supple::rpc
