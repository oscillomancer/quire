// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "scan.h"

#include "bridge_client.h"
#include "supple/base/files.h"
#include "supple/json/json.h"
#include "supple/product.h"
#include "supple/script_export.hpp"

#include "platform.h"
#include "supple/base/paths.h"
#include "supple/host/child_process.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <set>

namespace supple::host {
namespace fs = std::filesystem;
namespace {
constexpr std::string_view kSchema = "supple.plugin-scan/1.0";
// The cache record's own version: 2 leaves the script player out.
constexpr std::int64_t kCacheVersion = 2;

// Whether the module is a script player (or a player export), whose plugins
// are the scripts in its library: what it offers changes while its file
// stays the same, so no record of it can be trusted.
bool scriptPlayer(const fs::path& module) {
    std::error_code ec;
    if (fs::is_directory(module, ec)) {
        for (fs::directory_iterator it(module / "Contents/MacOS", ec), end; it != end;
             it.increment(ec)) {
            if (ec) break;
            if (it->is_regular_file(ec) && scriptPlayer(it->path())) return true;
        }
        return false;
    }
    std::ifstream in(module, std::ios::binary);
    const std::string_view mark = scriptc::kPlayerMark;
    std::string window;
    char chunk[1 << 16];
    while (in.read(chunk, sizeof chunk) || in.gcount() > 0) {
        window.append(chunk, static_cast<std::size_t>(in.gcount()));
        if (window.find(mark) != std::string::npos) return true;
        window.erase(0, window.size() - std::min(window.size(), mark.size() - 1));
    }
    return false;
}
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
        item["path"] = platform::pathText(p.path);
        item["instrument"] = p.instrument;
        item["semantic"] = p.semantic;
        auto features = json::Value::array();
        for (const auto& f : p.features) features.push_back(f);
        item["features"] = std::move(features);
        list.push_back(std::move(item));
    }
    return list;
}

bool bundleDirectory(const fs::path& path) {
    if (path.extension() == ".vst3") return true;
#if defined(__APPLE__)
    if (path.extension() == ".clap") return true;
#endif
    return false;
}
bool moduleFile(const fs::path& path) {
    if (path.extension() == ".clap") return true;
#if defined(_WIN32)
    if (path.extension() == ".vst3") return true;
#endif
    return false;
}

std::vector<fs::path> modules(std::span<const fs::path> roots) {
    std::vector<fs::path> result;
    std::set<fs::path> seen;
    for (const auto& root : roots) {
        std::error_code ec;
        std::vector<fs::path> found;
        if (fs::is_regular_file(root, ec) ||
            (bundleDirectory(root) && fs::is_directory(root, ec))) {
            found.push_back(root);
        } else if (fs::is_directory(root, ec)) {
            for (fs::recursive_directory_iterator
                     it(root, fs::directory_options::skip_permission_denied, ec),
                 end;
                 it != end; it.increment(ec)) {
                if (ec) break;
                if (bundleDirectory(it->path()) && it->is_directory(ec)) {
                    found.push_back(it->path());
                    it.disable_recursion_pending();
                } else if (moduleFile(it->path()) && it->is_regular_file(ec)) {
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

// Apple libc++ uses a 128-bit file-clock representation. Keep every digit,
// including negative/minimum values, without narrowing or nonstandard overloads.
std::string timestampText(auto value) {
    const bool negative = value < 0;
    std::string text;
    do {
        const int digit = static_cast<int>(value % 10);
        text += static_cast<char>('0' + (digit < 0 ? -digit : digit));
        value /= 10;
    } while (value != 0);
    if (negative) text += '-';
    std::reverse(text.begin(), text.end());
    return text;
}

// A bundle's directory mtime alone misses replacement/touch of an existing
// binary or moduleinfo.json. Record every regular file's size and timestamp.
Result<json::Value> fingerprint(const fs::path& module) {
    std::error_code ec;
    const auto canonical = fs::canonical(module, ec);
    if (ec)
        return Error("host.scan.stat", "cannot resolve module " + platform::pathText(module),
                     "/path");
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
    key["path"] = platform::pathText(canonical);
    auto stats = json::Value::array();
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        if (ec) break;
        const auto size = fs::file_size(file, ec);
        if (ec) break;
        const auto time = fs::last_write_time(file, ec);
        if (ec) break;
        auto stat = json::Value::object();
        stat["path"] = platform::pathText(file.lexically_relative(module));
        // Strings preserve the filesystem's complete integer range in JSON.
        stat["size"] = std::to_string(size);
        stat["mtime"] = timestampText(time.time_since_epoch().count());
        stats.push_back(std::move(stat));
    }
    if (ec)
        return Error("host.scan.stat",
                     "cannot stat module " + platform::pathText(module) + ": " + ec.message(),
                     "/path", "check file permissions");
    key["files"] = std::move(stats);
    return key;
}

fs::path cacheDirectory() {
    auto root = paths::cache();
    if (!root.is_absolute()) {
#if defined(__linux__)
        const auto home = paths::home();
        root = home.is_absolute() ? home / ".cache" : fs::path{};
#else
        root.clear();
#endif
    }
    if (root.empty()) return {};
    return root / product::kSlug / "plugin-scan";
}

fs::path cacheFile(const fs::path& directory, const fs::path& module) {
    // A filename hash only: the full canonical path is checked in the record,
    // so even a hash collision can only cause a cache miss.
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : platform::pathText(module)) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return directory / (std::to_string(hash) + ".json");
}

Result<std::vector<PluginInfo>> supervise(const fs::path& executable, const fs::path& module,
                                          const ScanOptions& options) {
    const std::array<std::string, 2> args{"--scan", platform::pathText(module)};
    auto child = ChildProcess::spawn(executable, args);
    if (!child) return Error("host.scan.spawn", child.error().message, "/path", child.error().hint);
    auto output = (*child)->collect(
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.timeoutMs),
        options.maxOutputBytes);
    if (!output) {
        const auto& error = output.error();
        if (error.code == "child.timeout")
            return Error(
                "host.scan.timeout",
                "plugin scanner timed out after " + std::to_string(options.timeoutMs) + " ms",
                "/path", "repair/reinstall the module to retry its scan");
        if (error.code == "child.output_limit")
            return Error("host.scan.output_limit",
                         "plugin scanner output exceeds " + std::to_string(options.maxOutputBytes) +
                             " bytes",
                         "/path", "update the plugin/bridge");
        if (error.code == "child.wait")
            return Error("host.scan.wait", "cannot observe scanner exit", "/path", error.hint);
        return Error("host.scan.io", "cannot read scanner output", "/path", error.hint);
    }
    if (output->crashed) {
#if defined(_WIN32)
        constexpr const char* cause = "exit code ";
#else
        constexpr const char* cause = "signal ";
#endif
        return Error(
            "host.scan.crashed",
            "plugin scanner crashed with " + std::string(cause) + std::to_string(output->exitCode),
            "/path", "repair/reinstall the module to retry its scan");
    }
    auto parsed = json::parse(output->text, {.maxDepth = 32, .maxBytes = options.maxOutputBytes});
    if (!parsed) return invalidOutput();
    auto result = parseScanReply(*parsed, module);
    if (result && output->exitCode != 0) return invalidOutput();
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

Result<std::vector<PluginInfo>> parseScanReply(const json::Value& reply, const fs::path& module,
                                               std::optional<bool> vst3Format) {
    const bool vst3 = vst3Format.value_or(module.extension() == ".vst3");
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
            !features->isArray() || item.find("path")->asString() != platform::pathText(module) ||
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
    const std::string helper(platform::scanHelperName());
    auto executable =
        options.bridgeExecutable.empty() ? bridgeExecutable() : options.bridgeExecutable;
    if (!options.forceInProcess && executable.empty()) {
        locateBridgeExecutable(std::string(product::kSlug) + "-" + helper);
        executable = bridgeExecutable();
    }
    std::error_code executableError;
    const bool bridgeMissing =
        !options.forceInProcess &&
        (executable.empty() || !fs::is_regular_file(executable, executableError) ||
         !platform::executable(executable));
    if (bridgeMissing && options.superviseAll)
        return Error(
            "host.scan.unsupervised", "a background scan needs the plugin " + helper, "/bridge",
            "install the " + helper + " beside the application, or scan in the foreground");
    if (bridgeMissing) {
        gReport.warnings.emplace_back(
            "host.scan.unsupervised", "plugin " + helper + " unavailable; scanning in process",
            "/bridge", "install the " + helper + " beside the application");
        std::fprintf(stderr, "%s\n", gReport.warnings.back().toString().c_str());
    }
    const auto directory =
        options.cacheDirectory.empty() ? cacheDirectory() : options.cacheDirectory;
    const auto stock = stockPluginDir();
    std::vector<PluginInfo> result;
    for (const auto& module : modules(paths)) {
        const bool stockModule = stock && module.parent_path() == *stock;
        const bool trusted = !options.superviseAll && stockModule;
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
        const fs::path canonical = platform::pathFromText(key->find("path")->asString());
        const auto recordPath = cacheFile(directory, canonical);
        // The stock folder's modules are never cached (they are cheap to
        // scan), nor is a script player anywhere (see scriptPlayer(), checked
        // when a record would be written).
        const bool cacheable = !directory.empty() && !stockModule;
        std::optional<json::Value> cached;
        if (cacheable) {
            auto text = readTextFile(recordPath, options.maxOutputBytes + (1u << 20));
            if (text) {
                auto record = json::parse(
                    *text, {.maxDepth = 32, .maxBytes = options.maxOutputBytes + (1u << 20)});
                if (record && record->find("schema") && record->find("schema")->isString() &&
                    record->find("schema")->asString() == kSchema && record->find("version") &&
                    *record->find("version") == json::Value(kCacheVersion) && record->find("key") &&
                    *record->find("key") == *key && record->find("reply"))
                    cached = *record->find("reply");
            }
        }
        bool cacheHit = false;
        auto outcome = [&]() -> Result<std::vector<PluginInfo>> {
            if (cached) {
                auto parsed = parseScanReply(*cached, canonical, module.extension() == ".vst3");
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
        if (!cacheHit && !bridgeMissing && cacheable) {
            // Do not pin transient spawn/I/O failures, or a result for a module
            // that changed while its child was scanning.
            auto after = fingerprint(module);
            const bool persistent = outcome || outcome.error().code == "host.scan.crashed" ||
                                    outcome.error().code == "host.scan.timeout" ||
                                    outcome.error().code == "host.scan.invalid_output" ||
                                    outcome.error().code == "host.scan.output_limit" ||
                                    outcome.error().code == "host.load" ||
                                    outcome.error().code == "host.scan.load";
            if (persistent && after && *after == *key && !scriptPlayer(module)) {
                auto record = json::Value::object();
                record["schema"] = kSchema;
                record["version"] = kCacheVersion;
                record["key"] = *key;
                record["reply"] = scanReply(outcome);
                if (outcome)
                    for (auto& info : record["reply"]["plugins"].asArray())
                        info["path"] = platform::pathText(canonical);
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
