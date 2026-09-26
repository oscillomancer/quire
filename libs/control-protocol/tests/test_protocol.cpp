#include <doctest/doctest.h>
#include <poll.h>
#include <supple/product.h>
#include <supple/rpc/protocol.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <condition_variable>
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
    ::close(fd);
    client.close();
    server.stop();
    CHECK_FALSE(std::filesystem::exists(path));
}
