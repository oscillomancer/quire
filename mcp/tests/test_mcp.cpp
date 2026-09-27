#include <doctest/doctest.h>
#include <supple/mcp/server.h>
#include <supple/product.h>
#include <unistd.h>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <sstream>
#include <thread>
using namespace supple;
namespace {
struct Count {
    int count = 0;
};
struct Args {
    int value = 0;
};
struct Host : cmd::Host {
    int count = 0, prior = 0;
    cmd::Origin last;
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
    void emit(std::string_view, json::Value) override {}
};
cmd::Registry registry() {
    cmd::Registry r;
    cmd::Command<Args, Count> command;
    command.name = "counter.add";
    command.kind = cmd::Kind::Edit;
    command.exposure = cmd::Exposure::McpTool;
    command.summary = "Add to counter.";
    command.args =
        schema::object<Args>("args").field("value", &Args::value, schema::integerOf<int>()).build();
    command.result = schema::object<Count>("count")
                         .field("count", &Count::count, schema::integerOf<int>())
                         .build();
    command.examples = {{"Add", json::parse(R"({"value":1})").value(),
                         json::parse(R"({"count":1})").value(), "empty"}};
    command.run = [](const Args& args, cmd::Context& ctx) -> Result<Count> {
        return Count{ctx.service<int>() += args.value};
    };
    r.add(std::move(command));
    return r;
}
json::Value parse(std::string_view input) { return json::parse(input).value(); }
json::Value request(mcp::Server& server, std::string_view input) {
    auto output = server.handle(parse(input));
    REQUIRE(output);
    return *output;
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
}  // namespace
TEST_CASE("in-process MCP negotiates, lists and executes with structured errors") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    mcp::Server server(commands, dispatcher);
    auto early = request(server, R"({"jsonrpc":"2.0","id":1,"method":"tools/list"})");
    CHECK(early.find("error")->find("code")->asInt() == -32000);
    auto initialized = request(
        server,
        R"({"jsonrpc":"2.0","id":2,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"unit-client"}}})");
    CHECK(initialized.find("result")->find("protocolVersion")->asString() == "2025-06-18");
    auto listed = request(server, R"({"jsonrpc":"2.0","id":3,"method":"tools/list"})");
    bool found = false;
    for (const auto& tool : listed.find("result")->find("tools")->asArray())
        if (tool.find("name")->asString() == "counter_add") found = true;
    CHECK(found);
    auto success = request(
        server,
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"counter_add","arguments":{"value":3}}})");
    CHECK(success.find("result")->find("structuredContent")->find("count")->asInt() == 3);
    CHECK(host.last.type == cmd::Origin::Type::Mcp);
    CHECK(host.last.client == "unit-client");
    auto failure = request(
        server,
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"counter_add","arguments":{"value":"bad"}}})");
    CHECK(failure.find("result")->find("isError")->asBool());
    CHECK(failure.find("result")->find("structuredContent")->find("code")->asString() ==
          "command.invalid_args");
    auto described = request(
        server,
        R"({"jsonrpc":"2.0","id":8,"method":"tools/call","params":{"name":"describe","arguments":{"command":"counter.add"}}})");
    CHECK(described.find("result")->find("structuredContent")->find("name")->asString() ==
          "counter.add");
    auto executed = request(
        server,
        R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"exec","arguments":{"command":"counter.add","args":{"value":2}}}})");
    CHECK(executed.find("result")->find("structuredContent")->find("count")->asInt() == 5);
    auto ping = request(server, R"({"jsonrpc":"2.0","id":10,"method":"ping"})");
    CHECK(ping.find("result")->isObject());
    CHECK_FALSE(server.handle(parse(R"({"jsonrpc":"2.0","method":"notifications/initialized"})")));
    auto unknown = request(
        server, R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"wrong"}})");
    CHECK(unknown.find("error")->find("code")->asInt() == -32601);
    auto newer = request(
        server,
        R"({"jsonrpc":"2.0","id":7,"method":"initialize","params":{"protocolVersion":"2026-07-28"}})");
    CHECK(newer.find("result")->find("protocolVersion")->asString() == "2025-11-25");
    std::istringstream input(R"({"jsonrpc":"2.0","id":8,"method":"ping"})"
                             "\n");
    std::ostringstream output;
    CHECK(server.serve(input, output));
    CHECK(parse(output.str()).find("result") != nullptr);
}
TEST_CASE("proxy MCP forwards calls over the Unix control socket") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    ModelQueue model;
    rpc::Server rpcServer(commands, dispatcher, [&](auto task) { model.post(std::move(task)); });
    auto path = std::filesystem::temp_directory_path() / std::string(product::kSlug) /
                ("mcp-test-" + std::to_string(::getpid()) + ".sock");
    auto started = rpcServer.start(path);
    if (!started) MESSAGE(started.error().toString());
    REQUIRE(started);
    rpc::Client client;
    REQUIRE(client.connect(path, "proxy", {}, true));
    auto proxy = mcp::Server::proxy(client);
    REQUIRE(proxy);
    auto initialized = request(
        **proxy,
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","clientInfo":{"name":"remote-client"}}})");
    CHECK(initialized.find("result") != nullptr);
    auto listed = request(**proxy, R"({"jsonrpc":"2.0","id":2,"method":"tools/list"})");
    bool found = false;
    for (const auto& tool : listed.find("result")->find("tools")->asArray())
        if (tool.find("name")->asString() == "counter_add") found = true;
    CHECK(found);
    auto success = request(
        **proxy,
        R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"counter_add","arguments":{"value":5}}})");
    CHECK(success.find("result")->find("structuredContent")->find("count")->asInt() == 5);
    auto failure = request(
        **proxy,
        R"({"jsonrpc":"2.0","id":4,"method":"tools/call","params":{"name":"counter_add","arguments":{"value":"bad"}}})");
    CHECK(failure.find("result")->find("isError")->asBool());
    client.close();
    rpcServer.stop();
    CHECK_FALSE(std::filesystem::exists(path));
}

TEST_CASE("MCP stdio caps an unterminated oversized frame") {
    auto commands = registry();
    Host host;
    cmd::Dispatcher dispatcher(commands, host);
    mcp::Server server(commands, dispatcher);
    std::istringstream input(std::string(rpc::kMaxMessageBytes + 1, 'x'));
    std::ostringstream output;
    REQUIRE(server.serve(input, output));
    auto reply = json::parse(output.str());
    REQUIRE(reply);
    CHECK(reply->find("error")->find("code")->asInt() == -32600);
    CHECK(reply->find("id")->isNull());
}
