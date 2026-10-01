// <slug>-engine: the headless DAW (spec: docs/specs/engine.md §6).
//
//   <slug>-engine exec '<json>' [--project FILE] [--save] [--workdir DIR] [--plugin-path P]...
//       One invocation {"command": ..., "args": {...}} or {"batch": {...}}
//       against the project (a new one when --project is absent or missing);
//       --save writes the project back afterwards. Prints the result JSON.
//   <slug>-engine registry [--markdown | --mcp-tools | --lua]
//       The engine's command registry, Markdown reference, MCP tool list or
//       the LuaLS annotations of the scripting bindings.
//   <slug>-engine --mcp [--project FILE] [--workdir DIR] [--plugin-path P]...
//       Serve MCP over stdio against the in-process engine session.
//   <slug>-engine serve [--project FILE] [--instance NAME] [--socket PATH]
//       Serve the command bus on a Unix socket from the model thread.
//   <slug>-engine play FILE [--bar N] [--seconds S] [--block N] [--backend jack|dummy]
//       Play a project through the audio device (to its end, or for S
//       seconds; Ctrl-C stops), then print engine.status.
#include <supple/mcp/server.h>
#include <supple/rpc/model_loop.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include <supple/product.h>

#include "supple/engine/app.h"

using namespace supple;
namespace fs = std::filesystem;

namespace {

int usage() {
    const auto slug = std::string(product::kSlug);
    std::fprintf(stderr,
                 "usage: %s-engine exec '<json>' [--project FILE] [--save] [--workdir DIR] "
                 "[--plugin-path P]...\n"
                 "       %s-engine registry [--markdown | --mcp-tools | --lua]\n"
                 "       %s-engine --mcp [--project FILE] [--workdir DIR] [--plugin-path P]...\n"
                 "       %s-engine serve [--project FILE] [--instance NAME] [--socket PATH]\n"
                 "       %s-engine play FILE [--bar N] [--seconds S] [--block N] "
                 "[--backend jack|dummy] [--plugin-path P]...\n",
                 slug.c_str(), slug.c_str(), slug.c_str(), slug.c_str(), slug.c_str());
    return 2;
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
    const std::string text = json::writeCanonical(v);
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fputc('\n', stdout);
}

fs::path defaultWorkdir() {
    const char* cache = std::getenv("XDG_CACHE_HOME");
    const char* home = std::getenv("HOME");
    const fs::path base =
        cache && *cache ? fs::path(cache) : fs::path(home ? home : "/tmp") / ".cache";
    return base / std::string(product::kSlug) / "engine";
}

std::atomic<bool> g_interrupted{false};

// The plugin bridge (isolated fx): next to this executable when installed,
// in the clap-host build directory in a build tree.
void configureBridge() {
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return;
    const std::string name = std::string(product::kSlug) + "-bridge";
    for (const fs::path& candidate :
         {self.parent_path() / name, self.parent_path().parent_path() / "clap-host" / name})
        if (fs::exists(candidate)) {
            host::setBridgeExecutable(candidate);
            return;
        }
}

engine::AppOptions cliOptions() {
    engine::AppOptions options;
    options.identity.name = std::string(product::kName);
    options.identity.vendor = std::string(product::kVendor);
    options.identity.version = std::string(product::kVersion);
    return options;
}

int play(cmd::Registry& registry, const std::vector<std::string>& args) {
    if (args.size() < 2) return usage();
    const fs::path project = fs::absolute(args[1]);
    engine::AppOptions options = cliOptions();
    json::Value start = json::Value::object();
    std::optional<std::int64_t> bar;
    std::optional<double> seconds;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (i + 1 >= args.size()) return usage();
        if (args[i] == "--bar")
            bar = std::atoll(args[++i].c_str());
        else if (args[i] == "--seconds")
            seconds = std::atof(args[++i].c_str());
        else if (args[i] == "--block")
            start["block"] = std::int64_t{std::atoll(args[++i].c_str())};
        else if (args[i] == "--backend")
            start["backend"] = args[++i];
        else if (args[i] == "--plugin-path")
            options.pluginPath.emplace_back(args[++i]);
        else
            return usage();
    }
    options.workdir = project.parent_path();
    if (!options.pluginPath.empty())
        for (auto& p : host::defaultSearchPath()) options.pluginPath.push_back(p);
    auto app = engine::EngineApp::create(options);
    if (!app) {
        print(errorJson(app.error()));
        return 1;
    }
    if (auto st = (*app)->open(project); !st) {
        print(errorJson(st.error()));
        return 1;
    }
    cmd::Dispatcher dispatcher(registry, **app);
    (*app)->attachCommands(dispatcher);
    auto run = [&](const char* command, json::Value a) {
        return dispatcher.execute({command, std::move(a), {cmd::Origin::Type::Rpc, "cli", ""}});
    };
    json::Value seek = json::Value::object();
    seek["bar"] = bar.value_or(1);
    for (auto [command, a] : {std::pair{"engine.start", start}, std::pair{"transport.seek", seek},
                              std::pair{"transport.play", json::Value::object()}}) {
        if (auto r = run(command, a); !r) {
            print(errorJson(r.error()));
            return 1;
        }
    }
    // Default length: from the start position to the last item's end, plus a second.
    const model::Session s = (*app)->view();
    double endBeat = 0;
    for (const auto& e : s.items) endBeat = std::max(endBeat, e.value->position + e.value->length);
    const double from = (*app)->engine()->transport().seconds;
    const double length = seconds.value_or(s.timeMap->secondsAt(endBeat) - from + 1.0);
    std::signal(SIGINT, [](int) { g_interrupted = true; });
    std::fprintf(stderr, "playing %s for %.1f s (Ctrl-C stops)\n", project.filename().c_str(),
                 length);
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double>(length);
    while (!g_interrupted && std::chrono::steady_clock::now() < end) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        (*app)->engine()->service();
    }
    auto status = run("engine.status", json::Value::object());
    (void)run("engine.stop", json::Value::object());
    if (!status) {
        print(errorJson(status.error()));
        return 1;
    }
    print(*status);
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    configureBridge();
    cmd::Registry registry;
    engine::registerEngineCommands(registry);
    if (args[0] == "--mcp" || args[0] == "serve") {
        const bool rpcMode = args[0] == "serve";
        engine::AppOptions options;
        options.identity.name = std::string(product::kName);
        options.identity.vendor = std::string(product::kVendor);
        options.identity.version = std::string(product::kVersion);
        fs::path project;
        std::string instance = "default";
        fs::path socket;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--project" && i + 1 < args.size())
                project = fs::absolute(args[++i]);
            else if (args[i] == "--workdir" && i + 1 < args.size())
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
        if (options.workdir.empty())
            options.workdir = project.empty() ? defaultWorkdir() : project.parent_path();
        if (!options.pluginPath.empty())
            for (auto& path : host::defaultSearchPath()) options.pluginPath.push_back(path);
        auto app = engine::EngineApp::create(options);
        if (!app) {
            std::fprintf(stderr, "%s\n", app.error().toString().c_str());
            return 1;
        }
        if (!project.empty() && fs::exists(project)) {
            auto status = (*app)->open(project);
            if (!status) {
                std::fprintf(stderr, "%s\n", status.error().toString().c_str());
                return 1;
            }
        }
        cmd::Dispatcher dispatcher(registry, **app);
        // The service tick: engine housekeeping (plans, failures, device
        // restart, autosave) every 100 ms, also while no command arrives.
        const auto interval = std::chrono::milliseconds(100);
        rpc::Server* activeServer = nullptr;
        auto publishEvents = [&] {
            for (auto& [topic, payload] : (*app)->takeEvents())
                activeServer->publish(topic, payload);
        };
        (*app)->attachCommands(dispatcher);
        if (rpcMode) {
            rpc::ModelLoop loop;
            rpc::Server server(registry, dispatcher, [&](auto task) {
                loop.post([&, task = std::move(task)] {
                    task();
                    publishEvents();
                });
            });
            activeServer = &server;
            auto started = server.start(socket.empty() ? rpc::socketPath(instance) : socket);
            if (!started) {
                std::fprintf(stderr, "%s\n", started.error().toString().c_str());
                return 1;
            }
            auto tick = loop.every(interval, [&] {
                (*app)->tick();
                dispatcher.flushJobEvents();  // job events and follow-ups from workers
                publishEvents();
            });
            loop.run();
            tick.cancel();
            server.stop();
            return 0;
        }
        mcp::Server server(registry, dispatcher);
        // The tick drains the app's events: MCP has no event channel, so they
        // would only pile up.
        auto status = server.serve(::fileno(stdin), std::cout,
                                   mcp::Tick{[&] {
                                                 (*app)->tick();
                                                 dispatcher.flushJobEvents();
                                                 (*app)->takeEvents();
                                             },
                                             interval});
        if (!status) {
            std::fprintf(stderr, "%s\n", status.error().toString().c_str());
            return 1;
        }
        return 0;
    }
    if (args[0] == "registry") {
        if (args.size() == 1)
            print(registry.exportJson());
        else if (args[1] == "--markdown") {
            const std::string md = cmd::markdownReference(registry);
            std::fwrite(md.data(), 1, md.size(), stdout);
        } else if (args[1] == "--mcp-tools")
            print(cmd::mcpToolList(registry));
        else if (args[1] == "--lua") {
            const std::string stubs = cmd::luaStubs(registry);
            std::fwrite(stubs.data(), 1, stubs.size(), stdout);
        } else
            return usage();
        return 0;
    }
    if (args[0] == "play") return play(registry, args);
    if (args[0] != "exec" || args.size() < 2) return usage();

    engine::AppOptions options = cliOptions();
    fs::path project;
    bool save = false;
    for (std::size_t i = 2; i < args.size(); ++i) {
        if (args[i] == "--project" && i + 1 < args.size())
            project = fs::absolute(args[++i]);
        else if (args[i] == "--save")
            save = true;
        else if (args[i] == "--workdir" && i + 1 < args.size())
            options.workdir = args[++i];
        else if (args[i] == "--plugin-path" && i + 1 < args.size())
            options.pluginPath.emplace_back(args[++i]);
        else
            return usage();
    }
    if (options.workdir.empty())
        options.workdir = project.empty() ? defaultWorkdir() : project.parent_path();
    if (!options.pluginPath.empty())
        for (auto& p : host::defaultSearchPath()) options.pluginPath.push_back(p);
    auto app = engine::EngineApp::create(options);
    if (!app) {
        print(errorJson(app.error()));
        return 1;
    }
    if (!project.empty() && fs::exists(project)) {
        auto st = (*app)->open(project);
        if (!st) {
            print(errorJson(st.error()));
            return 1;
        }
    }
    auto parsed = json::parse(args[1]);
    if (!parsed || !parsed->isObject()) {
        print(errorJson(parsed ? Error("cli.invocation", "the invocation must be an object")
                               : parsed.error()));
        return 1;
    }
    cmd::Dispatcher dispatcher(registry, **app);
    (*app)->attachCommands(dispatcher);
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
                              R"({"command": "session.describe", "args": {}})")));
        return 1;
    }
    auto result = dispatcher.execute(inv);
    if (!result) {
        print(errorJson(result.error()));
        return 1;
    }
    if (save) {
        const fs::path target = project.empty() ? (*app)->projectPath() : project;
        if (target.empty()) {
            print(errorJson(Error("cli.save", "--save needs --project FILE")));
            return 1;
        }
        auto st = (*app)->save(target);
        if (!st) {
            print(errorJson(st.error()));
            return 1;
        }
    }
    print(*result);
    return 0;
}
