// <slug>-sandbox: the headless plugin sandbox (spec: docs/specs/sandbox.md §5).
//
//   <slug>-sandbox exec '<json>' [--workdir DIR] [--plugin-path P]...
//       One invocation {"command": "...", "args": {...}} or a batch
//       {"batch": {"label": ..., "steps": [...]}} against a fresh session;
//       prints the result (or the error) as JSON. Exit 0 = ok, 1 = error.
//   <slug>-sandbox registry [--markdown | --mcp-tools]
//       The command registry document, the Markdown reference, or the MCP
//       tool list (generator output; drift-tested against skill/generated/).
//   <slug>-sandbox --mcp [--workdir DIR] [--plugin-path P]...
//       Serve MCP over stdio against a live in-process sandbox session.
//   <slug>-sandbox serve [--workdir DIR] [--plugin-path P]... [--instance NAME] [--socket PATH]
//       Serve the command bus on a Unix socket from the model thread.
#include <unistd.h>

#include <supple/mcp/server.h>
#include <supple/rpc/model_loop.h>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <supple/product.h>

#include "supple/sandbox/commands.h"

using namespace supple;
namespace fs = std::filesystem;

namespace {

int usage() {
    std::fprintf(stderr,
                 "usage: %.*s-sandbox exec '<json>' [--workdir DIR] [--plugin-path P]...\n"
                 "       %.*s-sandbox registry [--markdown | --mcp-tools]\n"
                 "       %.*s-sandbox --mcp [--workdir DIR] [--plugin-path P]...\n"
                 "       %.*s-sandbox serve [--workdir DIR] [--plugin-path P]... "
                 "[--instance NAME] [--socket PATH]\n",
                 static_cast<int>(product::kSlug.size()), product::kSlug.data(),
                 static_cast<int>(product::kSlug.size()), product::kSlug.data(),
                 static_cast<int>(product::kSlug.size()), product::kSlug.data(),
                 static_cast<int>(product::kSlug.size()), product::kSlug.data());
    return 2;
}

fs::path defaultWorkdir() {
    const char* cache = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    fs::path base = cache && *cache ? fs::path(cache) : fs::path(home ? home : "/tmp") / ".cache";
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    return base / std::string(product::kSlug) / "sandbox" /
           (std::to_string(now) + "-" + std::to_string(::getpid()));
}

json::Value errorJson(const Error& e) {
    json::Value v = json::Value::object();
    v["code"] = e.code;
    v["message"] = e.message;
    if (!e.path.empty()) v["path"] = e.path;
    if (!e.hint.empty()) v["hint"] = e.hint;
    if (e.loc) {
        v["line"] = e.loc->line;
        v["column"] = e.loc->column;
    }
    if (!e.details.empty()) {
        json::Value d = json::Value::array();
        for (const auto& x : e.details) d.push_back(errorJson(x));
        v["details"] = std::move(d);
    }
    return v;
}

void print(const json::Value& v) {
    std::string text = json::writeCanonical(v);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    cmd::Registry registry;
    sandbox::registerCommands(registry);

    if (args[0] == "--mcp" || args[0] == "serve") {
        const bool rpcMode = args[0] == "serve";
        sandbox::SessionOptions options;
        options.identity.name = std::string(product::kName) + " sandbox";
        options.identity.vendor = std::string(product::kVendor);
        options.identity.version = std::string(product::kVersion);
        std::string instance = "default";
        fs::path socket;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--workdir" && i + 1 < args.size())
                options.workdir = args[++i];
            else if (args[i] == "--plugin-path" && i + 1 < args.size())
                options.pluginPath.emplace_back(args[++i]);
            else if (rpcMode && args[i] == "--instance" && i + 1 < args.size())
                instance = args[++i];
            else if (rpcMode && args[i] == "--socket" && i + 1 < args.size())
                socket = args[++i];
            else
                return usage();
        }
        if (options.workdir.empty()) options.workdir = defaultWorkdir();
        if (!options.pluginPath.empty())
            for (auto& path : host::defaultSearchPath()) options.pluginPath.push_back(path);
        auto session = sandbox::Session::open(options);
        if (!session) {
            std::fprintf(stderr, "%s\n", session.error().toString().c_str());
            return 1;
        }
        sandbox::SandboxHost host(std::move(*session), options.workdir / "commands.ndjson");
        cmd::Dispatcher dispatcher(registry, host);
        if (rpcMode) {
            rpc::ModelLoop loop;
            rpc::Server* activeServer = nullptr;
            rpc::Server server(registry, dispatcher, [&](auto task) {
                loop.post([&, task = std::move(task)] {
                    task();
                    for (auto& [topic, payload] : host.takeEvents())
                        activeServer->publish(topic, payload);
                });
            });
            activeServer = &server;
            auto started = server.start(socket.empty() ? rpc::socketPath(instance) : socket);
            if (!started) {
                std::fprintf(stderr, "%s\n", started.error().toString().c_str());
                return 1;
            }
            loop.run();
            server.stop();
            return 0;
        }
        mcp::Server server(registry, dispatcher);
        auto status = server.serve(std::cin, std::cout);
        if (!status) {
            std::fprintf(stderr, "%s\n", status.error().toString().c_str());
            return 1;
        }
        return 0;
    }

    if (args[0] == "registry") {
        if (args.size() == 1) {
            print(registry.exportJson());
        } else if (args[1] == "--markdown") {
            const std::string md = cmd::markdownReference(registry);
            std::fwrite(md.data(), 1, md.size(), stdout);
        } else if (args[1] == "--mcp-tools") {
            print(cmd::mcpToolList(registry));
        } else {
            return usage();
        }
        return 0;
    }

    if (args[0] == "exec") {
        if (args.size() < 2) return usage();
        sandbox::SessionOptions options;
        options.identity.name = std::string(product::kName) + " sandbox";
        options.identity.vendor = std::string(product::kVendor);
        options.identity.version = std::string(product::kVersion);
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--workdir" && i + 1 < args.size())
                options.workdir = args[++i];
            else if (args[i] == "--plugin-path" && i + 1 < args.size())
                options.pluginPath.emplace_back(args[++i]);
            else
                return usage();
        }
        if (options.workdir.empty()) options.workdir = defaultWorkdir();
        if (!options.pluginPath.empty()) {
            // Explicit paths come first; the standard locations still apply.
            for (auto& p : host::defaultSearchPath()) options.pluginPath.push_back(p);
        }
        auto parsed = json::parse(args[1]);
        if (!parsed || !parsed->isObject()) {
            print(errorJson(parsed ? Error("cli.invocation", "the invocation must be an object", {},
                                           R"({"command": "plugin.list", "args": {}})")
                                   : parsed.error()));
            return 1;
        }
        auto session = sandbox::Session::open(options);
        if (!session) {
            print(errorJson(session.error()));
            return 1;
        }
        const fs::path log = options.workdir / "commands.ndjson";
        sandbox::SandboxHost host(std::move(*session), log);
        cmd::Dispatcher dispatcher(registry, host);
        cmd::Invocation inv;
        inv.origin = {cmd::Origin::Type::Rpc, "cli", ""};
        if (const json::Value* batch = parsed->find("batch")) {
            inv.command = "batch";
            inv.args = *batch;
        } else if (const json::Value* c = parsed->find("command"); c && c->isString()) {
            inv.command = c->asString();
            if (const json::Value* a = parsed->find("args")) inv.args = *a;
        } else {
            print(errorJson(Error("cli.invocation", "missing \"command\" or \"batch\"", {},
                                  R"({"command": "plugin.list", "args": {}})")));
            return 1;
        }
        auto result = dispatcher.execute(inv);
        if (!result) {
            print(errorJson(result.error()));
            return 1;
        }
        print(*result);
        return 0;
    }
    return usage();
}
