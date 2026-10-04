#include <doctest/doctest.h>
#include <supple/base/environment.h>
#include <supple/cmd/bus.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
using namespace supple;
using namespace supple::cmd;

namespace {
struct Amount {
    int value = 0;
};
struct Count {
    int count = 0;
};
struct Message {
    std::string text;
};
struct Toy {
    int count = 0;
};
struct Target {
    Ref target;
};
struct JobStarted {
    std::string job, status;
};
struct FakeHost : Host, RefResolver {
    Toy toy;
    bool failRollback = false;
    int before = 0;
    int begins = 0, commits = 0, rollbacks = 0;
    std::vector<LogRecord> records;
    std::vector<std::string> events;
    Context makeContext(const Origin& origin) override {
        Context ctx(origin);
        ctx.provide(toy);
        ctx.provide<RefResolver>(*this);
        return ctx;
    }
    Result<std::uint64_t> beginTransaction(const Origin&, std::string_view) override {
        before = toy.count;
        ++begins;
        return static_cast<std::uint64_t>(begins);
    }
    Status commitTransaction(std::uint64_t) override {
        ++commits;
        return {};
    }
    Status rollbackTransaction(std::uint64_t) override {
        toy.count = before;
        ++rollbacks;
        if (failRollback) return Error("test.rollback", "restore failed", "/instances/in_1");
        return okStatus();
    }
    void log(const LogRecord& record) override { records.push_back(record); }
    void emit(std::string_view topic, json::Value) override { events.emplace_back(topic); }
    Result<std::string> resolve(std::string_view kind, const Ref& ref) override {
        if (kind == "toy" && ref.value.isObject() && ref.value.find("name") &&
            ref.value.find("name")->asString() == "counter")
            return std::string("toy_1");
        return Error("ref.ambiguous", "selector did not identify one toy", "/target",
                     "candidates: toy_1");
    }
    bool actionTransactional(std::string_view name) const override { return name == "toy.safe"; }
    std::uint64_t clockMs() const override { return 1234; }
    void setupFixture(Dispatcher& dispatcher, std::string_view name) override {
        if (name == "job_running") CHECK(dispatcher.jobs().create() == "jb_1");
    }
};
Origin testOrigin{Origin::Type::Test, "unit", ""};
json::Value obj(std::string_view source) { return json::parse(source).value(); }
Registry makeRegistry() {
    Registry registry;
    auto amount = schema::object<Amount>("amount")
                      .field("value", &Amount::value, schema::integerOf<int>())
                      .build();
    auto count = schema::object<Count>("count")
                     .field("count", &Count::count, schema::integerOf<int>())
                     .build();
    auto message = schema::object<Message>("message")
                       .field("text", &Message::text, schema::string().minLength(2))
                       .build();
    Command<Amount, Count> add;
    add.name = "toy.add";
    add.kind = Kind::Edit;
    add.exposure = Exposure::McpTool;
    add.summary = "Add to the toy counter.";
    add.description = "Returns the new counter value.";
    add.args = amount;
    add.result = count;
    add.examples = {{"Add three", obj("{\"value\":3}"), obj("{\"count\":3}"), "empty"}};
    add.run = [](const Amount& args, Context& ctx) -> Result<Count> {
        return Count{ctx.service<Toy>().count += args.value};
    };
    registry.add(std::move(add));
    Command<Amount, Count> set;
    set.name = "toy.set";
    set.kind = Kind::Edit;
    set.exposure = Exposure::McpTool;
    set.summary = "Set the toy counter.";
    set.args = amount;
    set.result = count;
    set.examples = {{"Set five", obj("{\"value\":5}"), obj("{\"count\":5}"), "empty"}};
    set.run = [](const Amount& args, Context& ctx) -> Result<Count> {
        return Count{ctx.service<Toy>().count = args.value};
    };
    registry.add(std::move(set));
    Command<NoArgs, Count> get;
    get.name = "toy.get";
    get.summary = "Get the toy counter.";
    get.args = schema::object<NoArgs>("no_args").build();
    get.result = count;
    get.examples = {{"Get zero", obj("{}"), obj("{\"count\":0}"), "empty"}};
    get.run = [](const NoArgs&, Context& ctx) -> Result<Count> {
        return Count{ctx.service<Toy>().count};
    };
    registry.add(std::move(get));
    Command<NoArgs, Count> fail;
    fail.name = "toy.fail";
    fail.kind = Kind::Edit;
    fail.exposure = Exposure::Internal;
    fail.summary = "Fail after changing the toy.";
    fail.args = schema::object<NoArgs>("no_args").build();
    fail.result = count;
    fail.run = [](const NoArgs&, Context& ctx) -> Result<Count> {
        ctx.service<Toy>().count = 999;
        return Error("toy.failed", "deliberate failure", "/value", "try another step");
    };
    registry.add(std::move(fail));
    Command<NoArgs, Count> unsafe;
    unsafe.name = "toy.unsafe";
    unsafe.kind = Kind::Action;
    unsafe.exposure = Exposure::Internal;
    unsafe.summary = "Run an unsafe action.";
    unsafe.args = schema::object<NoArgs>("no_args").build();
    unsafe.result = count;
    unsafe.run = [](const NoArgs&, Context&) -> Result<Count> { return Count{0}; };
    registry.add(std::move(unsafe));
    Command<NoArgs, Message> bad;
    bad.name = "toy.bad";
    bad.exposure = Exposure::Internal;
    bad.summary = "Return an invalid result.";
    bad.args = schema::object<NoArgs>("no_args").build();
    bad.result = message;
    bad.run = [](const NoArgs&, Context&) -> Result<Message> { return Message{"x"}; };
    registry.add(std::move(bad));
    Command<NoArgs, Count> safe;
    safe.name = "toy.safe";
    safe.kind = Kind::Action;
    safe.exposure = Exposure::Internal;
    safe.summary = "Run a safe action.";
    safe.args = schema::object<NoArgs>("no_args").build();
    safe.result = count;
    safe.run = [](const NoArgs&, Context& ctx) -> Result<Count> {
        return Count{ctx.service<Toy>().count};
    };
    registry.add(std::move(safe));
    Command<NoArgs, JobStarted> start;
    start.name = "toy.start";
    start.kind = Kind::Action;
    start.exposure = Exposure::Internal;
    start.summary = "Start a long toy job.";
    start.args = schema::object<NoArgs>("no_args").build();
    start.result = schema::object<JobStarted>("job_started")
                       .field("job", &JobStarted::job, schema::string())
                       .field("status", &JobStarted::status, schema::string())
                       .build();
    start.run = [](const NoArgs&, Context& ctx) -> Result<JobStarted> {
        return JobStarted{ctx.service<JobManager>().create(), "running"};
    };
    registry.add(std::move(start));
    Command<Target, Count> target;
    target.name = "toy.target.get";
    target.exposure = Exposure::Internal;
    target.summary = "Get a toy by reference.";
    target.args =
        schema::object<Target>("target").field("target", &Target::target, refSchema()).build();
    target.result = count;
    target.resolve = [](Target& args, Context& ctx) -> Status {
        auto id = resolveRef(ctx, "toy", args.target);
        if (!id) return id.error();
        args.target.value = *id;
        return {};
    };
    target.run = [](const Target& args, Context& ctx) -> Result<Count> {
        if (args.target.value.asString() != "toy_1") return Error("toy.unknown", "unknown toy");
        return Count{ctx.service<Toy>().count};
    };
    registry.add(std::move(target));
    return registry;
}
Result<json::Value> call(Dispatcher& dispatcher, std::string command,
                         json::Value args = json::Value::object()) {
    return dispatcher.execute({std::move(command), std::move(args), testOrigin});
}
}  // namespace

TEST_CASE("dispatch, versions, validation, results and log") {
    auto registry = makeRegistry();
    FakeHost host;
    Dispatcher dispatcher(registry, host);
    CHECK(call(dispatcher, "toy.ad").error().code == "command.unknown");
    CHECK(call(dispatcher, "toy.ad").error().hint.find("toy.add") != std::string::npos);
    CHECK(call(dispatcher, "toy.add@3").error().code == "command.version");
    CHECK(call(dispatcher, "toy.add@x").error().code == "command.version");
    auto invalid = call(dispatcher, "toy.add", obj("{\"valeu\":2}"));
    REQUIRE_FALSE(invalid);
    CHECK(invalid.error().code == "command.invalid_args");
    CHECK(invalid.error().details.size() == 2);
    auto added = call(dispatcher, "toy.add@1", obj("{\"value\":3}"));
    REQUIRE(added);
    CHECK(added->find("count")->asInt() == 3);
    CHECK(host.begins == 1);
    CHECK(host.commits == 1);
    auto failed = call(dispatcher, "toy.fail");
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "toy.failed");
    CHECK(failed.error().path == "/args/value");
    CHECK(host.toy.count == 3);
    CHECK(host.rollbacks == 1);
    CHECK(call(dispatcher, "toy.bad").error().code == "command.bad_result");
    CHECK(host.records.back().errorCode == "command.bad_result");
    CHECK(host.records.back().time_ms == 1234);
    CHECK(host.records.back().origin.client == "unit");
    CHECK(host.records.back().resultDigest.empty());
}
TEST_CASE("batch atomicity and references") {
    auto registry = makeRegistry();
    FakeHost host;
    Dispatcher dispatcher(registry, host);
    auto input = obj(
        R"({"label":"two steps","steps":[{"command":"toy.add","args":{"value":4},"as":"first"},{"command":"toy.add","args":{"value":"${first.count}"}}]})");
    auto result = call(dispatcher, "batch", input);
    REQUIRE(result);
    CHECK(host.toy.count == 8);
    CHECK(host.begins == 1);
    CHECK(host.commits == 1);
    CHECK(host.records.size() == 3);
    CHECK(host.records[1].args.find("value")->asInt() == 4);
    auto failed =
        call(dispatcher, "batch",
             obj(R"({"steps":[{"command":"toy.set","args":{"value":5}},{"command":"toy.fail"}]})"));
    REQUIRE_FALSE(failed);
    CHECK(failed.error().code == "batch.step_failed");
    CHECK(failed.error().path == "/steps/1");
    CHECK(failed.error().details.front().path.find("/steps/1") == 0);
    CHECK(host.toy.count == 8);
    CHECK(host.rollbacks == 1);
    auto ref = call(
        dispatcher, "batch",
        obj(R"({"steps":[{"command":"toy.set","args":{"value":5}},{"command":"toy.add","args":{"value":"${missing.count}"}}]})"));
    REQUIRE_FALSE(ref);
    CHECK(ref.error().code == "batch.bad_reference");
    CHECK(host.toy.count == 8);
    auto unknown =
        call(dispatcher, "batch",
             obj(R"({"steps":[{"command":"toy.set","args":{"value":5}},{"command":"toy.ad"}]})"));
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "command.unknown");
    CHECK(host.toy.count == 8);
    auto unsafe = call(dispatcher, "batch", obj(R"({"steps":[{"command":"toy.unsafe"}]})"));
    REQUIRE_FALSE(unsafe);
    CHECK(unsafe.error().code == "batch.not_transactional");
    auto safe = call(dispatcher, "batch", obj(R"({"steps":[{"command":"toy.safe"}]})"));
    CHECK(safe);
    json::Value many = json::Value::object();
    many["steps"] = json::Value::array();
    for (int i = 0; i < 257; ++i) many["steps"].push_back(obj("{\"command\":\"toy.get\"}"));
    CHECK(call(dispatcher, "batch", many).error().code == "batch.too_many_steps");
}
TEST_CASE("meta commands, generators and examples") {
    auto registry = makeRegistry();
    FakeHost host;
    Dispatcher dispatcher(registry, host);
    auto listed = call(dispatcher, "registry.list", obj("{\"kind\":\"edit\",\"text\":\"toy\"}"));
    REQUIRE(listed);
    CHECK(listed->find("commands")->size() == 3);
    auto described = call(dispatcher, "registry.describe", obj("{\"command\":\"toy.add\"}"));
    REQUIRE(described);
    CHECK(described->find("name")->asString() == "toy.add");
    auto missingVersion = call(dispatcher, "registry.describe", obj("{\"command\":\"toy.add@9\"}"));
    REQUIRE_FALSE(missingVersion);
    CHECK(missingVersion.error().code == "command.version");
    auto exported = registry.exportJson();
    CHECK(exported.find("schema")->asString() == "supple.command-registry/1.0");
    CHECK(exported.find("commands")->find("toy.add@1") != nullptr);
    auto tools = mcpToolList(registry);
    bool sawAdd = false;
    for (const auto& tool : tools.asArray())
        if (tool.find("name")->asString() == "toy_add") sawAdd = true;
    CHECK(sawAdd);
    CHECK(markdownReference(registry).find("toy.add@1") != std::string::npos);
    CHECK(runExamples(registry, [](std::string_view) { return std::make_unique<FakeHost>(); }));
}
TEST_CASE("job lifecycle, cancellation, timeout and event throttling") {
    auto registry = makeRegistry();
    FakeHost host;
    Dispatcher dispatcher(registry, host);
    auto started = call(dispatcher, "toy.start");
    REQUIRE(started);
    auto id = started->find("job")->asString();
    CHECK(id == "jb_1");
    CHECK(started->find("status")->asString() == "running");
    CHECK(dispatcher.jobs().progress(id, 0.25, "working"));
    CHECK(dispatcher.jobs().progress(id, 0.5, "working"));
    dispatcher.flushJobEvents();
    CHECK(host.events.size() == 1);
    auto timed = call(dispatcher, "job.wait", obj("{\"job\":\"jb_1\",\"timeout_s\":0.01}"));
    REQUIRE(timed);
    CHECK(timed->find("status")->asString() == "running");
    Status workerStatus;
    std::thread worker([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        workerStatus = dispatcher.jobs().complete(id, obj("{\"count\":7}"));
    });
    auto done = call(dispatcher, "job.wait", obj("{\"job\":\"jb_1\",\"timeout_s\":1}"));
    worker.join();
    CHECK(workerStatus);
    REQUIRE(done);
    CHECK(done->find("status")->asString() == "done");
    CHECK(done->find("result")->find("count")->asInt() == 7);
    CHECK(host.events.back() == "job.done");
    // A too-long timeout is clamped, not rejected, and says so; the wait above
    // returned on time, so it carries no clamp field.
    CHECK(timed->find("timeout_clamped_s") == nullptr);
    auto clamped = call(dispatcher, "job.wait", obj("{\"job\":\"jb_1\",\"timeout_s\":300}"));
    REQUIRE(clamped);
    CHECK(clamped->find("status")->asString() == "done");
    CHECK(clamped->find("timeout_clamped_s")->asDouble() == kMaxWaitSeconds);
    auto negative = call(dispatcher, "job.wait", obj("{\"job\":\"jb_1\",\"timeout_s\":-1}"));
    REQUIRE_FALSE(negative);
    // The dispatcher wraps argument validation; the cause is in the details.
    CHECK(negative.error().code == "command.invalid_args");
    REQUIRE(negative.error().details.size() == 1);
    CHECK(negative.error().details.front().code == "schema.out_of_range");
    CHECK_FALSE(call(dispatcher, "job.wait", obj("{\"job\":\"jb_1\",\"timeout_s\":\"soon\"}")));
    CHECK(dispatcher.jobs().wait("jb_1", 1e9)->find("timeout_clamped_s")->asDouble() == 30.0);
    CHECK_FALSE(dispatcher.jobs().wait("jb_1", -1.0));
    auto second = dispatcher.jobs().create();
    CHECK(call(dispatcher, "job.cancel", obj("{\"job\":\"jb_2\"}")));
    CHECK(dispatcher.jobs().cancelled(second));
    CHECK(dispatcher.jobs().status(second)->find("status")->asString() == "cancelled");
    CHECK_FALSE(dispatcher.jobs().complete(second, json::Value::object()));
    bool published = false;
    CHECK_FALSE(
        dispatcher.jobs().completeWith(second, json::Value::object(), [&] { published = true; }));
    CHECK_FALSE(published);
    CHECK(call(dispatcher, "job.status", obj("{\"job\":\"missing\"}")).error().code ==
          "job.unknown");
    CHECK(call(dispatcher, "job.list"));
    auto third = dispatcher.jobs().create();
    CHECK(dispatcher.jobs().fail(third, Error("toy.failed", "oops")));
    CHECK(dispatcher.jobs().status(third)->find("error")->find("code")->asString() == "toy.failed");
    auto fourth = dispatcher.jobs().create();
    CHECK(dispatcher.jobs().completeWith(fourth, json::Value::object(), [&] { published = true; }));
    CHECK(published);
}

TEST_CASE("generated registry, tools and reference match golden files") {
    auto registry = makeRegistry();
    const auto directory = std::filesystem::path(__FILE__).parent_path() / "golden";
    const std::pair<std::string, std::string> outputs[] = {
        {"registry.json", json::writeCanonical(registry.exportJson())},
        {"mcp-tools.json", json::writeCanonical(mcpToolList(registry))},
        {"commands.md", markdownReference(registry)},
    };
    for (const auto& [name, output] : outputs) {
        const auto file = directory / name;
        if (supple::environmentFlagEnabled("SUPPLE_UPDATE_BUS_GOLDENS")) {
            std::ofstream out(file, std::ios::binary);
            out << output;
            REQUIRE(out.good());
        }
        std::ifstream in(file, std::ios::binary);
        REQUIRE(in.good());
        std::string expected((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        CHECK(expected == output);
    }
}

TEST_CASE("references resolve before handlers and logs store IDs") {
    auto registry = makeRegistry();
    FakeHost host;
    Dispatcher dispatcher(registry, host);
    auto result = call(dispatcher, "toy.target.get", obj(R"({"target":{"name":"counter"}})"));
    REQUIRE(result);
    CHECK(host.records.back().args.find("target")->asString() == "toy_1");
    auto ambiguous = call(dispatcher, "toy.target.get", obj(R"({"target":{"name":"other"}})"));
    REQUIRE_FALSE(ambiguous);
    CHECK(ambiguous.error().code == "ref.ambiguous");
    CHECK(ambiguous.error().hint.find("toy_1") != std::string::npos);
}

TEST_CASE("transaction rollback failure preserves the initiating error") {
    Registry registry;
    FakeHost host;
    Dispatcher dispatcher(registry, host);
    host.failRollback = true;
    auto result = dispatcher.transact("rollback", Origin{}, []() -> Result<json::Value> {
        return Error("test.command", "command failed");
    });
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "test.rollback");
    REQUIRE(result.error().details.size() == 1);
    CHECK(result.error().details[0].code == "test.command");
}
