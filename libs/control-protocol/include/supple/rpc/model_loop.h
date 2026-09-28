#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>

namespace supple::rpc {

namespace detail {
// The wake pipe a timer signals to interrupt the loop's wait (shared so a
// timer can outlive a cancel from another thread without holding the loop).
struct Wake {
    int readFd = -1, writeFd = -1;
    void notify() const;
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

  private:
    struct Impl;
    Impl* impl_;
};
}  // namespace supple::rpc
