// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "supple/engine/realtime.h"
namespace supple::engine::detail {
// Non-RT startup barrier. Every helper applies device policy on itself; all
// roll back if any refuses. Main publishes live jobs only after finish().
class ThreadStartup {
  public:
    ThreadStartup(unsigned count, Backend* backend, bool live)
        : results_(backend ? count : 0), backend_(backend), live_(live) {}
    bool enter(unsigned index) {
        if (!backend_) return false;
        results_[index] = backend_->prepareCurrentThread(live_);
        ready_.fetch_add(1, std::memory_order_release);
        ready_.notify_one();
        gate_.wait(0, std::memory_order_acquire);
        if (gate_.load(std::memory_order_acquire) != 1) {
            backend_->releaseCurrentThread();
            return false;
        }
        return true;
    }
    Status finish() {
        if (!backend_) return okStatus();
        unsigned seen;
        while ((seen = ready_.load(std::memory_order_acquire)) < results_.size()) ready_.wait(seen);
        Status result;
        for (const auto& status : results_)
            if (!status) {
                result = status;
                break;
            }
        gate_.store(result ? 1 : 2, std::memory_order_release);
        gate_.notify_all();
        return result;
    }
    void cancel() {
        gate_.store(2, std::memory_order_release);
        gate_.notify_all();
    }
    struct Cleanup {
        Backend* backend;
        ~Cleanup() {
            if (backend) backend->releaseCurrentThread();
        }
    };

  private:
    std::vector<Status> results_;
    Backend* backend_;
    bool live_;
    std::atomic<unsigned> ready_{0}, gate_{0};
};
}  // namespace supple::engine::detail
