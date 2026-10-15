// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <limits>
#include "../src/bridge_protocol.h"
#include "../src/editor_platform.h"
#include "supple/host/clap_host.h"
using namespace supple;
using namespace supple::host;
namespace {
PluginInfo editorFixture(const std::filesystem::path& path, std::string_view name) {
    ScanOptions options;
    options.forceInProcess = true;
    auto list = scan(std::span(&path, 1), options);
    REQUIRE(list);
    auto it = std::find_if(list->begin(), list->end(),
                           [&](const auto& p) { return p.id == name || p.name == name; });
    REQUIRE(it != list->end());
    return *it;
}
}  // namespace
TEST_CASE("native CLAP editor parents retain pointer width and clean up partial opens") {
    const auto info = editorFixture(FIXTURE_CLAP_PATH, "com.suppleaudio.test.edge");
    setBridgeExecutable(BRIDGE_PATH);
    for (bool isolated : {false, true}) {
        // Cocoa remote windows require a display; covered by the opt-in test.
        if (isolated && nativeWindowKind() == NativeWindow::Kind::Cocoa) continue;
        auto made = Instance::create(info, {}, isolated);
        REQUIRE(made);
        auto& instance = **made;
        REQUIRE(instance.hasEditor());
        const NativeWindow parent{nativeWindowKind(), 0x123456789abULL};
        for (std::uint8_t mode : {3, 4, 6}) {
            const std::array<std::uint8_t, 4> state{mode, 0, 0, 0};
            REQUIRE(instance.loadState(state));
            auto opened = instance.openEditor(parent, 1.5);
            if (mode != 6) {
                CHECK_FALSE(opened);
                CHECK_FALSE(instance.editorOpen());
                continue;
            }
            REQUIRE(opened);
            CHECK(*instance.paramValue(8) == static_cast<double>(parent.handle));
            const auto requests = instance.takeEditorRequests();
            CHECK(requests.resize);
            CHECK(requests.show);
            CHECK(requests.hide);
            auto resized = instance.resizeEditor(30, 40);
            REQUIRE(resized);
            CHECK(resized->width == 100);
            CHECK(resized->height == 100);
            instance.closeEditor();
            instance.closeEditor();
            CHECK_FALSE(instance.editorOpen());
            REQUIRE(instance.openEditor(parent, 1));
            instance.closeEditor();
        }
        for (const auto invalidParent :
             {NativeWindow{}, NativeWindow{NativeWindow::Kind::Unavailable, 7}}) {
            auto bad = instance.openEditor(invalidParent, 1);
            REQUIRE_FALSE(bad);
            CHECK(bad.error().code == "host.editor.parent");
            CHECK_FALSE(bad.error().hint.empty());
            CHECK(bad.error().path == "/editor/parent");
        }
        CHECK_FALSE(instance.openEditor(parent, std::numeric_limits<double>::quiet_NaN()));
    }
}
TEST_CASE("native hosts advertise timers and only serviceable POSIX descriptors") {
    auto made = Instance::create(editorFixture(FIXTURE_CLAP_PATH, "com.suppleaudio.test.edge"), {});
    REQUIRE(made);
    const auto* plugin = (*made)->raw();
    const auto* host =
        static_cast<const clap_host_t*>(plugin->get_extension(plugin, "supple.test.edge-host"));
    REQUIRE(host);
    CHECK(host->get_extension(host, CLAP_EXT_TIMER_SUPPORT) != nullptr);
#if defined(_WIN32)
    CHECK(host->get_extension(host, CLAP_EXT_POSIX_FD_SUPPORT) == nullptr);
#else
    CHECK(host->get_extension(host, CLAP_EXT_POSIX_FD_SUPPORT) != nullptr);
#endif
}

TEST_CASE("native VST3 editors negotiate the platform API and repeatedly detach") {
    const auto info = editorFixture(FIXTURE_VST3_PATH, "Test VST3 Separate Gain");
    auto made = Instance::create(info, {});
    REQUIRE(made);
    REQUIRE((*made)->hasEditor());
    for (int cycle = 0; cycle < 3; ++cycle) {
        auto size = (*made)->openEditor({nativeWindowKind(), 0x123456789abULL}, 1.5);
        REQUIRE(size);
#if defined(__linux__)
        CHECK(size->width == 300);
        CHECK(size->height == 200);
#else
        CHECK(size->width == 320);
        CHECK(size->height == 240);
#endif
#if !defined(__linux__)
        auto requests = (*made)->takeEditorRequests();
        CHECK(requests.resize);
        CHECK(requests.width == 320);
        CHECK(requests.height == 240);
        REQUIRE((*made)->resizeEditor(500, 400));
#endif
        (*made)->closeEditor();
        CHECK_FALSE((*made)->editorOpen());
    }
}
TEST_CASE("an open native editor closes when its shared bridge crashes") {
    if (nativeWindowKind() == NativeWindow::Kind::Cocoa)
        return;  // display opt-in owns the NSWindow case
    setBridgeExecutable(BRIDGE_PATH);
    auto bridgeProcess = createBridgeProcess();
    REQUIRE(bridgeProcess);
    auto editor = Instance::create(editorFixture(FIXTURE_CLAP_PATH, "com.suppleaudio.test.edge"),
                                   {}, true, *bridgeProcess);
    auto crash = Instance::create(editorFixture(FIXTURE_CLAP_PATH, "com.suppleaudio.test.crash"),
                                  {}, true, *bridgeProcess);
    REQUIRE(editor);
    REQUIRE(crash);
    REQUIRE((*editor)->openEditor({nativeWindowKind(), 0x123456789abULL}, 1));
    REQUIRE((*crash)->activate(48000, 1, 64));
    REQUIRE((*crash)->setParam(1, 1));
    REQUIRE((*crash)->startProcessing());
    std::array<float, 64> left{}, right{};
    float* outputs[]{left.data(), right.data()};
    ProcessBlock block;
    block.outputs = outputs;
    block.outputChannels = 2;
    block.frames = 64;
    REQUIRE((*crash)->process(block));
    CHECK((*editor)->failed());
    CHECK((*editor)->takeEditorRequests().closed);
    CHECK_FALSE((*editor)->editorOpen());
    (*editor)->closeEditor();
    (*crash)->stopProcessing();
    (*crash)->deactivate();
}

TEST_CASE("reopening a bridged editor discards the previous window's close snapshot") {
    if (nativeWindowKind() == NativeWindow::Kind::Cocoa)
        return;  // native windows require the display opt-in
    setBridgeExecutable(BRIDGE_PATH);
    auto made =
        Instance::create(editorFixture(FIXTURE_CLAP_PATH, "com.suppleaudio.test.edge"), {}, true);
    REQUIRE(made);
    const std::array<std::uint8_t, 4> state{47, 0, 0, 0};
    REQUIRE((*made)->loadState(state));
    NativeWindow parent{nativeWindowKind(), 0x123456789abULL};
    REQUIRE((*made)->openEditor(parent, 1));
    // A control round-trip lets child main retain its close snapshot without
    // consuming it into the parent's editor cache.
    REQUIRE((*made)->paramValue(8));
    REQUIRE((*made)->openEditor(parent, 1));
    CHECK_FALSE((*made)->takeEditorRequests().closed);
    CHECK((*made)->editorOpen());
    (*made)->closeEditor();
}

TEST_CASE("typed editor protocol rejects old payloads and unknown parent kinds") {
    bridge::Writer old;
    old.u64(123);
    old.f64(1);
    CHECK_FALSE(bridge::validRequest(bridge::Op::EditorOpen, {old.bytes.data(), old.bytes.size()}));
    bridge::Writer typed;
    typed.u8(static_cast<std::uint8_t>(nativeWindowKind()));
    typed.u64(0x123456789abULL);
    typed.f64(1);
    CHECK(bridge::validRequest(bridge::Op::EditorOpen, {typed.bytes.data(), typed.bytes.size()}));
    typed.bytes[0] = 255;
    CHECK_FALSE(
        bridge::validRequest(bridge::Op::EditorOpen, {typed.bytes.data(), typed.bytes.size()}));
}
TEST_CASE("stock modules advertise the native editor API without creating a window") {
#if SUPPLE_TEST_STOCK_EDITORS
    auto directory = stockPluginDir();
    if (!directory) {
        MESSAGE("skipped: this partial build has no staged stock modules");
        return;
    }
    ScanOptions options;
    options.forceInProcess = true;
    auto list = scan(std::span(&*directory, 1), options);
    REQUIRE(list);
    if (list->empty()) {
        MESSAGE("skipped: this partial build has no staged stock modules");
        return;
    }
    int count = 0;
    for (const auto& info : *list) {
        auto made = Instance::create(info, {});
        REQUIRE(made);
        if ((*made)->hasEditor()) ++count;
        CHECK_FALSE((*made)->editorOpen());
    }
    CHECK(count >= 8);
#endif
}

TEST_CASE("Cocoa bridge editor parents carry server window numbers without pointers") {
    const auto pointer = cocoaBridgeParent({NativeWindow::Kind::Cocoa, 0x123456789abULL});
    CHECK(pointer.kind == NativeWindow::Kind::Floating);
    CHECK(pointer.handle == 0);
    for (std::uintptr_t number :
         {std::uintptr_t{0}, std::uintptr_t{1234}, std::uintptr_t{0xffffffffU}}) {
        const auto parent = cocoaBridgeParent({NativeWindow::Kind::Floating, number});
        CHECK(validFloatingEditorParent(parent));
        bridge::Writer payload;
        payload.u8(static_cast<std::uint8_t>(parent.kind));
        payload.u64(parent.handle);
        payload.f64(1);
        REQUIRE(bridge::validRequest(bridge::Op::EditorOpen,
                                     {payload.bytes.data(), payload.bytes.size()}));
        bridge::Reader read{payload.bytes.data(), payload.bytes.size()};
        CHECK(read.u8() == static_cast<std::uint8_t>(NativeWindow::Kind::Floating));
        CHECK(read.u64() == number);
        CHECK(read.f64() == 1);
        CHECK(read.ok());
    }
    const NativeWindow invalid{NativeWindow::Kind::Floating, 0x100000000ULL};
    CHECK_FALSE(validFloatingEditorParent(invalid));
    CHECK_FALSE(validFloatingEditorParent({NativeWindow::Kind::Cocoa, 1234}));
    bridge::Writer payload;
    payload.u8(static_cast<std::uint8_t>(invalid.kind));
    payload.u64(invalid.handle);
    payload.f64(1);
    CHECK_FALSE(
        bridge::validRequest(bridge::Op::EditorOpen, {payload.bytes.data(), payload.bytes.size()}));
    if (nativeWindowKind() == NativeWindow::Kind::Cocoa) {
        setBridgeExecutable(BRIDGE_PATH);
        auto instance = Instance::create(
            editorFixture(FIXTURE_CLAP_PATH, "com.suppleaudio.test.edge"), {}, true);
        REQUIRE(instance);
        auto opened = (*instance)->openEditor(invalid, 1);
        REQUIRE_FALSE(opened);
        CHECK(opened.error().code == "host.editor.parent");
    }
}
