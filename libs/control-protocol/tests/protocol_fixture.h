// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <doctest/doctest.h>
#include <supple/rpc/protocol.h>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
using namespace supple;
namespace {
struct Amount {
    int value = 0;
};
struct Count {
    int count = 0;
};
struct Blob {
    std::string data;
};
struct Host : cmd::Host {
    int count = 0, prior = 0;
    cmd::Origin last;
    std::function<void(std::string_view, const json::Value&)> events;
    cmd::Context makeContext(const cmd::Origin& origin) override {
        last = origin;
        cmd::Context ctx(origin);
        ctx.provide(count);
        return ctx;
    }
    Result<std::uint64_t> beginTransaction(const cmd::Origin&, std::string_view) override {
        prior = count;
        return std::uint64_t{1};
    }
    Status commitTransaction(std::uint64_t) override { return {}; }
    Status rollbackTransaction(std::uint64_t) override {
        count = prior;
        return okStatus();
    }
    Result<std::function<Status()>> invocationSavepoint() override {
        return std::function<Status()>([this, saved = count] {
            count = saved;
            return okStatus();
        });
    }
    void log(const cmd::LogRecord&) override {}
    void emit(std::string_view topic, json::Value payload) override {
        if (events) events(topic, payload);
    }
};
cmd::Registry registry() {
    cmd::Registry r;
    auto argsSchema = schema::object<Amount>("amount")
                          .field("value", &Amount::value, schema::integerOf<int>())
                          .build();
    auto result = schema::object<Count>("count")
                      .field("count", &Count::count, schema::integerOf<int>())
                      .build();
    cmd::Command<Amount, Count> command;
    command.name = "counter.add";
    command.kind = cmd::Kind::Edit;
    command.exposure = cmd::Exposure::Internal;
    command.summary = "Add to counter.";
    command.args = argsSchema;
    command.result = result;
    command.run = [](const Amount& args, cmd::Context& ctx) -> Result<Count> {
        return Count{ctx.service<int>() += args.value};
    };
    r.add(std::move(command));
    auto blobSchema =
        schema::object<Blob>("blob").field("data", &Blob::data, schema::string()).build();
    cmd::Command<Amount, Blob> large;
    large.name = "counter.blob";
    large.kind = cmd::Kind::Query;
    large.exposure = cmd::Exposure::Internal;
    large.summary = "Return a test blob.";
    large.args = argsSchema;
    large.result = blobSchema;
    large.run = [](const Amount& args, cmd::Context&) -> Result<Blob> {
        return Blob{std::string(static_cast<std::size_t>(args.value), 'x')};
    };
    r.add(std::move(large));
    cmd::Command<Amount, Count> failing;
    failing.name = "counter.fail";
    failing.kind = cmd::Kind::Query;
    failing.exposure = cmd::Exposure::Internal;
    failing.summary = "Fail with a located error.";
    failing.args = argsSchema;
    failing.result = result;
    failing.run = [](const Amount&, cmd::Context&) -> Result<Count> {
        Error detail("test.detail", "inner problem");
        detail.loc = SourceLoc{4, 2};
        Error error("test.located", "located problem", "/text", "fix line 3");
        error.loc = SourceLoc{3, 14};
        error.details.push_back(std::move(detail));
        return error;
    };
    r.add(std::move(failing));
    return r;
}
// Waits in these tests end on a handshake; the guard only stops a hung test
// and never decides a timing-dependent result.
constexpr auto kGuard = std::chrono::seconds(60);
[[maybe_unused]] constexpr int kGuardMs = 60000;
// A handshake between the test thread and work on other threads.
struct Probe {
    std::mutex mutex;
    std::condition_variable cv;
    template <typename F>
    void update(F change) {
        {
            std::lock_guard lock(mutex);
            change();
        }
        cv.notify_all();
    }
    template <typename P>
    bool waitFor(P predicate) {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, kGuard, predicate);
    }
};
struct ModelQueue {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::function<void()>> tasks;
    bool stopping = false;
    std::thread thread;
    ModelQueue()
        : thread([this] {
              while (true) {
                  std::function<void()> task;
                  {
                      std::unique_lock lock(mutex);
                      cv.wait(lock, [&] { return stopping || !tasks.empty(); });
                      if (stopping && tasks.empty()) break;
                      task = std::move(tasks.front());
                      tasks.pop_front();
                  }
                  task();
              }
          }) {}
    ~ModelQueue() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        cv.notify_all();
        thread.join();
    }
    void post(std::function<void()> task) {
        {
            std::lock_guard lock(mutex);
            tasks.push_back(std::move(task));
        }
        cv.notify_one();
    }
};
json::Value parse(std::string_view text) { return json::parse(text).value(); }
}  // namespace
