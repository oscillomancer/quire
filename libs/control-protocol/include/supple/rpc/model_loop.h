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
    std::atomic<bool> cancelled{false};
    std::weak_ptr<Wake> wake;
};
}  // namespace detail

// A cancellable periodic timer on the model thread (ModelLoop::every).
// Destroying or cancelling it stops it; it is safe to cancel from any thread.
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
    // called from any thread.
    Timer every(std::chrono::milliseconds interval, std::function<void()> fn);
    void post(std::function<void()> task);
    void run();  // blocks until SIGINT or SIGTERM

    // Embedding in another event loop (the GUI's): `waker` is called, from any
    // thread, whenever the loop has new work (a post, a timer change); it must
    // only wake the owner (e.g. push an SDL event). Set it before other threads
    // use the loop. Signals do not call it: the owner sees stopRequested() at
    // its next wake-up, which the nearest timer bounds.
    void setWaker(std::function<void()> waker);
    // Runs the due timers and the posted tasks once, without blocking.
    // Returns the milliseconds until the nearest timer, or -1 when there is none.
    int pump();
    // SIGINT or SIGTERM arrived.
    bool stopRequested() const;

  private:
    struct Impl;
    Impl* impl_;
};
}  // namespace supple::rpc
