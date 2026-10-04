// A test host for lua-bus: a handful of commands covering every schema kind,
// one-level transactions that really roll back, and an event queue.
#pragma once

#include <doctest/doctest.h>
#include <supple/cmd/bus.h>
#include <supple/lua/runtime.h>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace supple::lua::tests {

struct Toy {
    std::int64_t count = 0;
    std::string origin;  // origin text of the last edit
};

inline Toy& toy(cmd::Context& context) { return context.service<Toy>(); }

struct Point {
    std::int64_t x = 0;
    std::optional<std::string> label;
};

using Move = std::variant<Point, std::string>;

enum class Mode : std::uint8_t { Off, Fast, Slow };

struct AddArgs {
    std::int64_t value = 0;
};
struct CountResult {
    std::int64_t count = 0;
};
struct GetResult {
    std::int64_t count = 0;
    std::string name;
    bool flag = false;
};
struct FailArgs {
    std::int64_t value = 0;
};

// Every schema kind the bindings have to convert.
struct Echo {
    std::string text;
    std::int64_t n = 0;
    double freq = 0;
    bool flag = false;
    Mode mode = Mode::Off;
    std::vector<std::int64_t> ints;
    std::vector<Point> points;
    std::map<std::string, double> gains;
    Point point;
    std::optional<std::string> note;
    json::Value anything;
    std::optional<json::Value> maybe;
    Move move;
    cmd::Ref ref;
};

struct ToyHost : cmd::Host {
    Toy toy;
    int begins = 0, commits = 0, rollbacks = 0;
    bool open = false;
    std::int64_t saved = 0;
    std::vector<cmd::LogRecord> records;
    std::vector<std::pair<std::string, json::Value>> events;
    // When set, emitted events queue in the runtime and commandFinished()
    // delivers them: the wiring the engine and the sandbox use.
    lua::Runtime* runtime = nullptr;
    std::unique_ptr<lua::Runtime> ownedRuntime;  // for hosts that own theirs (fixtures)

    void useScripts(lua::Runtime& scripts) { runtime = &scripts; }
    // A host whose fixture cannot know the registry or the dispatcher: a
    // runtime with no commands, enough for scripts that only compute.
    void attachOwnScripts() {
        ownedRuntime = std::make_unique<lua::Runtime>();
        runtime = ownedRuntime.get();
    }

    cmd::Context makeContext(const cmd::Origin& origin) override {
        cmd::Context context(origin);
        context.provide(toy);
        if (runtime) context.provide<lua::Runtime>(*runtime);
        return context;
    }
    Result<std::uint64_t> beginTransaction(const cmd::Origin& origin, std::string_view) override {
        // A host transaction never nests: transact() and batch() guard it.
        if (open) return Error("test.nested", "nested transaction");
        open = true;
        saved = toy.count;
        toy.origin = originText(origin);
        return static_cast<std::uint64_t>(++begins);
    }
    Status commitTransaction(std::uint64_t) override {
        open = false;
        ++commits;
        // Like the engine: a committed edit is an event.
        json::Value event = json::Value::object();
        event["count"] = toy.count;
        emit("session.changed", std::move(event));
        return {};
    }
    void rollbackTransaction(std::uint64_t) override {
        open = false;
        toy.count = saved;
        ++rollbacks;
    }
    void log(const cmd::LogRecord& record) override { records.push_back(record); }
    void emit(std::string_view topic, json::Value payload) override {
        events.emplace_back(std::string(topic), std::move(payload));
        if (runtime) runtime->event(topic, events.back().second);
    }
    void commandFinished() override {
        if (runtime) runtime->deliver();
    }
    std::uint64_t clockMs() const override { return 1000; }
    // The bus's own job.* examples use this fixture.
    void setupFixture(cmd::Dispatcher& dispatcher, std::string_view fixture) override {
        if (fixture == "job_running") (void)dispatcher.jobs().create();
    }
    static std::string originText(const cmd::Origin& origin) {
        static constexpr const char* kTypes[] = {"ui", "lua", "mcp", "rpc", "test", "internal"};
        std::string text = kTypes[static_cast<int>(origin.type)];
        if (!origin.client.empty()) text += ":" + origin.client;
        return text;
    }
};

inline cmd::Registry toyRegistry(bool withScripts = true) {
    using namespace schema;
    cmd::Registry registry;

    cmd::Command<AddArgs, CountResult> add;
    add.name = "toy.add";
    add.kind = cmd::Kind::Edit;
    add.exposure = cmd::Exposure::McpTool;
    add.summary = "Add to the toy counter.";
    add.description = "Returns the new counter value.";
    add.args = object<AddArgs>("add_args")
                   .field("value", &AddArgs::value, integer().doc("How much to add."))
                   .build();
    add.result =
        object<CountResult>("count_result").field("count", &CountResult::count, integer()).build();
    add.examples = {{"Add three", json::parse(R"({"value": 3})").value(),
                     json::parse(R"({"count": 3})").value(), "empty"}};
    add.run = [](const AddArgs& args, cmd::Context& context) -> Result<CountResult> {
        std::int64_t next;
        if (__builtin_add_overflow(toy(context).count, args.value, &next))
            return Error("toy.overflow", "counter addition exceeds int64", "/value",
                         "use a smaller value");
        toy(context).count = next;
        return CountResult{toy(context).count};
    };
    registry.add(std::move(add));

    cmd::Command<cmd::NoArgs, GetResult> get;
    get.name = "toy.get";
    get.summary = "Read the toy counter.";
    get.description = "A query: it changes nothing.";
    get.args = object<cmd::NoArgs>("no_args").build();
    get.result = object<GetResult>("get_result")
                     .field("count", &GetResult::count, integer())
                     .field("name", &GetResult::name, string())
                     .field("flag", &GetResult::flag, boolean())
                     .build();
    get.examples = {{"Read zero", json::Value::object(),
                     json::parse(R"({"count": 0, "name": "toy", "flag": true})").value(), "empty"}};
    get.run = [](const cmd::NoArgs&, cmd::Context& context) -> Result<GetResult> {
        return GetResult{toy(context).count, "toy", true};
    };
    registry.add(std::move(get));

    cmd::Command<FailArgs, CountResult> fail;
    fail.name = "toy.fail";
    fail.kind = cmd::Kind::Edit;
    fail.exposure = cmd::Exposure::McpExec;
    fail.summary = "Always fails, to test error paths.";
    fail.description = "Returns a structured error and changes nothing.";
    fail.args = object<FailArgs>("fail_args").field("value", &FailArgs::value, integer()).build();
    fail.result =
        object<CountResult>("count_result").field("count", &CountResult::count, integer()).build();
    fail.examples = {{"Nothing to fail about", json::parse(R"({"value": 0})").value(),
                      json::parse(R"({"count": 0})").value(), "empty"}};
    fail.run = [](const FailArgs& args, cmd::Context& context) -> Result<CountResult> {
        if (args.value == 0) return CountResult{toy(context).count};  // the example passes
        return Error("toy.busy", "the toy is busy", "/value",
                     "use 0 .. 9, not " + std::to_string(args.value));
    };
    registry.add(std::move(fail));

    auto point = object<Point>("point")
                     .field("x", &Point::x, integer())
                     .field("label", &Point::label, string())
                     .build();
    auto move = variant<Move>("move", "kind")
                    .alternative("point", point)
                    .alternative("name", string())
                    .build();
    cmd::Command<Echo, Echo> echo;
    echo.name = "toy.echo";
    echo.kind = cmd::Kind::Edit;
    echo.exposure = cmd::Exposure::McpExec;
    echo.summary = "Echo the arguments back, with every schema kind.";
    echo.description = "The result is the decoded arguments re-encoded: a faithful round trip.";
    echo.args =
        object<Echo>("echo")
            .field("text", &Echo::text, string())
            .field("n", &Echo::n, integer())
            .field("freq", &Echo::freq, number().unit(units::Unit::Hz).range(1, 20000))
            .field("flag", &Echo::flag, boolean())
            .field(
                "mode", &Echo::mode,
                enumeration<Mode>({{Mode::Off, "off"}, {Mode::Fast, "fast"}, {Mode::Slow, "slow"}}))
            .field("ints", &Echo::ints, arrayOf(integer()))
            .field("points", &Echo::points, arrayOf(point))
            .field("gains", &Echo::gains, mapOf(number()))
            .field("point", &Echo::point, point)
            .field("note", &Echo::note, string())
            .field("anything", &Echo::anything, any())
            .field("maybe", &Echo::maybe, any())
            .field("move", &Echo::move, move)
            .field("ref", &Echo::ref, cmd::refSchema())
            .build();
    echo.result = echo.args;
    // The example carries every required member; the echoed result is the
    // same JSON (the typed decode re-encodes it unchanged).
    const json::Value echoExample = json::parse(R"({
        "text": "t", "n": 1, "freq": 2, "flag": false, "mode": "off",
        "ints": [1], "points": [{"x": 0}], "gains": {"a": 0.5}, "point": {"x": 0},
        "anything": false, "move": {"kind": "point", "x": 0}, "ref": "tr_1"})")
                                        .value();
    echo.examples = {{"Echo every kind", echoExample, echoExample, "empty"}};
    echo.run = [](const Echo& args, cmd::Context&) -> Result<Echo> { return args; };
    registry.add(std::move(echo));

    if (withScripts) registerScriptCommands(registry);
    return registry;
}

}  // namespace supple::lua::tests
