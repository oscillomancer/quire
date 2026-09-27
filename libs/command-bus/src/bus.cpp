#include <supple/base/closest.h>
#include <supple/cmd/bus.h>
#include <supple/product.h>
#include <algorithm>
#include <charconv>
#include <chrono>

namespace supple::cmd {
namespace {
std::string kindName(Kind kind) {
    switch (kind) {
        case Kind::Query:
            return "query";
        case Kind::Edit:
            return "edit";
        case Kind::Action:
            return "action";
    }
    return {};
}
std::string exposureName(Exposure exposure) {
    switch (exposure) {
        case Exposure::McpTool:
            return "mcp-tool";
        case Exposure::McpExec:
            return "mcp-exec";
        case Exposure::Internal:
            return "internal";
    }
    return {};
}
bool validName(std::string_view name) {
    if (name.empty()) return false;
    bool segmentStart = true;
    for (char ch : name) {
        if (ch == '.') {
            if (segmentStart) return false;
            segmentStart = true;
        } else if (segmentStart) {
            if (ch < 'a' || ch > 'z') return false;
            segmentStart = false;
        } else if (!((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-'))
            return false;
    }
    return !segmentStart;
}
json::Value exampleJson(const Example& example) {
    json::Value out = json::Value::object();
    out["title"] = example.title;
    out["args"] = example.args;
    out["result"] = example.result;
    out["fixture"] = example.fixture;
    return out;
}
json::Value commandJson(const CommandInfo& info) {
    json::Value out = json::Value::object();
    out["name"] = info.name;
    out["version"] = info.version;
    out["kind"] = kindName(info.kind);
    out["exposure"] = exposureName(info.exposure);
    out["summary"] = info.summary;
    out["description"] = info.description;
    out["tags"] = json::Value::array();
    for (const auto& tag : info.tags) out["tags"].push_back(tag);
    out["args"] = info.argsSchema;
    out["result"] = info.resultSchema;
    out["errors"] = json::Value::array();
    for (const auto& error : info.errors) {
        json::Value item = json::Value::object();
        item["code"] = error.code;
        item["when"] = error.when;
        item["hint"] = error.hint;
        out["errors"].push_back(std::move(item));
    }
    out["examples"] = json::Value::array();
    for (const auto& example : info.examples) out["examples"].push_back(exampleJson(example));
    return out;
}
json::Value objectSchema(std::initializer_list<std::pair<std::string, json::Value>> fields,
                         std::initializer_list<std::string> required = {}) {
    json::Value out = json::Value::object();
    out["type"] = "object";
    out["properties"] = json::Value::object();
    for (const auto& [name, schema] : fields) out["properties"][name] = schema;
    out["required"] = json::Value::array();
    for (const auto& name : required) out["required"].push_back(name);
    out["additionalProperties"] = false;
    return out;
}
json::Value typeSchema(std::string_view type) {
    json::Value out = json::Value::object();
    out["type"] = std::string(type);
    return out;
}
std::string stringField(const json::Value& value, std::string_view key) {
    const auto* member = value.find(key);
    return member && member->isString() ? member->asString() : std::string{};
}
Error requiredField(std::string_view field) {
    return Error("schema.missing", "missing required field", json::pointerJoin("", field),
                 "provide \"" + std::string(field) + "\"");
}
Status validateFields(const json::Value& args, std::initializer_list<std::string_view> required,
                      std::initializer_list<std::string_view> allowed) {
    if (!args.isObject()) return Error("schema.type", "expected object", "", "use an object");
    std::vector<Error> errors;
    for (auto key : required) {
        const auto* value = args.find(key);
        if (!value)
            errors.push_back(requiredField(key));
        else if (!value->isString())
            errors.emplace_back("schema.type", "expected string", json::pointerJoin("", key),
                                "use a string");
    }
    for (const auto& member : args.asObject()) {
        bool known = false;
        for (auto key : allowed)
            if (member.key == key) known = true;
        if (!known)
            errors.emplace_back("schema.unknown_member", "unknown member",
                                json::pointerJoin("", member.key), "remove this member");
    }
    if (errors.empty()) return {};
    if (errors.size() == 1) return errors.front();
    Error error("schema.invalid", "invalid arguments");
    error.details = std::move(errors);
    return error;
}
std::string pointerPrefix(std::string_view prefix, std::string_view path) {
    // Only JSON pointers nest; a text position ("1:43", notation) stays as it is.
    if (!path.empty() && path.front() != '/') return std::string(path);
    return std::string(prefix) + std::string(path);
}
void prefixError(Error& error, std::string_view prefix) {
    error.path = pointerPrefix(prefix, error.path);
    for (auto& detail : error.details) prefixError(detail, prefix);
}
Result<std::pair<std::string, int>> parseCommand(std::string_view command) {
    const auto at = command.find('@');
    if (at == std::string_view::npos) return std::pair{std::string(command), 0};
    std::string name(command.substr(0, at));
    auto suffix = command.substr(at + 1);
    if (suffix.empty() || suffix.front() == '0')
        return Error("command.version", "invalid command version", "/command",
                     "use @ followed by a positive integer");
    int version = 0;
    for (char ch : suffix) {
        if (ch < '0' || ch > '9' || version > 1000000)
            return Error("command.version", "invalid command version", "/command",
                         "use @ followed by a positive integer");
        version = version * 10 + (ch - '0');
    }
    return std::pair{std::move(name), version};
}
std::string availableVersions(const Registry& registry, std::string_view name) {
    std::string result;
    for (auto* info : registry.all())
        if (info->name == name) {
            if (!result.empty()) result += ", ";
            result += std::to_string(info->version);
        }
    return result;
}
Result<json::Value> resolveReference(const json::Value& value,
                                     const std::map<std::string, json::Value>& saved,
                                     const std::string& path) {
    if (value.isString()) {
        auto text = value.asString();
        if (text.size() >= 4 && text.substr(0, 2) == "${" && text.back() == '}') {
            auto expr = text.substr(2, text.size() - 3);
            auto dot = expr.find('.');
            std::string name = expr.substr(0, dot);
            auto it = saved.find(name);
            const json::Value* current = it == saved.end() ? nullptr : &it->second;
            std::size_t pos = dot;
            while (current && pos != std::string::npos) {
                std::size_t next = expr.find('.', pos + 1);
                auto key = expr.substr(pos + 1, next == std::string::npos ? next : next - pos - 1);
                if (current->isObject())
                    current = current->find(key);
                else if (current->isArray()) {
                    if (key.empty() || !std::all_of(key.begin(), key.end(),
                                                    [](char ch) { return ch >= '0' && ch <= '9'; }))
                        current = nullptr;
                    else {
                        std::size_t index = 0;
                        auto [end, error] =
                            std::from_chars(key.data(), key.data() + key.size(), index);
                        current = error == std::errc{} && end == key.data() + key.size() &&
                                          index < current->size()
                                      ? &current->at(index)
                                      : nullptr;
                    }
                } else
                    current = nullptr;
                pos = next;
            }
            if (!current) {
                std::string names;
                for (const auto& [available, unused] : saved) {
                    (void)unused;
                    if (!names.empty()) names += ", ";
                    names += available;
                }
                return Error("batch.bad_reference", "unknown step reference \"" + expr + "\"", path,
                             "available names: " + (names.empty() ? "(none)" : names));
            }
            return *current;
        }
        return value;
    }
    if (value.isArray()) {
        json::Value out = json::Value::array();
        for (std::size_t i = 0; i < value.size(); ++i) {
            auto child =
                resolveReference(value.at(i), saved, json::pointerJoin(path, std::to_string(i)));
            if (!child) return child.error();
            out.push_back(std::move(*child));
        }
        return out;
    }
    if (value.isObject()) {
        json::Value out = json::Value::object();
        for (const auto& member : value.asObject()) {
            auto child = resolveReference(member.value, saved, json::pointerJoin(path, member.key));
            if (!child) return child.error();
            out[member.key] = std::move(*child);
        }
        return out;
    }
    return value;
}
bool matches(const json::Value& expected, const json::Value& actual) {
    if (expected.isString() && expected.asString() == "<any>") return true;
    if (expected.isArray() && actual.isArray() && expected.size() == actual.size()) {
        for (std::size_t i = 0; i < expected.size(); ++i)
            if (!matches(expected.at(i), actual.at(i))) return false;
        return true;
    }
    if (expected.isObject() && actual.isObject() && expected.size() == actual.size()) {
        for (const auto& member : expected.asObject()) {
            const auto* value = actual.find(member.key);
            if (!value || !matches(member.value, *value)) return false;
        }
        return true;
    }
    return expected == actual;
}
}  // namespace

Registry::Registry() {
    auto addMeta = [&](std::string name, Kind kind, Exposure exposure, std::string summary,
                       json::Value argsSchema, json::Value resultSchema,
                       std::function<Status(const json::Value&)> validate,
                       std::function<Result<json::Value>(const json::Value&, Context&)> invoke,
                       Example example) {
        CommandInfo info;
        info.name = std::move(name);
        info.kind = kind;
        info.exposure = exposure;
        info.summary = std::move(summary);
        info.description = info.summary;
        info.argsSchema = std::move(argsSchema);
        info.resultSchema = std::move(resultSchema);
        info.validate = std::move(validate);
        info.validateResult = [](const json::Value&) -> Status { return {}; };
        info.invoke = std::move(invoke);
        info.examples.push_back(std::move(example));
        addInfo(std::move(info));
    };
    auto string = typeSchema("string");
    auto number = typeSchema("number");
    auto anyObject = typeSchema("object");
    addMeta(
        "registry.list", Kind::Query, Exposure::McpExec, "List available commands.",
        objectSchema({{"tag", string}, {"kind", string}, {"text", string}}), anyObject,
        [](const json::Value& args) -> Status {
            SUPPLE_TRY(validateFields(args, {}, {"tag", "kind", "text"}));
            for (auto key : {"tag", "kind", "text"})
                if (auto* value = args.find(key); value && !value->isString())
                    return Error("schema.type", "expected string", json::pointerJoin("", key));
            return {};
        },
        [](const json::Value& args, Context& ctx) -> Result<json::Value> {
            json::Value result = json::Value::object();
            result["commands"] = json::Value::array();
            auto tag = stringField(args, "tag"), kind = stringField(args, "kind");
            auto text = stringField(args, "text");
            for (auto* info : ctx.service<const Registry>().all()) {
                if (!tag.empty() &&
                    std::find(info->tags.begin(), info->tags.end(), tag) == info->tags.end())
                    continue;
                if (!kind.empty() && kindName(info->kind) != kind) continue;
                if (!text.empty() && info->name.find(text) == std::string::npos &&
                    info->summary.find(text) == std::string::npos)
                    continue;
                json::Value item = json::Value::object();
                item["name"] = info->name;
                item["version"] = info->version;
                item["kind"] = kindName(info->kind);
                item["summary"] = info->summary;
                result["commands"].push_back(std::move(item));
            }
            return result;
        },
        {"List commands", json::Value::object(), json::Value::object(), "empty"});
    addMeta(
        "registry.describe", Kind::Query, Exposure::McpExec, "Describe a command.",
        objectSchema({{"command", string}}, {"command"}), anyObject,
        [](const json::Value& args) { return validateFields(args, {"command"}, {"command"}); },
        [](const json::Value& args, Context& ctx) -> Result<json::Value> {
            auto parsed = parseCommand(stringField(args, "command"));
            if (!parsed) return parsed.error();
            auto* info = ctx.service<const Registry>().find(parsed->first, parsed->second);
            if (!info) {
                auto versions = availableVersions(ctx.service<const Registry>(), parsed->first);
                if (!versions.empty())
                    return Error("command.version", "unknown command version", "/command",
                                 "available versions: " + versions);
                return ctx.service<const Registry>().unknown(parsed->first);
            }
            return commandJson(*info);
        },
        {"Describe registry.list", json::Value::object(), json::Value::object(), "empty"});
    auto stepSchema =
        objectSchema({{"command", string}, {"args", anyObject}, {"as", string}}, {"command"});
    auto stepsSchema = typeSchema("array");
    stepsSchema["items"] = std::move(stepSchema);
    stepsSchema["maxItems"] = 256;
    addMeta(
        "batch", Kind::Edit, Exposure::McpTool, "Run commands atomically.",
        objectSchema({{"label", string}, {"steps", stepsSchema}}, {"steps"}), anyObject,
        [](const json::Value& args) -> Status {
            if (!args.isObject())
                return Error("schema.type", "expected object", "", "use an object");
            auto* steps = args.find("steps");
            if (!steps) return requiredField("steps");
            if (!steps->isArray())
                return Error("schema.type", "expected array", "/steps", "use an array of steps");
            if (auto* label = args.find("label"); label && !label->isString())
                return Error("schema.type", "expected string", "/label");
            return {};
        },
        [](const json::Value&, Context&) -> Result<json::Value> {
            return Error("command.internal", "batch must run through Dispatcher");
        },
        {"Empty batch", json::Value::object(), json::Value::object(), "empty"});
    addMeta(
        "job.status", Kind::Query, Exposure::McpExec, "Get a job's status.",
        objectSchema({{"job", string}}, {"job"}), anyObject,
        [](const json::Value& args) { return validateFields(args, {"job"}, {"job"}); },
        [](const json::Value& args, Context& ctx) {
            return ctx.service<JobManager>().status(stringField(args, "job"));
        },
        {"Get job", json::Value::object(), json::Value::object(), "job_running"});
    addMeta(
        "job.wait", Kind::Query, Exposure::McpExec, "Wait for a job.",
        objectSchema({{"job", string}, {"timeout_s", number}}, {"job"}), anyObject,
        [](const json::Value& args) -> Status {
            SUPPLE_TRY(validateFields(args, {"job"}, {"job", "timeout_s"}));
            if (auto* timeout = args.find("timeout_s");
                timeout &&
                (!timeout->isNumber() || timeout->asDouble() < 0 || timeout->asDouble() > 30))
                return Error("schema.out_of_range", "timeout_s must be between 0 and 30",
                             "/timeout_s");
            return {};
        },
        [](const json::Value& args, Context& ctx) {
            auto* timeout = args.find("timeout_s");
            return ctx.service<JobManager>().wait(stringField(args, "job"),
                                                  timeout ? timeout->asDouble() : 30.0);
        },
        {"Wait for job", json::Value::object(), json::Value::object(), "job_running"});
    addMeta(
        "job.cancel", Kind::Action, Exposure::McpExec, "Cancel a job.",
        objectSchema({{"job", string}}, {"job"}), anyObject,
        [](const json::Value& args) { return validateFields(args, {"job"}, {"job"}); },
        [](const json::Value& args, Context& ctx) -> Result<json::Value> {
            auto& jobs = ctx.service<JobManager>();
            SUPPLE_TRY(jobs.cancel(stringField(args, "job")));
            return jobs.status(stringField(args, "job"));
        },
        {"Cancel job", json::Value::object(), json::Value::object(), "job_running"});
    addMeta(
        "job.list", Kind::Query, Exposure::McpExec, "List session jobs.", objectSchema({}),
        anyObject, [](const json::Value& args) { return validateFields(args, {}, {}); },
        [](const json::Value&, Context& ctx) -> Result<json::Value> {
            return ctx.service<JobManager>().list();
        },
        {"List jobs", json::Value::object(), json::Value::object(), "empty"});
    auto& listExample = commands_.at({"registry.list", 1}).examples.front();
    listExample.args["text"] = "registry.list";
    listExample.result["commands"] = json::Value::array();
    json::Value listed = json::Value::object();
    const auto& self = commands_.at({"registry.list", 1});
    listed["name"] = self.name;
    listed["version"] = self.version;
    listed["kind"] = kindName(self.kind);
    listed["summary"] = self.summary;
    listExample.result["commands"].push_back(std::move(listed));
    auto& describe = commands_.at({"registry.describe", 1}).examples.front();
    describe.args["command"] = "registry.list";
    describe.result = commandJson(commands_.at({"registry.list", 1}));
    auto& batchExample = commands_.at({"batch", 1}).examples.front();
    batchExample.args["steps"] = json::Value::array();
    batchExample.result["label"] = "";
    batchExample.result["results"] = json::Value::array();
    for (auto name : {"job.status", "job.wait", "job.cancel"}) {
        auto& ex = commands_.at({name, 1}).examples.front();
        ex.args["job"] = "jb_1";
        ex.result["job"] = "jb_1";
        ex.result["status"] = name == std::string_view("job.cancel") ? "cancelled" : "running";
        ex.result["progress"] = json::Value::object();
        ex.result["progress"]["fraction"] = 0.0;
        ex.result["progress"]["message"] = "";
        if (name == std::string_view("job.wait")) ex.args["timeout_s"] = 0;
    }
    commands_.at({"job.list", 1}).examples.front().result["jobs"] = json::Value::array();
}
void Registry::addInfo(CommandInfo info) {
    assert(validName(info.name) && info.version > 0 && info.summary.size() <= 80);
    assert(info.exposure == Exposure::Internal || !info.examples.empty());
    auto key = std::pair{info.name, info.version};
    assert(!commands_.contains(key));
    commands_.emplace(std::move(key), std::move(info));
}
const CommandInfo* Registry::find(std::string_view name, int version) const {
    if (version) {
        auto it = commands_.find({std::string(name), version});
        return it == commands_.end() ? nullptr : &it->second;
    }
    const CommandInfo* found = nullptr;
    auto it = commands_.lower_bound({std::string(name), 0});
    while (it != commands_.end() && it->first.first == name) {
        found = &it->second;
        ++it;
    }
    return found;
}
std::vector<const CommandInfo*> Registry::all() const {
    std::vector<const CommandInfo*> out;
    for (const auto& [key, value] : commands_) {
        (void)key;
        out.push_back(&value);
    }
    return out;
}
Error Registry::unknown(std::string_view name) const {
    std::vector<std::string_view> names;
    std::string last;
    for (auto* info : all())
        if (info->name != last) {
            names.push_back(info->name);
            last = info->name;
        }
    std::string hint;
    for (int i = 0; i < 3 && !names.empty(); ++i) {
        auto choice = closestMatch(name, names);
        if (choice.empty()) break;
        if (!hint.empty()) hint += ", ";
        hint += choice;
        names.erase(std::remove(names.begin(), names.end(), choice), names.end());
    }
    return Error("command.unknown", "unknown command \"" + std::string(name) + "\"", "/command",
                 hint.empty() ? "use registry.list" : "did you mean " + hint + "?");
}
json::Value Registry::exportJson() const {
    json::Value out = json::Value::object();
    out["schema"] = "supple.command-registry/1.0";
    out["product"] = std::string(product::kName);
    out["version"] = std::string(product::kVersion);
    out["commands"] = json::Value::object();
    for (auto* info : all())
        out["commands"][info->name + "@" + std::to_string(info->version)] = commandJson(*info);
    return out;
}
std::uint64_t Host::clockMs() const {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

Dispatcher::~Dispatcher() { host_.stopJobs(); }

Result<json::Value> Dispatcher::execute(const Invocation& invocation) {
    auto result = dispatch(invocation, false);
    flushJobEvents();
    return result;
}
void Dispatcher::flushJobEvents() {
    for (auto& [topic, payload] : jobs_.drainEvents()) host_.emit(topic, std::move(payload));
}
Result<json::Value> Dispatcher::dispatch(const Invocation& invocation, bool inBatch, bool) {
    const auto start = std::chrono::steady_clock::now();
    auto parsed = parseCommand(invocation.command);
    auto name = parsed ? parsed->first : invocation.command;
    auto* info = parsed ? registry_.find(name, parsed->second) : nullptr;
    std::optional<json::Value> resolvedArgs;
    auto record = [&](const Result<json::Value>& result) {
        LogRecord log;
        log.seq = ++sequence_;
        log.time_ms = host_.clockMs();
        log.origin = invocation.origin;
        log.command = name;
        log.version = info ? info->version : parsed ? parsed->second : 0;
        log.args = resolvedArgs.value_or(invocation.args);
        log.ok = result.ok();
        if (result) {
            log.resultDigest = json::writeCompact(*result).substr(0, 200);
        } else {
            log.errorCode = result.error().code;
        }
        log.duration_us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                           std::chrono::steady_clock::now() - start)
                                           .count());
        host_.log(log);
    };
    auto finish = [&](Result<json::Value> result) {
        record(result);
        return result;
    };
    if (!parsed) return finish(parsed.error());
    if (!info) {
        auto versions = availableVersions(registry_, name);
        if (!versions.empty())
            return finish(Error("command.version", "unknown version of \"" + name + "\"",
                                "/command", "available versions: " + versions));
        return finish(registry_.unknown(name));
    }
    auto validation = info->validate(invocation.args);
    if (!validation) {
        Error error("command.invalid_args", "invalid arguments for " + name, "/args",
                    "fix the listed argument errors");
        auto inner = validation.error();
        if (inner.details.empty()) {
            prefixError(inner, "/args");
            error.details.push_back(std::move(inner));
        } else {
            for (auto& detail : inner.details) {
                prefixError(detail, "/args");
                error.details.push_back(std::move(detail));
            }
        }
        return finish(std::move(error));
    }
    if (inBatch && info->kind == Kind::Action && !host_.actionTransactional(name))
        return finish(Error("batch.not_transactional", "action cannot run inside a transaction",
                            "/command", "run it outside batch or make the host transaction-safe"));
    if (name == "batch") {
        if (inBatch)
            return finish(
                Error("batch.not_transactional", "nested batch is unsupported", "/command"));
        return finish(batch(invocation.args, invocation.origin));
    }
    Context context = host_.makeContext(invocation.origin);
    context.provide<const Registry>(registry_);
    context.provide<JobManager>(jobs_);
    auto invoke = [&]() -> Result<json::Value> {
        auto result = info->invoke(invocation.args, context);
        resolvedArgs = context.resolvedArgs();
        if (!result) {
            auto error = result.error();
            if (!error.path.empty() && error.path.front() == '/' &&
                error.path.rfind("/args", 0) != 0)
                prefixError(error, "/args");
            return error;
        }
        auto checked = info->validateResult(*result);
        if (!checked) {
            Error error("command.bad_result", "handler returned a value outside its result schema",
                        "/result", "fix the command handler or result schema");
            error.details.push_back(checked.error());
            return error;
        }
        if (info->kind != Kind::Query)
            host_.invoked(name, info->version, resolvedArgs.value_or(invocation.args), info->kind);
        return result;
    };
    Result<json::Value> result = Error("command.internal", "unreachable");
    if (info->kind == Kind::Edit && !inBatch) {
        auto transaction = host_.beginTransaction(invocation.origin, name);
        if (!transaction) return finish(transaction.error());
        result = invoke();
        if (result) {
            auto committed = host_.commitTransaction(*transaction);
            if (!committed) {
                host_.rollbackTransaction(*transaction);
                result = committed.error();
            }
        } else
            host_.rollbackTransaction(*transaction);
    } else
        result = invoke();
    return finish(std::move(result));
}
Result<json::Value> Dispatcher::batch(const json::Value& args, const Origin& origin) {
    const auto& steps = *args.find("steps");
    if (steps.size() > 256)
        return Error("batch.too_many_steps", "batch has more than 256 steps", "/steps",
                     "split the batch into at most 256 steps");
    // Resolve all command names and transaction constraints before any handler runs.
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps.at(i);
        auto path = json::pointerJoin("/steps", std::to_string(i));
        if (!step.isObject() || !step.find("command") || !step.find("command")->isString())
            return Error("command.invalid_args", "step needs a command string", path + "/command",
                         "set command to a registered command name");
        auto parsed = parseCommand(step.find("command")->asString());
        if (!parsed) {
            auto error = parsed.error();
            prefixError(error, path);
            return error;
        }
        auto* info = registry_.find(parsed->first, parsed->second);
        if (!info) {
            auto versions = availableVersions(registry_, parsed->first);
            auto error = versions.empty() ? registry_.unknown(parsed->first)
                                          : Error("command.version", "unknown command version",
                                                  "/command", "available versions: " + versions);
            prefixError(error, path);
            return error;
        }
        if (info->name == "batch" ||
            (info->kind == Kind::Action && !host_.actionTransactional(info->name)))
            return Error("batch.not_transactional", "step cannot run inside a transaction", path,
                         "remove this action or make it transaction-safe in the host");
        if (auto* as = step.find("as"); as && !as->isString())
            return Error("command.invalid_args", "step alias must be a string", path + "/as");
        if (auto* stepArgs = step.find("args"); stepArgs && !stepArgs->isObject())
            return Error("command.invalid_args", "step args must be an object", path + "/args");
    }
    auto label = stringField(args, "label");
    auto transaction = host_.beginTransaction(origin, label);
    if (!transaction) return transaction.error();
    json::Value result = json::Value::object();
    result["label"] = label;
    result["results"] = json::Value::array();
    std::map<std::string, json::Value> saved;
    for (std::size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps.at(i);
        auto path = json::pointerJoin("/steps", std::to_string(i));
        const auto* rawArgs = step.find("args");
        auto resolved =
            resolveReference(rawArgs ? *rawArgs : json::Value::object(), saved, path + "/args");
        if (!resolved) {
            host_.rollbackTransaction(*transaction);
            return resolved.error();
        }
        Invocation invocation{step.find("command")->asString(), std::move(*resolved), origin};
        auto output = dispatch(invocation, true, true);
        if (!output) {
            host_.rollbackTransaction(*transaction);
            auto detail = output.error();
            prefixError(detail, path);
            Error error("batch.step_failed", "batch step " + std::to_string(i) + " failed", path,
                        "fix this step and retry the batch");
            error.details.push_back(std::move(detail));
            return error;
        }
        if (auto* alias = step.find("as"); alias) saved[alias->asString()] = *output;
        result["results"].push_back(std::move(*output));
    }
    auto committed = host_.commitTransaction(*transaction);
    if (!committed) {
        host_.rollbackTransaction(*transaction);
        return committed.error();
    }
    return result;
}

Status runExamples(const Registry& registry, const FixtureFactory& factory) {
    for (auto* info : registry.all()) {
        if (info->exposure == Exposure::Internal) continue;
        for (const auto& example : info->examples) {
            auto host = factory(example.fixture);
            if (!host)
                return Error("command.example", "unknown example fixture", "",
                             "provide fixture \"" + example.fixture + "\"");
            Dispatcher dispatcher(registry, *host);
            host->setupFixture(dispatcher, example.fixture);
            auto output = dispatcher.execute({info->name + "@" + std::to_string(info->version),
                                              example.args,
                                              {Origin::Type::Test, "examples", {}}});
            if (!output)
                return Error("command.example",
                             "example failed: " + info->name + ": " + output.error().toString(), "",
                             "update the example or handler");
            if (!matches(example.result, *output))
                return Error("command.example", "example result differs: " + info->name, "",
                             "update the expected result or handler");
        }
    }
    return {};
}
json::Value mcpToolList(const Registry& registry) {
    json::Value tools = json::Value::array();
    for (auto* info : registry.all()) {
        if (info->exposure != Exposure::McpTool || registry.find(info->name) != info) continue;
        json::Value tool = json::Value::object();
        std::string name = info->name;
        std::replace(name.begin(), name.end(), '.', '_');
        tool["name"] = name;
        tool["title"] = info->summary;
        tool["description"] =
            info->summary + " " + info->description + " See describe for errors and examples.";
        auto input = info->argsSchema;
        if (auto* properties = input.find("properties"); properties && properties->isObject())
            for (auto& field : input["properties"].asObject()) {
                auto* unit = field.value.find("x-unit");
                if (!unit || !unit->isString()) continue;
                auto* description = field.value.find("description");
                std::string updated = description && description->isString()
                                          ? description->asString() + " "
                                          : std::string{};
                updated += "(" + unit->asString() + "; accepts quantity strings)";
                field.value["description"] = std::move(updated);
            }
        tool["inputSchema"] = std::move(input);
        tool["outputSchema"] = info->resultSchema;
        tool["annotations"] = json::Value::object();
        tool["annotations"]["readOnlyHint"] = info->kind == Kind::Query;
        tool["annotations"]["destructiveHint"] = info->kind == Kind::Action;
        tool["annotations"]["idempotentHint"] =
            info->name.ends_with(".set") || info->name.ends_with(".merge");
        tools.push_back(std::move(tool));
    }
    return tools;
}
std::string markdownReference(const Registry& registry) {
    std::string text = "# Command reference\n\nGenerated from the command registry.\n";
    for (auto* info : registry.all()) {
        text += "\n## `" + info->name + "@" + std::to_string(info->version) + "`\n\n";
        text += info->summary + "\n\n" + info->description + "\n\n";
        text += "Kind: `" + kindName(info->kind) + "`. Exposure: `" + exposureName(info->exposure) +
                "`.\n\n";
        text += "### Arguments\n\n| Field | Type | Unit | Range | Default | Description |\n";
        text += "| --- | --- | --- | --- | --- | --- |\n";
        if (auto* properties = info->argsSchema.find("properties");
            properties && properties->isObject())
            for (const auto& field : properties->asObject()) {
                const auto& schema = field.value;
                auto* type = schema.find("type");
                auto* unit = schema.find("x-unit");
                auto* minimum = schema.find("minimum");
                auto* maximum = schema.find("maximum");
                if (auto* variants = schema.find("anyOf");
                    variants && variants->isArray() && variants->size()) {
                    minimum = variants->at(0).find("minimum");
                    maximum = variants->at(0).find("maximum");
                }
                auto* fallback = schema.find("default");
                auto* description = schema.find("description");
                std::string range;
                if (minimum) range += json::writeCompact(*minimum);
                if (minimum || maximum) range += "..";
                if (maximum) range += json::writeCompact(*maximum);
                text += "| `" + field.key + "` | " +
                        (type && type->isString() ? type->asString() : "value") + " | " +
                        (unit && unit->isString() ? unit->asString() : "") + " | " + range + " | " +
                        (fallback ? json::writeCompact(*fallback) : "") + " | " +
                        (description && description->isString() ? description->asString() : "") +
                        " |\n";
            }
        text += "\n### Result\n\n```json\n" + json::writeCanonical(info->resultSchema) + "\n```\n";
        if (!info->errors.empty()) {
            text += "\n### Errors\n\n";
            for (const auto& error : info->errors)
                text += "- `" + error.code + "`: " + error.when + " Hint: " + error.hint + "\n";
        }
        text += "\n### Examples\n";
        for (const auto& example : info->examples)
            text += "\n**" + example.title + "** (`" + example.fixture + "`)\n\n```json\n" +
                    json::writeCanonical(example.args) + "\n```\n\nResult:\n\n```json\n" +
                    json::writeCanonical(example.result) + "\n```\n";
    }
    return text;
}
schema::Schema<Ref> refSchema() {
    json::Value exported = json::Value::object();
    exported["oneOf"] = json::Value::array();
    exported["oneOf"].push_back(typeSchema("string"));
    exported["oneOf"].push_back(typeSchema("object"));
    return schema::Schema<Ref>(
        "ref",
        [](const json::Value& value, Ref& out, schema::DecodeCtx& ctx, const std::string& path) {
            if (!value.isString() && !value.isObject()) {
                ctx.error("schema.type", "reference must be an ID or selector", path,
                          "use an ID string or selector object");
                return false;
            }
            out.value = value;
            return true;
        },
        [](const Ref& ref) { return ref.value; }, exported);
}
Result<std::string> resolveRef(Context& context, std::string_view kind, const Ref& ref) {
    if (ref.value.isString()) return ref.value.asString();
    return context.service<RefResolver>().resolve(kind, ref);
}
}  // namespace supple::cmd
