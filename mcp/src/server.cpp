#include <supple/mcp/server.h>
#include <supple/product.h>
#include <algorithm>
#include <string>

namespace supple::mcp {
namespace {
json::Value typeSchema(std::string_view type) {
    json::Value value = json::Value::object();
    value["type"] = std::string(type);
    return value;
}
json::Value objectSchema(std::initializer_list<std::pair<std::string, json::Value>> properties,
                         std::initializer_list<std::string> required = {}) {
    auto value = typeSchema("object");
    value["properties"] = json::Value::object();
    for (const auto& [name, schema] : properties) value["properties"][name] = schema;
    value["required"] = json::Value::array();
    for (const auto& name : required) value["required"].push_back(name);
    value["additionalProperties"] = false;
    return value;
}
json::Value response(const json::Value& id, json::Value value) {
    json::Value out = json::Value::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["result"] = std::move(value);
    return out;
}
json::Value error(const json::Value& id, int code, std::string message) {
    json::Value out = json::Value::object();
    out["jsonrpc"] = "2.0";
    out["id"] = id;
    out["error"] = json::Value::object();
    out["error"]["code"] = code;
    out["error"]["message"] = std::move(message);
    return out;
}
std::string stringField(const json::Value& value, std::string_view key) {
    auto* part = value.isObject() ? value.find(key) : nullptr;
    return part && part->isString() ? part->asString() : std::string{};
}
json::Value tool(std::string name, std::string title, std::string description, json::Value input,
                 json::Value output = typeSchema("object")) {
    json::Value value = json::Value::object();
    value["name"] = std::move(name);
    value["title"] = std::move(title);
    value["description"] = std::move(description);
    value["inputSchema"] = std::move(input);
    value["outputSchema"] = std::move(output);
    return value;
}
json::Value toolFromInfo(const json::Value& info) {
    auto name = stringField(info, "name");
    std::replace(name.begin(), name.end(), '.', '_');
    auto title = stringField(info, "summary");
    auto description =
        title + " " + stringField(info, "description") + " See describe for errors and examples.";
    auto* args = info.find("args");
    auto input = args ? *args : objectSchema({});
    if (auto* props = input.find("properties"); props && props->isObject())
        for (auto& field : input["properties"].asObject()) {
            auto* unit = field.value.find("x-unit");
            if (!unit || !unit->isString()) continue;
            auto prior = stringField(field.value, "description");
            field.value["description"] = prior + (prior.empty() ? "" : " ") + "(" +
                                         unit->asString() + "; accepts quantity strings)";
        }
    auto* result = info.find("result");
    auto value = tool(std::move(name), std::move(title), std::move(description), std::move(input),
                      result ? *result : typeSchema("object"));
    auto kind = stringField(info, "kind");
    value["annotations"] = json::Value::object();
    value["annotations"]["readOnlyHint"] = kind == "query";
    value["annotations"]["destructiveHint"] = kind == "action";
    auto command = stringField(info, "name");
    value["annotations"]["idempotentHint"] =
        command.ends_with(".set") || command.ends_with(".merge");
    return value;
}
}  // namespace

Server::Server(const cmd::Registry& registry, cmd::Dispatcher& dispatcher) {
    invoke_ = [&dispatcher](std::string_view name, const json::Value& args,
                            const cmd::Origin& origin) {
        return dispatcher.execute({std::string(name), args, origin});
    };
    dedicated_ = cmd::mcpToolList(registry);
    for (auto* info : registry.all()) {
        if (registry.find(info->name) != info) continue;
        kinds_[info->name] = info->kind;
        if (info->exposure == cmd::Exposure::McpTool) {
            std::string toolName = info->name;
            std::replace(toolName.begin(), toolName.end(), '.', '_');
            commands_[toolName] = info->name;
        }
    }
    queryTool_ = registry.find("session.describe") != nullptr;
}
Server::Server(rpc::Client& client) : proxyClient_(&client) {
    invoke_ = [&client](std::string_view name, const json::Value& args, const cmd::Origin&) {
        return client.call(name, args);
    };
}
Result<std::unique_ptr<Server>> Server::proxy(rpc::Client& client) {
    auto server = std::unique_ptr<Server>(new Server(client));
    auto listed = client.call("registry.list");
    if (!listed) return listed.error();
    auto* commands = listed->find("commands");
    if (!commands || !commands->isArray())
        return Error("mcp.registry", "remote registry.list returned no commands");
    std::map<std::string, int> latest;
    for (const auto& item : commands->asArray()) {
        auto name = stringField(item, "name");
        auto* version = item.find("version");
        if (version && version->isInt())
            latest[name] = std::max(latest[name], static_cast<int>(version->asInt()));
    }
    for (const auto& [name, version] : latest) {
        json::Value args = json::Value::object();
        args["command"] = name + "@" + std::to_string(version);
        auto described = client.call("registry.describe", args);
        if (!described) return described.error();
        auto kind = stringField(*described, "kind");
        server->kinds_[name] = kind == "query"  ? cmd::Kind::Query
                               : kind == "edit" ? cmd::Kind::Edit
                                                : cmd::Kind::Action;
        if (stringField(*described, "exposure") == "mcp-tool") {
            auto definition = toolFromInfo(*described);
            auto toolName = stringField(definition, "name");
            server->commands_[toolName] = name;
            server->dedicated_.push_back(std::move(definition));
        }
    }
    server->queryTool_ = latest.contains("session.describe");
    return server;
}
json::Value Server::tools() const {
    auto out = dedicated_;
    auto string = typeSchema("string"), object = typeSchema("object");
    out.push_back(tool(
        "describe", "Describe commands", "Search or describe registered commands.",
        objectSchema({{"command", string}, {"tag", string}, {"kind", string}, {"text", string}})));
    auto execInput = objectSchema({{"command", string}, {"args", object}, {"batch", object}});
    out.push_back(tool("exec", "Execute a command", "Run one command or an atomic batch.",
                       std::move(execInput)));
    if (queryTool_)
        out.push_back(tool("query", "Query the session", "Run a read-only command.",
                           objectSchema({{"command", string}, {"args", object}}, {"command"})));
    return out;
}
Result<json::Value> Server::call(std::string_view name, const json::Value& args) {
    if (name == "describe") {
        auto command = stringField(args, "command");
        if (!command.empty()) {
            json::Value params = json::Value::object();
            params["command"] = command;
            return invoke_("registry.describe", params, origin_);
        }
        return invoke_("registry.list", args, origin_);
    }
    if (name == "exec") {
        if (auto* batch = args.find("batch")) return invoke_("batch", *batch, origin_);
        auto command = stringField(args, "command");
        if (command.empty())
            return Error("mcp.arguments", "exec needs command or batch", "/arguments",
                         "provide command and args, or batch");
        auto* params = args.find("args");
        return invoke_(command, params ? *params : json::Value::object(), origin_);
    }
    if (name == "query") {
        auto command = stringField(args, "command");
        auto it = kinds_.find(command);
        if (it == kinds_.end() || it->second != cmd::Kind::Query)
            return Error("mcp.query", "query needs a registered read-only command",
                         "/arguments/command", "use describe to find a query command");
        auto* params = args.find("args");
        return invoke_(command, params ? *params : json::Value::object(), origin_);
    }
    auto it = commands_.find(std::string(name));
    if (it == commands_.end()) return Error("mcp.tool", "unknown tool", "/name", "use tools/list");
    return invoke_(it->second, args, origin_);
}
std::optional<json::Value> Server::handle(const json::Value& request) {
    if (!request.isObject()) return error(nullptr, -32600, "Invalid Request");
    auto* id = request.find("id");
    auto* version = request.find("jsonrpc");
    auto* method = request.find("method");
    if (!id) return std::nullopt;
    if (!version || !version->isString() || version->asString() != "2.0" || !method ||
        !method->isString() || !(id->isString() || id->isInt() || id->isNull()))
        return error(*id, -32600, "Invalid Request");
    auto* params = request.find("params");
    if (params && !params->isObject()) return error(*id, -32602, "Invalid params");
    auto args = params ? *params : json::Value::object();
    auto name = method->asString();
    if (name == "initialize") {
        auto* requested = args.find("protocolVersion");
        if (!requested || !requested->isString())
            return error(*id, -32602, "Missing protocolVersion");
        auto* clientInfo = args.find("clientInfo");
        origin_.client = clientInfo ? stringField(*clientInfo, "name") : std::string{};
        if (auto* meta = args.find("_meta")) origin_.agent = stringField(*meta, "agent");
        auto versionName = requested->asString();
        if (versionName != "2025-06-18" && versionName != "2025-11-25") versionName = "2025-11-25";
        if (proxyClient_) {
            json::Value hello = json::Value::object();
            hello["client"] = origin_.client.empty() ? "mcp-client" : origin_.client;
            hello["agent"] = origin_.agent;
            hello["protocol"] = 1;
            hello["mcp"] = true;
            auto greeted = proxyClient_->call("rpc.hello", hello);
            if (!greeted) return error(*id, -32000, greeted.error().toString());
        }
        initialized_ = true;
        json::Value result = json::Value::object();
        result["protocolVersion"] = versionName;
        result["capabilities"] = json::Value::object();
        result["capabilities"]["tools"] = json::Value::object();
        result["serverInfo"] = json::Value::object();
        result["serverInfo"]["name"] = std::string(product::kSlug);
        result["serverInfo"]["version"] = std::string(product::kVersion);
        return response(*id, std::move(result));
    }
    if (!initialized_) return error(*id, -32000, "Initialize first");
    if (name == "ping") return response(*id, json::Value::object());
    if (name == "tools/list") {
        json::Value result = json::Value::object();
        result["tools"] = tools();
        return response(*id, std::move(result));
    }
    if (name == "tools/call") {
        auto toolName = stringField(args, "name");
        auto* arguments = args.find("arguments");
        if (toolName.empty() || (arguments && !arguments->isObject()))
            return error(*id, -32602, "tools/call needs name and object arguments");
        if (toolName != "describe" && toolName != "exec" && !(toolName == "query" && queryTool_) &&
            !commands_.contains(toolName))
            return error(*id, -32601, "Unknown tool");
        auto result = call(toolName, arguments ? *arguments : json::Value::object());
        json::Value output = json::Value::object();
        output["content"] = json::Value::array();
        json::Value text = json::Value::object();
        text["type"] = "text";
        if (result) {
            output["structuredContent"] = *result;
            text["text"] = json::writeCompact(*result);
        } else {
            output["isError"] = true;
            output["structuredContent"] = rpc::errorJson(result.error());
            text["text"] = result.error().toString();
        }
        output["content"].push_back(std::move(text));
        return response(*id, std::move(output));
    }
    return error(*id, -32601, "Method not found");
}
Status Server::serve(std::istream& input, std::ostream& output) {
    std::string frame;
    while (std::getline(input, frame)) {
        if (frame.size() > rpc::kMaxMessageBytes) {
            output << json::writeCompact(error(nullptr, -32600, "Message too large")) << '\n';
            output.flush();
            continue;
        }
        auto parsed = json::parse(frame);
        auto reply = parsed ? handle(*parsed)
                            : std::optional<json::Value>(error(nullptr, -32700, "Parse error"));
        if (reply) {
            output << json::writeCompact(*reply) << '\n';
            output.flush();
            if (!output) return Error("mcp.write", "failed to write stdio response");
        }
    }
    return {};
}
}  // namespace supple::mcp
