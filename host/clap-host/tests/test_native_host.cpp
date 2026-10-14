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
TEST_CASE("native CLAP and VST3 scan, render and restore state in process") {
    for (const fs::path module : {fs::path(FIXTURE_CLAP_PATH), fs::path(FIXTURE_VST3_PATH)}) {
        ScanOptions options;
        options.forceInProcess = true;
        auto catalogue = scan(std::span<const fs::path>(&module, 1), options);
        REQUIRE(catalogue);
        REQUIRE_FALSE(catalogue->empty());
        for (const auto& info : *catalogue) {
            if (info.name.find("Gain") == std::string::npos &&
                info.id != "com.suppleaudio.test.gain")
                continue;
            auto made = Instance::create(info, {});
            REQUIRE(made);
            auto& instance = **made;
            const auto params = instance.params();
            REQUIRE_FALSE(params.empty());
            REQUIRE(instance.setParam(params[0].id, params[0].def));
            auto saved = instance.saveState();
            REQUIRE(saved);
            REQUIRE(instance.loadState(*saved));
            REQUIRE(instance.activate(48000, 1, 64));
            REQUIRE(instance.startProcessing());
            std::array<float, 64> left{}, right{}, outLeft{}, outRight{};
            left.fill(0.25f);
            right.fill(0.25f);
            const float* inputs[]{left.data(), right.data()};
            float* outputs[]{outLeft.data(), outRight.data()};
            ProcessBlock block;
            block.inputs = inputs;
            block.outputs = outputs;
            block.frames = 64;
            block.inputChannels = 2;
            block.outputChannels = 2;
            REQUIRE(instance.process(block));
            CHECK(outLeft[0] == doctest::Approx(0.25f));
            instance.stopProcessing();
            instance.deactivate();
        }
    }
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
