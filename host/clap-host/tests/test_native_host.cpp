// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <array>
#include <filesystem>
#include <vector>
#include "supple/host/clap_host.h"
using namespace supple;
using namespace supple::host;
namespace fs = std::filesystem;
TEST_CASE("missing scan helper names the executable to install") {
    ScanOptions options;
    options.bridgeExecutable = fs::path(SCANNER_PATH) / "missing";
    options.superviseAll = true;
    auto result = scan({}, options);
    REQUIRE_FALSE(result);
    CHECK(result.error().code == "host.scan.unsupervised");
    CHECK(result.error().path == "/bridge");
#if defined(__linux__)
    CHECK(result.error().message == "a background scan needs the plugin bridge");
    CHECK(result.error().hint ==
          "install the bridge beside the application, or scan in the foreground");
#else
    CHECK(result.error().message == "a background scan needs the plugin scanner");
    CHECK(result.error().hint ==
          "install the scanner beside the application, or scan in the foreground");
#endif
}
TEST_CASE("native supervised scanner isolates crashes and hangs and caches results") {
    const auto work = fs::temp_directory_path() / "supple-native-scan";
    std::error_code ec;
    fs::remove_all(work, ec);
    fs::create_directories(work);
    struct Cleanup {
        fs::path path;
        ~Cleanup() {
            std::error_code error;
            fs::remove_all(path, error);
        }
    } cleanup{work};
    const std::vector<fs::path> modules{SCAN_GOOD_PATH, SCAN_CRASH_PATH, SCAN_HANG_PATH};
    ScanOptions options;
    options.bridgeExecutable = SCANNER_PATH;
    options.cacheDirectory = work / "cache";
    options.superviseAll = true;
    options.timeoutMs = 2000;
    ScanReport report;
    auto first = scan(modules, options, &report);
    REQUIRE(first);
    REQUIRE(first->size() == 1);
    REQUIRE(report.failures.size() == 2);
    CHECK(report.failures[0].reason.code == "host.scan.crashed");
    CHECK(report.failures[1].reason.code == "host.scan.timeout");
    REQUIRE(scan(modules, options, &report));
    CHECK(report.cachedModules == 3);
}
TEST_CASE("native VST3 bundle scanning is supervised and cached") {
    const fs::path module = FIXTURE_VST3_PATH;
    ScanOptions options;
    options.bridgeExecutable = SCANNER_PATH;
    options.superviseAll = true;
    options.cacheDirectory = fs::temp_directory_path() / "supple-native-vst3-scan";
    std::error_code ec;
    fs::remove_all(options.cacheDirectory, ec);
    ScanReport report;
    auto result = scan(std::span<const fs::path>(&module, 1), options, &report);
    REQUIRE(result);
    CHECK_FALSE(result->empty());
    CHECK(report.failures.empty());
    REQUIRE(scan(std::span<const fs::path>(&module, 1), options, &report));
    CHECK(report.cachedModules == 1);
    fs::remove_all(options.cacheDirectory, ec);
}
#if !defined(__linux__)
TEST_CASE("native bridge requests fail with actionable structured errors") {
    PluginInfo info;
    info.path = FIXTURE_CLAP_PATH;
    info.id = "com.suppleaudio.test.gain";
    auto instance = Instance::create(info, {}, true);
    REQUIRE_FALSE(instance);
    CHECK(instance.error().code == "host.bridge.unsupported");
    CHECK(instance.error().path == "/isolation");
    CHECK_FALSE(instance.error().hint.empty());
    auto shared = createBridgeProcess();
    REQUIRE_FALSE(shared);
    CHECK(shared.error().code == "host.bridge.unsupported");
}

#endif
