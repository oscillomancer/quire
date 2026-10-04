#include "scan.h"

#include "bridge_client.h"
#include "supple/base/files.h"
#include "supple/json/json.h"
#include "supple/product.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>

extern char** environ;

namespace supple::host {
namespace fs = std::filesystem;
namespace {
constexpr std::string_view kSchema = "supple.plugin-scan/1.0";
thread_local ScanReport gReport;

Error invalidOutput() {
    return Error("host.scan.invalid_output", "plugin scanner returned invalid output", "/path",
                 "update the bridge and plugin, then change/reinstall the module to retry");
}

json::Value pluginsJson(const std::vector<PluginInfo>& plugins) {
    auto list = json::Value::array();
    for (const auto& p : plugins) {
        auto item = json::Value::object();
        item["id"] = p.id;
        item["name"] = p.name;
        item["vendor"] = p.vendor;
        item["version"] = p.version;
        item["description"] = p.description;
        item["format"] = p.format;
        item["path"] = p.path.string();
        item["instrument"] = p.instrument;
        item["semantic"] = p.semantic;
        auto features = json::Value::array();
        for (const auto& f : p.features) features.push_back(f);
        item["features"] = std::move(features);
        list.push_back(std::move(item));
    }
    return list;
}

std::vector<fs::path> modules(std::span<const fs::path> roots) {
    std::vector<fs::path> result;
    std::set<fs::path> seen;
    for (const auto& root : roots) {
        std::error_code ec;
        std::vector<fs::path> found;
        if (fs::is_regular_file(root, ec) ||
            (root.extension() == ".vst3" && fs::is_directory(root, ec))) {
            found.push_back(root);
        } else if (fs::is_directory(root, ec)) {
            for (fs::recursive_directory_iterator
                     it(root, fs::directory_options::skip_permission_denied, ec),
                 end;
                 it != end; it.increment(ec)) {
                if (ec) break;
                if (it->path().extension() == ".vst3" && it->is_directory(ec)) {
                    found.push_back(it->path());
                    it.disable_recursion_pending();
                } else if (it->path().extension() == ".clap" && it->is_regular_file(ec)) {
                    found.push_back(it->path());
                }
            }
        }
        std::sort(found.begin(), found.end());
        for (const auto& path : found) {
            auto canonical = fs::canonical(path, ec);
            if (!ec && seen.insert(canonical).second) {
                // VST3's loader derives the binary name from the bundle name.
                // Preserve its discovered alias for loading, canonicalize only
                // its cache identity. CLAP binary aliases do not affect loading.
                auto loadPath = path.extension() == ".vst3"
                                    ? fs::absolute(path, ec).lexically_normal()
                                    : std::move(canonical);
                if (!ec) result.push_back(std::move(loadPath));
            }
        }
    }
    return result;
}

// A bundle's directory mtime alone misses replacement/touch of an existing
// binary or moduleinfo.json. Record every regular file's size and timestamp.
Result<json::Value> fingerprint(const fs::path& module) {
    std::error_code ec;
    const auto canonical = fs::canonical(module, ec);
    if (ec) return Error("host.scan.stat", "cannot resolve module " + module.string(), "/path");
    std::vector<fs::path> files;
    if (fs::is_regular_file(module, ec)) {
        files.push_back(module);
    } else {
        std::vector<fs::path> ancestors{canonical};
        for (fs::recursive_directory_iterator
                 it(module, fs::directory_options::follow_directory_symlink, ec),
             end;
             it != end; it.increment(ec)) {
            if (ec) break;
            ancestors.resize(static_cast<std::size_t>(it.depth()) + 1);
            if (it->is_directory(ec)) {
                const auto directory = fs::canonical(it->path(), ec);
                if (ec) break;
                if (std::find(ancestors.begin(), ancestors.end(), directory) != ancestors.end())
                    it.disable_recursion_pending();
                else
                    ancestors.push_back(directory);
            }
            if (it->is_regular_file(ec)) files.push_back(it->path());
            if (ec) break;
        }
    }
    auto key = json::Value::object();
    key["path"] = canonical.string();
    auto stats = json::Value::array();
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        if (ec) break;
        const auto size = fs::file_size(file, ec);
        if (ec) break;
        const auto time = fs::last_write_time(file, ec);
        if (ec) break;
        auto stat = json::Value::object();
        stat["path"] = file.lexically_relative(module).string();
        // Strings preserve the filesystem's complete integer range in JSON.
        stat["size"] = std::to_string(size);
        stat["mtime"] = std::to_string(time.time_since_epoch().count());
        stats.push_back(std::move(stat));
    }
    if (ec)
        return Error("host.scan.stat",
                     "cannot stat module " + module.string() + ": " + ec.message(), "/path",
                     "check file permissions");
    key["files"] = std::move(stats);
    return key;
}

fs::path cacheDirectory() {
    fs::path root;
    if (const char* env = std::getenv("XDG_CACHE_HOME"); env && fs::path(env).is_absolute())
        root = env;
    else if (const char* home = std::getenv("HOME"); home && fs::path(home).is_absolute())
        root = fs::path(home) / ".cache";
    if (root.empty()) return {};
    return root / product::kSlug / "plugin-scan";
}

fs::path cacheFile(const fs::path& directory, const fs::path& module) {
    // A filename hash only: the full canonical path is checked in the record,
    // so even a hash collision can only cause a cache miss.
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : module.string()) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return directory / (std::to_string(hash) + ".json");
}

struct Child {
    pid_t pid = 0;
    int fd = -1;
    ~Child() {
        if (fd >= 0) ::close(fd);
        if (pid > 0) {
            // The leader is not reaped until here: its PID cannot be reused.
            // Also close descendants holding stdout open after the leader exits.
            ::kill(-pid, SIGKILL);
            int status = 0;
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
            }
        }
    }
};

Result<std::vector<PluginInfo>> supervise(const fs::path& executable, const fs::path& module,
                                          const ScanOptions& options) {
    int pipeFds[2];
    if (::pipe2(pipeFds, O_CLOEXEC) != 0)
        return Error("host.scan.spawn",
                     "cannot create scanner pipe: " + std::string(std::strerror(errno)));
    Child child;
    child.fd = pipeFds[0];
    const int flags = ::fcntl(child.fd, F_GETFL);
    if (flags < 0 || ::fcntl(child.fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::close(pipeFds[1]);
        return Error("host.scan.spawn", "cannot make scanner pipe nonblocking");
    }
    posix_spawn_file_actions_t actions;
    int rc = ::posix_spawn_file_actions_init(&actions);
    if (rc != 0) {
        ::close(pipeFds[1]);
        return Error("host.scan.spawn", "cannot initialize scanner file actions");
    }
    posix_spawnattr_t attrs;
    rc = ::posix_spawnattr_init(&attrs);
    if (rc == 0) {
        rc = ::posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
        if (rc == 0) rc = ::posix_spawn_file_actions_adddup2(&actions, pipeFds[1], STDOUT_FILENO);
        if (rc == 0 && pipeFds[1] != STDOUT_FILENO)
            rc = ::posix_spawn_file_actions_addclose(&actions, pipeFds[1]);
        if (rc == 0)
            rc = ::posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY,
                                                    0);
        if (rc == 0)
            rc = ::posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY,
                                                    0);
        if (rc == 0) rc = ::posix_spawn_file_actions_addclosefrom_np(&actions, 3);
        if (rc == 0) rc = ::posix_spawnattr_setpgroup(&attrs, 0);
        if (rc == 0) rc = ::posix_spawnattr_setflags(&attrs, POSIX_SPAWN_SETPGROUP);
        const auto exeText = executable.string();
        const auto moduleText = module.string();
        char* args[] = {const_cast<char*>(exeText.c_str()), const_cast<char*>("--scan"),
                        const_cast<char*>(moduleText.c_str()), nullptr};
        if (rc == 0)
            rc = ::posix_spawn(&child.pid, exeText.c_str(), &actions, &attrs, args, environ);
        ::posix_spawnattr_destroy(&attrs);
    }
    ::posix_spawn_file_actions_destroy(&actions);
    ::close(pipeFds[1]);
    if (rc != 0) {
        child.pid = 0;
        return Error("host.scan.spawn", "cannot start scanner: " + std::string(std::strerror(rc)),
                     "/path", "check the bridge executable and its permissions");
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs);
    std::string output;
    std::array<char, 8192> bytes;
    bool eof = false;
    siginfo_t exit{};
    for (;;) {
        if (std::chrono::steady_clock::now() >= deadline)
            return Error(
                "host.scan.timeout",
                "plugin scanner timed out after " + std::to_string(options.timeoutMs) + " ms",
                "/path", "repair/reinstall the module to retry its scan");
        if (!eof) {
            const auto n = ::read(child.fd, bytes.data(), bytes.size());
            if (n > 0) {
                if (static_cast<std::size_t>(n) > options.maxOutputBytes - output.size())
                    return Error("host.scan.output_limit",
                                 "plugin scanner output exceeds " +
                                     std::to_string(options.maxOutputBytes) + " bytes",
                                 "/path", "update the plugin/bridge");
                output.append(bytes.data(), static_cast<std::size_t>(n));
                continue;
            }
            if (n == 0)
                eof = true;
            else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK)
                return Error("host.scan.io", "cannot read scanner output");
        }
        if (::waitid(P_PID, static_cast<id_t>(child.pid), &exit, WEXITED | WNOHANG | WNOWAIT) < 0 &&
            errno != EINTR)
            return Error("host.scan.wait", "cannot observe scanner exit");
        if (exit.si_pid != 0) {
            if (exit.si_code == CLD_KILLED || exit.si_code == CLD_DUMPED)
                return Error("host.scan.crashed",
                             "plugin scanner crashed with signal " + std::to_string(exit.si_status),
                             "/path", "repair/reinstall the module to retry its scan");
            if (eof) break;
        }
        pollfd polled{eof ? -1 : child.fd, POLLIN, 0};
        if (::poll(&polled, 1, 10) < 0 && errno != EINTR)
            return Error("host.scan.io", "cannot poll scanner output");
    }
    auto parsed = json::parse(output, {.maxDepth = 32, .maxBytes = options.maxOutputBytes});
    if (!parsed) return invalidOutput();
    auto result = parseScanReply(*parsed, module);
    if (result && exit.si_status != 0) return invalidOutput();
    return result;
}
}  // namespace

json::Value scanReply(const Result<std::vector<PluginInfo>>& result) {
    auto reply = json::Value::object();
    reply["schema"] = kSchema;
    if (result) {
        reply["plugins"] = pluginsJson(*result);
    } else {
        auto error = json::Value::object();
        error["code"] = result.error().code;
        error["message"] = result.error().message;
        error["path"] = result.error().path;
        error["hint"] = result.error().hint;
        reply["error"] = std::move(error);
    }
    return reply;
}

Result<std::vector<PluginInfo>> parseScanReply(const json::Value& reply, const fs::path& module) {
    std::error_code ec;
    const bool vst3 = fs::is_directory(module, ec);
    const auto* schema = reply.find("schema");
    if (!schema || !schema->isString() || schema->asString() != kSchema) return invalidOutput();
    const auto* error = reply.find("error");
    const auto* plugins = reply.find("plugins");
    if (error) {
        if (plugins || !error->isObject()) return invalidOutput();
        for (const auto field : {"code", "message", "path", "hint"}) {
            const auto* v = error->find(field);
            if (!v || !v->isString()) return invalidOutput();
        }
        if (error->find("code")->asString().empty()) return invalidOutput();
        return Error(error->find("code")->asString(), error->find("message")->asString(),
                     error->find("path")->asString(), error->find("hint")->asString());
    }
    if (!plugins || !plugins->isArray() || plugins->size() > 4096) return invalidOutput();
    std::vector<PluginInfo> result;
    for (const auto& item : plugins->asArray()) {
        if (!item.isObject()) return invalidOutput();
        for (const auto field :
             {"id", "name", "vendor", "version", "description", "format", "path"}) {
            const auto* v = item.find(field);
            if (!v || !v->isString()) return invalidOutput();
        }
        const auto* instrument = item.find("instrument");
        const auto* semantic = item.find("semantic");
        const auto* features = item.find("features");
        if (!instrument || !instrument->isBool() || !semantic || !semantic->isBool() || !features ||
            !features->isArray() || item.find("path")->asString() != module.string() ||
            item.find("id")->asString().empty())
            return invalidOutput();
        PluginInfo info;
        info.id = item.find("id")->asString();
        info.name = item.find("name")->asString();
        info.vendor = item.find("vendor")->asString();
        info.version = item.find("version")->asString();
        info.description = item.find("description")->asString();
        info.format = item.find("format")->asString();
        if (info.format != (vst3 ? "vst3" : "clap")) return invalidOutput();
        info.path = module;
        info.instrument = instrument->asBool();
        info.semantic = semantic->asBool();
        for (const auto& feature : features->asArray()) {
            if (!feature.isString()) return invalidOutput();
            info.features.push_back(feature.asString());
        }
        result.push_back(std::move(info));
    }
    return result;
}

const ScanReport& lastScanReport() { return gReport; }

Result<std::vector<PluginInfo>> scan(std::span<const fs::path> paths) {
    return scan(paths, ScanOptions{});
}

Result<std::vector<PluginInfo>> scan(std::span<const fs::path> paths, const ScanOptions& options,
                                     ScanReport* report) {
    gReport = {};
    struct Publish {
        ScanReport* target;
        ~Publish() {
            if (target) *target = gReport;
        }
    } publish{report};
    if (options.timeoutMs <= 0 || options.maxOutputBytes == 0 ||
        options.maxOutputBytes > (64u << 20))
        return Error("host.scan.options",
                     "scanner needs a positive timeout and output limit <= 64 MiB");
    auto executable =
        options.bridgeExecutable.empty() ? bridgeExecutable() : options.bridgeExecutable;
    if (!options.forceInProcess && executable.empty()) {
        locateBridgeExecutable(std::string(product::kSlug) + "-bridge");
        executable = bridgeExecutable();
    }
    std::error_code executableError;
    const bool bridgeMissing =
        !options.forceInProcess &&
        (executable.empty() || !fs::is_regular_file(executable, executableError) ||
         ::access(executable.c_str(), X_OK) != 0);
    if (bridgeMissing) {
        gReport.warnings.emplace_back("host.scan.unsupervised",
                                      "plugin bridge unavailable; scanning in process", "/bridge",
                                      "install the bridge beside the application");
        std::fprintf(stderr, "%s\n", gReport.warnings.back().toString().c_str());
    }
    const auto directory =
        options.cacheDirectory.empty() ? cacheDirectory() : options.cacheDirectory;
    const auto stock = stockPluginDir();
    std::vector<PluginInfo> result;
    for (const auto& module : modules(paths)) {
        const bool trusted = stock && module.parent_path() == *stock;
        if (options.forceInProcess || trusted) {
            auto local = scanModuleInProcess(module);
            if (!local)
                gReport.failures.push_back({module, local.error(), false});
            else
                result.insert(result.end(), std::make_move_iterator(local->begin()),
                              std::make_move_iterator(local->end()));
            continue;
        }
        auto key = fingerprint(module);
        if (!key) {
            gReport.failures.push_back({module, key.error(), false});
            continue;
        }
        const fs::path canonical = key->find("path")->asString();
        const auto recordPath = cacheFile(directory, canonical);
        std::optional<json::Value> cached;
        if (!directory.empty()) {
            auto text = readTextFile(recordPath, options.maxOutputBytes + (1u << 20));
            if (text) {
                auto record = json::parse(
                    *text, {.maxDepth = 32, .maxBytes = options.maxOutputBytes + (1u << 20)});
                if (record && record->find("schema") && record->find("schema")->isString() &&
                    record->find("schema")->asString() == kSchema && record->find("key") &&
                    *record->find("key") == *key && record->find("reply"))
                    cached = *record->find("reply");
            }
        }
        bool cacheHit = false;
        auto outcome = [&]() -> Result<std::vector<PluginInfo>> {
            if (cached) {
                auto parsed = parseScanReply(*cached, canonical);
                if (parsed || (cached->find("error") && scanReply(parsed) == *cached)) {
                    cacheHit = true;
                    if (parsed)
                        for (auto& info : *parsed) info.path = module;
                    return parsed;
                }
            }
            return bridgeMissing ? scanModuleInProcess(module)
                                 : supervise(executable, module, options);
        }();
        if (cacheHit) ++gReport.cachedModules;
        if (!cacheHit && !bridgeMissing && !directory.empty()) {
            // Do not pin transient spawn/I/O failures, or a result for a module
            // that changed while its child was scanning.
            auto after = fingerprint(module);
            const bool persistent = outcome || outcome.error().code == "host.scan.crashed" ||
                                    outcome.error().code == "host.scan.timeout" ||
                                    outcome.error().code == "host.scan.invalid_output" ||
                                    outcome.error().code == "host.scan.output_limit" ||
                                    outcome.error().code == "host.load" ||
                                    outcome.error().code == "host.scan.load";
            if (persistent && after && *after == *key) {
                auto record = json::Value::object();
                record["schema"] = kSchema;
                record["key"] = *key;
                record["reply"] = scanReply(outcome);
                if (outcome)
                    for (auto& info : record["reply"]["plugins"].asArray())
                        info["path"] = canonical.string();
                auto written = writeFileAtomic(recordPath, json::writeCanonical(record));
                if (!written) gReport.warnings.push_back(written.error());
            }
        }
        if (!outcome)
            gReport.failures.push_back({module, outcome.error(), cacheHit});
        else
            result.insert(result.end(), std::make_move_iterator(outcome->begin()),
                          std::make_move_iterator(outcome->end()));
    }
    return result;
}
}  // namespace supple::host
