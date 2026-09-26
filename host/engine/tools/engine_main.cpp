// <slug>-engine: the headless DAW (spec: docs/specs/engine.md §6).
//
//   <slug>-engine exec '<json>' [--project FILE] [--save] [--workdir DIR] [--plugin-path P]...
//       One invocation {"command": ..., "args": {...}} or {"batch": {...}}
//       against the project (a new one when --project is absent or missing);
//       --save writes the project back afterwards. Prints the result JSON.
//   <slug>-engine registry [--markdown | --mcp-tools]
//       The engine's command registry, Markdown reference or MCP tool list.
//   (--mcp and the control socket arrive with the MCP adapter, P2.6.)
#include <cstdio>
#include <cstdlib>
#include <string>
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
                 "       %s-engine registry [--markdown | --mcp-tools]\n",
                 slug.c_str(), slug.c_str());
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

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) return usage();
    cmd::Registry registry;
    engine::registerEngineCommands(registry);
    if (args[0] == "registry") {
        if (args.size() == 1)
            print(registry.exportJson());
        else if (args[1] == "--markdown") {
            const std::string md = cmd::markdownReference(registry);
            std::fwrite(md.data(), 1, md.size(), stdout);
        } else if (args[1] == "--mcp-tools")
            print(cmd::mcpToolList(registry));
        else
            return usage();
        return 0;
    }
    if (args[0] != "exec" || args.size() < 2) return usage();

    engine::AppOptions options;
    options.identity.name = std::string(product::kName);
    options.identity.vendor = std::string(product::kVendor);
    options.identity.version = std::string(product::kVersion);
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
