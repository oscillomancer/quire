// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#pragma once
// Cooperative non-RT jobs. Linux/Windows retain the standard types; Apple's
// system libc++ currently omits them. SUPPLE_TEST_FALLBACK_STOP is only for
// standalone tests linked to libraries that do not include this header. No locks or callbacks
// belong on audio.
#include <thread>
#include <utility>
#include <version>
#if defined(__cpp_lib_jthread) && !defined(SUPPLE_TEST_FALLBACK_STOP)
#include <stop_token>
namespace supple {
using StopToken = std::stop_token;
using JoiningThread = std::jthread;
template <typename F>
class StopCallback {
  public:
    StopCallback(StopToken token, F callback) : callback_(std::move(token), std::move(callback)) {}

  private:
    std::stop_callback<F> callback_;
};
}  // namespace supple
#else
#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>
namespace supple {
namespace detail {
struct StopSlot {
    // Recursive so a callback may destroy its own registration.
    std::recursive_mutex mutex;
    bool active = true;
    std::function<void()> callback;
    void invoke() noexcept { callback(); }
};
struct StopState {
    std::atomic<bool> requested{false}, sourceAlive{true};
    std::mutex mutex;
    std::vector<std::shared_ptr<StopSlot> > callbacks;
    bool request() noexcept {
        std::vector<std::shared_ptr<StopSlot> > pending;
        {
            std::lock_guard lock(mutex);
            if (requested.exchange(true, std::memory_order_acq_rel)) return false;
            pending = callbacks;
        }
        for (const auto& slot : pending) {
            std::lock_guard lock(slot->mutex);
            if (slot->active) slot->invoke();
        }
        return true;
    }
};
}  // namespace detail
class StopToken {
  public:
    StopToken() = default;
    bool stop_requested() const noexcept {
        return state_ && state_->requested.load(std::memory_order_acquire);
    }
    bool stop_possible() const noexcept {
        return state_ && (state_->requested.load(std::memory_order_acquire) ||
                          state_->sourceAlive.load(std::memory_order_acquire));
    }

  private:
    friend class JoiningThread;
    template <typename F>
    friend class StopCallback;
    explicit StopToken(std::shared_ptr<detail::StopState> state) : state_(std::move(state)) {}
    std::shared_ptr<detail::StopState> state_;
};
template <typename F>
class StopCallback {
  public:
    StopCallback(StopToken token, F callback) : state_(std::move(token.state_)) {
        if (!state_) return;
        slot_ = std::make_shared<detail::StopSlot>();
        auto function = std::make_shared<F>(std::move(callback));
        slot_->callback = [function] { std::invoke(std::move(*function)); };
        bool immediate;
        {
            std::lock_guard lock(state_->mutex);
            immediate = state_->requested.load(std::memory_order_acquire);
            if (!immediate) state_->callbacks.push_back(slot_);
        }
        if (immediate) slot_->invoke();
    }
    ~StopCallback() {
        if (!slot_) return;
        // Wait for any callback claimed by request_stop(), then prevent a
        // snapshot that has not claimed this slot from invoking it later.
        {
            std::lock_guard lock(slot_->mutex);
            slot_->active = false;
        }
        std::lock_guard lock(state_->mutex);
        std::erase(state_->callbacks, slot_);
    }
    StopCallback(const StopCallback&) = delete;
    StopCallback& operator=(const StopCallback&) = delete;

  private:
    std::shared_ptr<detail::StopState> state_;
    std::shared_ptr<detail::StopSlot> slot_;
};
class JoiningThread {
  public:
    JoiningThread() = default;
    template <typename F>
        requires(!std::is_same_v<std::remove_cvref_t<F>, JoiningThread>)
    explicit JoiningThread(F&& work)
        : state_(std::make_shared<detail::StopState>()),
          thread_([token = StopToken(state_), task = std::forward<F>(work)]() mutable {
              if constexpr (std::is_invocable_v<std::decay_t<F>, StopToken>)
                  std::invoke(std::move(task), token);
              else
                  std::invoke(std::move(task));
          }) {}
    ~JoiningThread() { stopAndJoin(); }
    JoiningThread(JoiningThread&&) noexcept = default;
    JoiningThread& operator=(JoiningThread&& other) noexcept {
        if (this != &other) {
            stopAndJoin();
            state_ = std::move(other.state_);
            thread_ = std::move(other.thread_);
        }
        return *this;
    }
    JoiningThread(const JoiningThread&) = delete;
    JoiningThread& operator=(const JoiningThread&) = delete;
    bool joinable() const noexcept { return thread_.joinable(); }
    void join() { thread_.join(); }
    bool request_stop() noexcept { return state_ && state_->request(); }
    StopToken get_stop_token() const { return StopToken(state_); }

  private:
    void stopAndJoin() {
        if (joinable()) {
            request_stop();
            join();
        }
        if (state_) state_->sourceAlive.store(false, std::memory_order_release);
    }
    std::shared_ptr<detail::StopState> state_;
    std::thread thread_;
};
}  // namespace supple
#endif
namespace supple {
template <typename F>
StopCallback(StopToken, F) -> StopCallback<F>;
}
