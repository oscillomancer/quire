#include <doctest/doctest.h>
#include <poll.h>
#include <supple/product.h>
#include <supple/rpc/model_loop.h>
#include <supple/rpc/protocol.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <deque>
#include <future>
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
    void rollbackTransaction(std::uint64_t) override { count = prior; }
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
    return r;
}
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
int rawConnect(const std::filesystem::path& path) {
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    auto name = path.string();
    std::memcpy(address.sun_path, name.c_str(), name.size() + 1);
    REQUIRE(::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    return fd;
}
json::Value readFrame(int fd) {
    std::string frame;
    while (true) {
        pollfd pfd{fd, POLLIN, 0};
        REQUIRE(::poll(&pfd, 1, 2000) > 0);
        char ch = 0;
        REQUIRE(::recv(fd, &ch, 1, 0) == 1);
        if (ch == '\n') break;
        frame += ch;
    }
    return parse(frame);
}
json::Value readFrameFast(int fd, std::string& pending) {
    while (true) {
        auto newline = pending.find('\n');
        if (newline != std::string::npos) {
            auto frame = pending.substr(0, newline);
            pending.erase(0, newline + 1);
            return parse(frame);
        }
        pollfd pfd{fd, POLLIN, 0};
        REQUIRE(::poll(&pfd, 1, 5000) > 0);
        char bytes[8192];
        auto count = ::recv(fd, bytes, sizeof(bytes), 0);
        REQUIRE(count > 0);
        pending.append(bytes, static_cast<std::size_t>(count));
    }
}
json::Value exchange(int fd, std::string request) {
    request += '\n';
    REQUIRE(::send(fd, request.data(), request.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(request.size()));
    return readFrame(fd);
}
}  // namespace
TEST_CASE("socket JSON-RPC handshake, dispatch, errors, events and permissions") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    ModelQueue queue;
    rpc::Server server(commands, dispatcher, [&](auto task) { queue.post(std::move(task)); });
    host.events = [&](std::string_view topic, const json::Value& payload) {
        server.publish(topic, payload);
    };
    auto path = std::filesystem::temp_directory_path() / std::string(product::kSlug) /
                ("test-" + std::to_string(::getpid()) + ".sock");
    auto started = server.start(path);
    if (!started) MESSAGE(started.error().toString());
    REQUIRE(started);
    struct stat info{};
    REQUIRE(::stat(path.c_str(), &info) == 0);
    CHECK((info.st_mode & 0777) == 0600);
    rpc::Client client;
    REQUIRE(client.connect(path, "test-client", "agent-1"));
    auto added = client.call("counter.add", parse(R"({"value":3})"));
    REQUIRE(added);
    CHECK(added->find("count")->asInt() == 3);
    auto unknown = client.call("counter.missing");
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "command.unknown");
    auto invalid = client.call("counter.add", parse(R"({"value":"wrong"})"));
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "command.invalid_args");
    int fd = rawConnect(path);
    auto premature =
        exchange(fd, R"({"jsonrpc":"2.0","id":1,"method":"counter.add","params":{"value":1}})");
    CHECK(premature.find("error")->find("data")->find("code")->asString() == "rpc.handshake");
    auto hello = exchange(
        fd,
        R"({"jsonrpc":"2.0","id":2,"method":"rpc.hello","params":{"client":"raw","protocol":1}})");
    CHECK(hello.find("result")->find("protocol")->asInt() == 1);
    auto batch = exchange(fd, R"([{"jsonrpc":"2.0","id":3,"method":"counter.add"}])");
    CHECK(batch.find("error")->find("code")->asInt() == -32600);
    auto malformed = exchange(fd, "{");
    CHECK(malformed.find("error")->find("code")->asInt() == -32700);
    for (const auto& frame : {"42", "null"}) {
        auto invalidFrame = exchange(fd, frame);
        CHECK(invalidFrame.find("error")->find("code")->asInt() == -32600);
        CHECK(invalidFrame.find("id")->isNull());
    }
    for (const auto& params : {"[]", "7"}) {
        auto invalidParams = exchange(
            fd, std::string(R"({"jsonrpc":"2.0","id":7,"method":"counter.add","params":)") +
                    params + "}");
        CHECK(invalidParams.find("error")->find("code")->asInt() == -32602);
        CHECK(invalidParams.find("error")->find("data")->find("code")->asString() ==
              "rpc.invalid_params");
    }
    auto sub = exchange(
        fd,
        R"({"jsonrpc":"2.0","id":4,"method":"rpc.subscribe","params":{"topics":["session.changed"],"filter":{"branch":"main"}}})");
    CHECK(sub.find("result")->find("subscription")->asString() == "sub_1");
    queue.post(
        [&] { server.publish("session.changed", parse(R"({"branch":"main","version":2})")); });
    auto event = readFrame(fd);
    CHECK(event.find("method")->asString() == "event");
    CHECK(event.find("params")->find("topic")->asString() == "session.changed");
    CHECK(event.find("params")->find("seq")->asInt() == 1);
    auto unsub = exchange(
        fd,
        R"({"jsonrpc":"2.0","id":5,"method":"rpc.unsubscribe","params":{"subscription":"sub_1"}})");
    CHECK(unsub.find("result") != nullptr);
    auto bulk = exchange(
        fd, R"({"jsonrpc":"2.0","id":6,"method":"rpc.subscribe","params":{"topics":["bulk"]}})");
    CHECK(bulk.find("result") != nullptr);
    std::promise<void> published;
    auto publishedFuture = published.get_future();
    queue.post([&] {
        json::Value payload = json::Value::object();
        payload["data"] = std::string(8192, 'x');
        for (int i = 0; i < 512; ++i) server.publish("bulk", payload);
        published.set_value();
    });
    publishedFuture.wait();
    std::string pending;
    bool dropped = false;
    for (int i = 0; i < 512; ++i) {
        auto message = readFrameFast(fd, pending);
        if (message.find("method") && message.find("method")->asString() == "event.dropped") {
            CHECK(message.find("params")->find("count")->asInt() > 0);
            dropped = true;
            break;
        }
    }
    CHECK(dropped);
    auto large = client.call("counter.blob", parse(R"({"value":5242880})"));
    REQUIRE(large);
    CHECK(large->find("data")->asString().size() == 5242880);
    auto tooLarge = client.call("counter.blob", parse(R"({"value":17825792})"));
    REQUIRE_FALSE(tooLarge);
    CHECK(tooLarge.error().code == "rpc.response_too_large");
    int slow = rawConnect(path);
    auto slowHello = exchange(
        slow,
        R"({"jsonrpc":"2.0","id":1,"method":"rpc.hello","params":{"client":"slow","protocol":1}})");
    CHECK(slowHello.find("result") != nullptr);
    std::string slowRequest =
        R"({"jsonrpc":"2.0","id":2,"method":"counter.blob","params":{"value":5242880}})";
    slowRequest += '\n';
    REQUIRE(::send(slow, slowRequest.data(), slowRequest.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(slowRequest.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    std::string slowPending;
    auto slowResult = readFrameFast(slow, slowPending);
    CHECK(slowResult.find("result")->find("data")->asString().size() == 5242880);
    ::close(slow);
    ::close(fd);
    client.close();
    server.stop();
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("socket ownership survives competing bind and backpressure bounds posted work") {
    auto commands = registry();
    Host firstHost, secondHost;
    cmd::Dispatcher firstDispatcher(commands, firstHost), secondDispatcher(commands, secondHost);
    ModelQueue firstQueue, secondQueue;
    rpc::Server first(commands, firstDispatcher,
                      [&](auto task) { firstQueue.post(std::move(task)); });
    rpc::Server second(commands, secondDispatcher,
                       [&](auto task) { secondQueue.post(std::move(task)); });
    auto path = std::filesystem::temp_directory_path() / std::string(product::kSlug) /
                ("race-" + std::to_string(::getpid()) + ".sock");
    Status a, b;
    std::thread left([&] { a = first.start(path); });
    std::thread right([&] { b = second.start(path); });
    left.join();
    right.join();
    CHECK(static_cast<bool>(a) != static_cast<bool>(b));
    CHECK((a ? b.error().code : a.error().code) == "rpc.address_in_use");
    rpc::Client client;
    REQUIRE(client.connect(path, "race-test"));
    auto result = client.call("counter.add", parse(R"({"value":1})"));
    REQUIRE(result);
    CHECK(result->find("count")->asInt() == 1);
    client.close();
    first.stop();
    second.stop();
    CHECK_FALSE(std::filesystem::exists(path));

    // Leave the model thread blocked while the I/O thread reads pipelined calls.
    REQUIRE(first.start(path));
    int fd = rawConnect(path);
    auto hello = exchange(
        fd,
        R"({"jsonrpc":"2.0","id":1,"method":"rpc.hello","params":{"client":"raw","protocol":1}})");
    (void)hello;
    std::promise<void> hold;
    auto blocked = hold.get_future().share();
    firstQueue.post([blocked] { blocked.wait(); });
    std::string batch;
    for (int i = 0; i < 64; ++i)
        batch += R"({"jsonrpc":"2.0","id":2,"method":"counter.add","params":{"value":1}})"
                 "\n";
    REQUIRE(::send(fd, batch.data(), batch.size(), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(batch.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    {
        std::lock_guard lock(firstQueue.mutex);
        CHECK(firstQueue.tasks.size() <= 16);
    }
    hold.set_value();
    ::close(fd);
    first.stop();
}

TEST_CASE("large pipelined frames stop at the per-peer posted byte limit") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    ModelQueue queue;
    rpc::Server server(commands, dispatcher, [&](auto task) { queue.post(std::move(task)); });
    auto path = std::filesystem::temp_directory_path() / std::string(product::kSlug) /
                ("large-" + std::to_string(::getpid()) + ".sock");
    REQUIRE(server.start(path));
    int fd = rawConnect(path);
    auto hello = exchange(
        fd,
        R"({"jsonrpc":"2.0","id":1,"method":"rpc.hello","params":{"client":"large","protocol":1}})");
    REQUIRE(hello.find("result") != nullptr);
    std::promise<void> release;
    auto gate = release.get_future().share();
    queue.post([gate] { gate.wait(); });
    std::string frame =
        R"({"jsonrpc":"2.0","id":2,"method":"counter.add","params":{"value":1,"pad":")";
    frame += std::string(10 * 1024 * 1024, 'x');
    frame += R"("}})"
             "\n";
    std::atomic<bool> sentAll = true;
    std::thread sender([&] {
        for (int i = 0; i < 4; ++i) {
            std::size_t sent = 0;
            while (sent < frame.size()) {
                auto n = ::send(fd, frame.data() + sent, frame.size() - sent, MSG_NOSIGNAL);
                if (n < 0 && errno == EINTR) continue;
                if (n <= 0) {
                    sentAll = false;
                    return;
                }
                sent += static_cast<std::size_t>(n);
            }
        }
    });
    std::size_t posted = 0;
    for (int i = 0; i < 250; ++i) {
        {
            std::lock_guard lock(queue.mutex);
            posted = queue.tasks.size();
        }
        if (posted >= 3) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    // The model thread is held, so the I/O thread must eventually post. Wait
    // for it (bounded) rather than assuming how long a 10 MiB frame takes to
    // read and parse on this machine.
    for (int i = 0; i < 750 && posted < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        std::lock_guard lock(queue.mutex);
        posted = queue.tasks.size();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));  // backpressure settles
    {
        std::lock_guard lock(queue.mutex);
        CHECK(queue.tasks.size() >= 1);
        CHECK(queue.tasks.size() <= 3);
    }
    release.set_value();
    sender.join();
    CHECK(sentAll);
    ::close(fd);
    server.stop();
}

// Embedded in another loop (the GUI's): pump() runs what is due without
// blocking and reports the next deadline; the waker fires for posts and timer
// changes from any thread, so the owner's wait can be interrupted.
TEST_CASE("model-loop pump runs due work without blocking and wakes its owner") {
    rpc::ModelLoop loop;
    std::atomic<int> wakes{0};
    loop.setWaker([&] { ++wakes; });
    CHECK(loop.pump() == -1);  // nothing scheduled
    int ran = 0;
    std::thread poster([&] { loop.post([&] { ++ran; }); });
    poster.join();
    CHECK(wakes.load() >= 1);
    CHECK(loop.pump() == -1);
    CHECK(ran == 1);
    int ticks = 0;
    const int before = wakes.load();
    auto timer = loop.every(std::chrono::milliseconds(20), [&] { ++ticks; });
    CHECK(wakes.load() > before);
    const int next = loop.pump();
    CHECK(next >= 0);
    CHECK(next <= 20);
    CHECK(ticks == 0);  // not due yet
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    CHECK(loop.pump() >= 0);
    CHECK(ticks == 1);
    timer.cancel();
    CHECK(loop.pump() == -1);
    CHECK_FALSE(loop.stopRequested());
}

// The service timers run on the model thread between posted tasks, without a
// busy wait (the loop's poll gets the nearest deadline as its timeout).
TEST_CASE("model-loop timers run on the model thread in order and stop on cancel") {
    rpc::ModelLoop loop;
    std::atomic<int> fast{0}, slow{0};
    std::atomic<bool> wrongThread{false};
    std::atomic<bool> taskRan{false};
    std::mutex mutex;
    std::vector<std::string> order;
    std::thread::id model;
    auto record = [&](const char* what) {
        std::lock_guard lock(mutex);
        order.emplace_back(what);
    };
    auto onModelThread = [&] {
        if (std::this_thread::get_id() != model) wrongThread = true;
    };
    auto fastTimer = loop.every(std::chrono::milliseconds(10), [&] {
        onModelThread();
        ++fast;
        record("timer-10");
    });
    auto slowTimer = loop.every(std::chrono::milliseconds(30), [&] {
        onModelThread();
        ++slow;
        record("timer-30");
    });
    std::thread runner([&] {
        model = std::this_thread::get_id();
        loop.run();
    });
    loop.post([&] {  // a posted task shares the thread, between the timers
        onModelThread();
        record("task");
        taskRan = true;
    });
    auto waitFor = [&](auto predicate, int milliseconds = 3000) {
        for (int waited = 0; waited < milliseconds && !predicate(); waited += 5)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return predicate();
    };
    REQUIRE(waitFor([&] { return fast >= 4 && slow >= 2 && taskRan.load(); }));
    CHECK_FALSE(wrongThread.load());
    {
        std::lock_guard lock(mutex);
        CHECK(order.front() == "task");  // posted immediately; the first timer is due later
        const auto firstFast = std::find(order.begin(), order.end(), "timer-10");
        const auto firstSlow = std::find(order.begin(), order.end(), "timer-30");
        CHECK(firstFast < firstSlow);
    }
    fastTimer.cancel();
    const int fastAtCancel = fast.load();
    const int slowBefore = slow.load();
    REQUIRE(waitFor([&] { return slow >= slowBefore + 3; }));
    CHECK(fast.load() <= fastAtCancel + 1);  // at most one already collected
    CHECK_FALSE(fastTimer.active());
    slowTimer.cancel();
    CHECK_FALSE(slowTimer.active());
    ::raise(SIGTERM);  // the loop's wait is bounded but ends on a signal
    runner.join();
    CHECK(slow.load() <= slowBefore + 4);  // cancelled: no further ticks
}
