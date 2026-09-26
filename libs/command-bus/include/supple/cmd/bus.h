#pragma once

#include <supple/base/result.h>
#include <supple/json/json.h>
#include <supple/schema/schema.h>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <typeindex>
#include <unordered_map>
#include <vector>

namespace supple::cmd {

enum class Kind : std::uint8_t { Query, Edit, Action };
enum class Exposure : std::uint8_t { McpTool, McpExec, Internal };
struct ErrorSpec {
    std::string code, when, hint;
};
struct Example {
    std::string title;
    json::Value args, result;
    std::string fixture;
};
struct Origin {
    enum class Type : std::uint8_t { Ui, Lua, Mcp, Rpc, Test, Internal } type = Type::Internal;
    std::string client, agent;
};
using Warning = schema::Warning;

class Context {
  public:
    explicit Context(Origin origin = {}) : origin_(std::move(origin)) {}
    const Origin& origin() const { return origin_; }
    template <class S>
    void provide(S& service) {
        services_[std::type_index(typeid(S))] = {&service, !std::is_const_v<S>};
    }
    template <class S>
    S& service() const {
        auto it = services_.find(std::type_index(typeid(S)));
        assert(it != services_.end());
        assert(std::is_const_v<S> || it->second.mutableAccess);
        return *static_cast<S*>(const_cast<void*>(it->second.pointer));
    }
    void warn(std::string code, std::string message, std::string path = {}) {
        warnings_.push_back({std::move(code), std::move(message), std::move(path)});
    }
    const std::vector<Warning>& warnings() const { return warnings_; }
    void setResolvedArgs(json::Value value) { resolvedArgs_ = std::move(value); }
    const std::optional<json::Value>& resolvedArgs() const { return resolvedArgs_; }

  private:
    Origin origin_;
    struct ServiceEntry {
        const void* pointer;
        bool mutableAccess;
    };
    std::unordered_map<std::type_index, ServiceEntry> services_;
    std::vector<Warning> warnings_;
    std::optional<json::Value> resolvedArgs_;
};
struct NoArgs {};
// A reference is either a stable ID or a selector object. The host resolves
// selectors and supplies ambiguity errors with candidate IDs.
struct Ref {
    json::Value value;
};
class RefResolver {
  public:
    virtual ~RefResolver() = default;
    virtual Result<std::string> resolve(std::string_view kind, const Ref&) = 0;
};
schema::Schema<Ref> refSchema();
Result<std::string> resolveRef(Context&, std::string_view kind, const Ref&);

template <class Args, class Res>
struct Command {
    std::string name;
    int version = 1;
    Kind kind = Kind::Query;
    Exposure exposure = Exposure::McpExec;
    std::string summary, description;
    std::vector<std::string> tags;
    schema::Schema<Args> args;
    schema::Schema<Res> result;
    std::vector<ErrorSpec> errors;
    std::vector<Example> examples;
    // Optional host-side selector resolution after schema validation. Mutate Args to IDs.
    std::function<Status(Args&, Context&)> resolve;
    std::function<Result<Res>(const Args&, Context&)> run;
};

struct CommandInfo {
    std::string name;
    int version = 1;
    Kind kind = Kind::Query;
    Exposure exposure = Exposure::McpExec;
    std::string summary, description;
    std::vector<std::string> tags;
    json::Value argsSchema, resultSchema;
    std::vector<ErrorSpec> errors;
    std::vector<Example> examples;
    std::function<Result<json::Value>(const json::Value&, Context&)> invoke;
    std::function<Status(const json::Value&)> validate;
    std::function<Status(const json::Value&)> validateResult;
};

class Registry {
  public:
    Registry();
    template <class Args, class Res>
    void add(Command<Args, Res> command) {
        assert(command.run);
        assert(command.exposure == Exposure::Internal || !command.examples.empty());
        auto args = command.args;
        auto result = command.result;
        CommandInfo info;
        info.name = std::move(command.name);
        info.version = command.version;
        info.kind = command.kind;
        info.exposure = command.exposure;
        info.summary = std::move(command.summary);
        info.description = std::move(command.description);
        info.tags = std::move(command.tags);
        info.argsSchema = args.jsonSchema();
        info.resultSchema = result.jsonSchema();
        info.errors = std::move(command.errors);
        info.examples = std::move(command.examples);
        info.validate = [args](const json::Value& value) { return args.validate(value); };
        info.validateResult = [result](const json::Value& value) { return result.validate(value); };
        info.invoke = [args, result, resolve = std::move(command.resolve),
                       run = std::move(command.run)](const json::Value& value,
                                                     Context& context) -> Result<json::Value> {
            std::vector<schema::Warning> warnings;
            auto decoded = args.decode(value, {}, &warnings);
            for (const auto& warning : warnings)
                context.warn(warning.code, warning.message, warning.path);
            if (!decoded) return decoded.error();
            if (resolve) {
                SUPPLE_TRY(resolve(*decoded, context));
                context.setResolvedArgs(args.encode(*decoded));
            }
            auto output = run(*decoded, context);
            if (!output) return output.error();
            return result.encode(*output);
        };
        addInfo(std::move(info));
    }
    const CommandInfo* find(std::string_view name, int version = 0) const;
    std::vector<const CommandInfo*> all() const;
    Error unknown(std::string_view name) const;
    json::Value exportJson() const;

  private:
    void addInfo(CommandInfo info);
    std::map<std::pair<std::string, int>, CommandInfo> commands_;
};

struct Invocation {
    std::string command;
    json::Value args = json::Value::object();
    Origin origin;
};
struct LogRecord {
    std::uint64_t seq = 0, time_ms = 0;
    Origin origin;
    std::string command;
    int version = 1;
    json::Value args;
    bool ok = false;
    std::string errorCode, resultDigest;
    std::uint64_t duration_us = 0;
};

// Workers update jobs through this thread-safe store. Events are queued until
// drainEvents() is called on the model thread, so Host::emit stays model-thread only.
class JobManager {
  public:
    std::string create();
    bool cancelled(std::string_view id) const;
    Status progress(std::string_view id, double fraction, std::string message,
                    json::Value extra = json::Value::object());
    Status complete(std::string_view id, json::Value result);
    Status fail(std::string_view id, Error error);
    Status cancel(std::string_view id);
    Result<json::Value> status(std::string_view id) const;
    Result<json::Value> wait(std::string_view id, double timeoutSeconds);
    json::Value list() const;
    std::vector<std::pair<std::string, json::Value>> drainEvents();

  private:
    struct Job {
        std::string id;
        std::string status = "running";
        json::Value progress = json::Value::object();
        json::Value result;
        Error error;
        bool cancelRequested = false;
        std::chrono::steady_clock::time_point lastProgressEvent{};
    };
    static json::Value snapshot(const Job& job);
    void prune();
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::map<std::string, Job> jobs_;
    std::vector<std::string> order_;
    std::vector<std::pair<std::string, json::Value>> events_;
    std::uint64_t next_ = 1;
};

class Dispatcher;
class Host {
  public:
    virtual ~Host() = default;
    virtual Context makeContext(const Origin&) = 0;
    virtual Result<std::uint64_t> beginTransaction(const Origin&, std::string_view label) = 0;
    virtual Status commitTransaction(std::uint64_t) = 0;
    virtual void rollbackTransaction(std::uint64_t) = 0;
    virtual void log(const LogRecord&) = 0;
    virtual void emit(std::string_view topic, json::Value payload) = 0;
    virtual bool actionTransactional(std::string_view) const { return false; }
    virtual std::uint64_t clockMs() const;
    virtual void setupFixture(Dispatcher&, std::string_view) {}
};

class Dispatcher {
  public:
    Dispatcher(const Registry& registry, Host& host) : registry_(registry), host_(host) {}
    Result<json::Value> execute(const Invocation& invocation);
    JobManager& jobs() { return jobs_; }
    void flushJobEvents();

  private:
    Result<json::Value> dispatch(const Invocation&, bool inBatch, bool preResolved = false);
    Result<json::Value> batch(const json::Value&, const Origin&);
    const Registry& registry_;
    Host& host_;
    JobManager jobs_;
    std::uint64_t sequence_ = 0;
};

using FixtureFactory = std::function<std::unique_ptr<Host>(std::string_view)>;
Status runExamples(const Registry&, const FixtureFactory&);
json::Value mcpToolList(const Registry&);
std::string markdownReference(const Registry&);

}  // namespace supple::cmd
