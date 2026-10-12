// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// What the audio thread may not do -- read files, decode, build tables,
// allocate -- done on a thread of its own, and the result handed over
// without a lock.
//
//   Worker: a thread that runs jobs in order, and recurring tasks between
//   them (a disk stream's read-ahead). Post from the main thread.
//
//   Handoff<T>: one finished object at a time, from the worker (or the main
//   thread) to the audio thread. The audio thread takes the newest with
//   update() and reads it with get(); what it lets go of is freed by the
//   publishing side later, never on the audio thread.
//
//   // main thread
//   worker_.post([this, path] {
//       auto table = std::make_unique<Table>(build(path));
//       table_.publish(std::move(table));
//   });
//   // audio thread, start of process()
//   table_.update();
//   if (const Table* t = table_.get()) ...
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/Ring.h"

namespace supple::dsp {

class Worker {
  public:
    Worker() = default;
    ~Worker() { stop(); }
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    // Runs `job` after those before it. The thread starts with the first.
    void post(std::function<void()> job) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopping_) return;
            jobs_.push_back(std::move(job));
            start();
        }
        wake_.notify_one();
    }

    // Blocks until every job posted so far has run.
    void wait() {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this] { return jobs_.empty() && busy_ == 0; });
    }

    // Runs `task` about every `ms` milliseconds, between jobs, until
    // cancel(id).
    int repeat(std::function<void()> task, double ms) {
        int id = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            id = ++lastId_;
            repeats_.push_back({id, std::move(task), std::chrono::microseconds(static_cast<long long>(ms * 1000.0)),
                                std::chrono::steady_clock::now()});
            start();
        }
        wake_.notify_one();
        return id;
    }
    // After this returns the task is not running and never runs again.
    void cancel(int id) {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_.wait(lock, [this, id] { return runningRepeat_ != id; });
        std::erase_if(repeats_, [id](const Repeat& r) { return r.id == id; });
    }

    // Finishes the job running, drops the rest, and ends the thread.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopping_ = true;
            jobs_.clear();
        }
        wake_.notify_one();
        if (thread_.joinable()) thread_.join();
    }

  private:
    struct Repeat {
        int id;
        std::function<void()> task;
        std::chrono::microseconds interval;
        std::chrono::steady_clock::time_point due;
    };

    void start() {
        if (!thread_.joinable() && !stopping_) thread_ = std::thread([this] { run(); });
    }

    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopping_) {
            if (!jobs_.empty()) {
                std::function<void()> job = std::move(jobs_.front());
                jobs_.pop_front();
                ++busy_;
                lock.unlock();
                job();
                lock.lock();
                --busy_;
                if (jobs_.empty()) idle_.notify_all();
                continue;
            }
            // The first recurring task due, if any; else sleep until one is.
            const auto now = std::chrono::steady_clock::now();
            auto next = now + std::chrono::hours(1);
            Repeat* due = nullptr;
            for (Repeat& r : repeats_) {
                if (r.due <= now && !due) due = &r;
                next = std::min(next, r.due);
            }
            if (!due) {
                wake_.wait_until(lock, next);
                continue;
            }
            due->due = now + due->interval;
            runningRepeat_ = due->id;
            std::function<void()> task = due->task;
            lock.unlock();
            task();
            lock.lock();
            runningRepeat_ = 0;
            idle_.notify_all();
        }
        idle_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable wake_, idle_;
    std::deque<std::function<void()>> jobs_;
    std::vector<Repeat> repeats_;
    int busy_ = 0, lastId_ = 0, runningRepeat_ = 0;
    bool stopping_ = false;
    std::thread thread_;
};

template <class T>
class Handoff {
  public:
    Handoff() = default;
    ~Handoff() {
        collect();
        delete current_;
        delete pending_.exchange(nullptr, std::memory_order_acq_rel);
    }
    Handoff(const Handoff&) = delete;
    Handoff& operator=(const Handoff&) = delete;

    // --- the publishing side: one thread at a time ----------------------------

    // Replaces what the audio thread will take next; null clears it. A value
    // published twice before the audio thread looked is freed here, unseen.
    void publish(std::unique_ptr<T> value) {
        collect();
        delete pending_.exchange(new Box{std::move(value)}, std::memory_order_acq_rel);
    }
    // Frees what the audio thread has let go of.
    void collect() {
        Box* done = nullptr;
        while (retired_.pop(done)) delete done;
    }

    // --- the audio thread (or any thread while it is stopped) ---------------

    // Takes the newest published value, if there is one: true when it changed.
    bool update() {
        if (!pending_.load(std::memory_order_relaxed) || !retired_.writable()) return false;
        Box* fresh = pending_.exchange(nullptr, std::memory_order_acq_rel);
        if (!fresh) return false;
        if (current_) retired_.push(current_);
        current_ = fresh;
        return true;
    }
    T* get() const { return current_ ? current_->value.get() : nullptr; }

  private:
    struct Box {
        std::unique_ptr<T> value;
    };
    std::atomic<Box*> pending_{nullptr};
    Box* current_ = nullptr;
    Ring<Box*, 3> retired_;
};

}  // namespace supple::dsp
