// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <limits>
#include "../src/bridge_notifications.h"
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
TEST_CASE("a live display-scale change reaches the plug-in and keeps a logical size") {
    const auto info = editorFixture(FIXTURE_CLAP_PATH, "com.suppleaudio.test.edge");
    setBridgeExecutable(BRIDGE_PATH);
    const bool takesScale = editorTakesScale(nativeWindowKind());
    for (bool isolated : {false, true}) {
        // Cocoa remote windows require a display; covered by the opt-in test.
        if (isolated && nativeWindowKind() == NativeWindow::Kind::Cocoa) continue;
        CAPTURE(isolated);
        auto made = Instance::create(info, {}, isolated);
        REQUIRE(made);
        auto& instance = **made;
        const NativeWindow parent{nativeWindowKind(), 0x123456789abULL};
        const auto scaleSeen = [&] { return *instance.paramValue(11); };
        const auto scaleCalls = [&] { return *instance.paramValue(12); };

        // Nothing is open: no factor to renegotiate.
        auto early = instance.setEditorScale(2);
        REQUIRE_FALSE(early);
        CHECK(early.error().code == "host.editor");

        // A plug-in that keeps its size: the resizable editor keeps the logical
        // size, 300x200 at 1x becomes 450x300 at 1.5x and 300x200 again at 1x.
        auto opened = instance.openEditor(parent, 1);
        REQUIRE(opened);
        CHECK(opened->width == 300);
        CHECK(scaleCalls() == (takesScale ? 1 : 0));
        auto grown = instance.setEditorScale(1.5);
        REQUIRE(grown);
        CHECK(grown->resizable);
        CHECK(grown->width == (takesScale ? 450u : 300u));
        CHECK(grown->height == (takesScale ? 300u : 200u));
        CHECK(scaleSeen() == (takesScale ? 1.5 : 0.0));
        CHECK(scaleCalls() == (takesScale ? 2 : 0));
        auto back = instance.setEditorScale(1);
        REQUIRE(back);
        CHECK(back->width == 300);
        CHECK(back->height == 200);
        CHECK(scaleSeen() == (takesScale ? 1.0 : 0.0));

        // Invalid factors are refused without reaching the plug-in.
        for (double bad : {0.0, -1.0, std::numeric_limits<double>::infinity(),
                           std::numeric_limits<double>::quiet_NaN()}) {
            auto refused = instance.setEditorScale(bad);
            REQUIRE_FALSE(refused);
            CHECK(refused.error().code == "host.editor.scale");
            CHECK(refused.error().path == "/editor/scale");
            CHECK_FALSE(refused.error().hint.empty());
        }
        CHECK(scaleCalls() == (takesScale ? 3 : 0));
        instance.closeEditor();
        CHECK_FALSE(instance.setEditorScale(2));

        // A plug-in that rescales itself answers with its own size; the host
        // does not scale it a second time.
        const std::array<std::uint8_t, 4> selfScaling{7, 0, 0, 0};
        REQUIRE(instance.loadState(selfScaling));
        REQUIRE(instance.openEditor(parent, 1));
        auto own = instance.setEditorScale(2);
        REQUIRE(own);
        CHECK(own->width == (takesScale ? 600u : 300u));
        CHECK(own->height == (takesScale ? 400u : 200u));
        instance.closeEditor();
    }
}

TEST_CASE("a live display-scale change reaches a VST3 content-scale view") {
    const auto info = editorFixture(FIXTURE_VST3_PATH, "Test VST3 Separate Gain");
    auto made = Instance::create(info, {});
    REQUIRE(made);
    auto& instance = **made;
    REQUIRE(instance.openEditor({nativeWindowKind(), 0x123456789abULL}, 1));
    auto size = instance.setEditorScale(2);
    REQUIRE(size);
#if defined(__linux__)
    // The fixture's X11 view cannot resize, so its size is its own.
    CHECK_FALSE(size->resizable);
    CHECK(size->width == 300);
    CHECK(size->height == 200);
#else
    CHECK(size->resizable);
    if (editorTakesScale(nativeWindowKind())) {
        CHECK(size->width == 640);  // 320x240 at 1x, kept at 2x
        CHECK(size->height == 480);
    } else {
        CHECK(size->width == 320);
        CHECK(size->height == 240);
    }
#endif
    CHECK_FALSE(instance.setEditorScale(0));
    instance.closeEditor();
    CHECK_FALSE(instance.setEditorScale(2));
}

TEST_CASE("the rescale rule only sizes pixels for editors that take a scale") {
    CHECK(editorTakesScale(NativeWindow::Kind::X11));
    CHECK(editorTakesScale(NativeWindow::Kind::Win32));
    CHECK_FALSE(editorTakesScale(NativeWindow::Kind::Cocoa));
    CHECK_FALSE(editorTakesScale(NativeWindow::Kind::Floating));
    CHECK_FALSE(editorTakesScale(NativeWindow::Kind::Unavailable));
    std::uint32_t w = 800, h = 600;
    rescaleEditorSize(w, h, 1, 1.5);
    CHECK(w == 1200);
    CHECK(h == 900);
    rescaleEditorSize(w, h, 1.5, 1.25);
    CHECK(w == 1000);
    CHECK(h == 750);
    std::uint32_t tiny = 1, tall = 3;
    rescaleEditorSize(tiny, tall, 4, 1);
    CHECK(tiny == 1);  // never zero
    CHECK(tall == 1);
    rescaleEditorSize(w, h, 0, 2);  // an unusable source scale leaves the size alone
    CHECK(w == 1000);
    CHECK(validEditorScale(1));
    CHECK_FALSE(validEditorScale(0));
    CHECK_FALSE(validEditorScale(-2));
    CHECK_FALSE(validEditorScale(std::numeric_limits<double>::infinity()));
}

TEST_CASE("the editor scale request round-trips through the bridge protocol") {
    bridge::Writer request;
    request.f64(1.75);
    CHECK(bridge::validRequest(bridge::Op::EditorScale,
                               {request.bytes.data(), request.bytes.size()}));
    bridge::Reader read{request.bytes.data(), request.bytes.size()};
    CHECK(read.f64() == 1.75);
    CHECK(read.done());
    request.u8(0);  // trailing bytes
    CHECK_FALSE(bridge::validRequest(bridge::Op::EditorScale,
                                     {request.bytes.data(), request.bytes.size()}));
    CHECK_FALSE(bridge::validRequest(bridge::Op::EditorScale, {nullptr, 0}));
    bridge::Writer reply;  // the size to give the window, as EditorOpen replies
    reply.u32(450);
    reply.u32(300);
    reply.u8(1);
    CHECK(bridge::validReply(bridge::Op::EditorScale, {reply.bytes.data(), reply.bytes.size()}));
    reply.u8(0);
    CHECK_FALSE(
        bridge::validReply(bridge::Op::EditorScale, {reply.bytes.data(), reply.bytes.size()}));
}

TEST_CASE("editor window frames round-trip through the bridge protocol") {
    using Frame = Instance::EditorFrame;
    const auto request = [](std::optional<Frame> frame) {
        bridge::Writer w;
        w.u8(static_cast<std::uint8_t>(NativeWindow::Kind::Floating));
        w.u64(0);
        w.f64(1);
        bridge::writeFrame(w, frame);
        return w;
    };
    for (const std::optional<Frame> frame :
         {std::optional<Frame>{}, std::optional<Frame>{Frame{-1440, 25, 0, 0}},
          std::optional<Frame>{Frame{120, 80, 800, 500}}}) {
        const auto w = request(frame);
        CHECK(bridge::validRequest(bridge::Op::EditorOpen, {w.bytes.data(), w.bytes.size()}));
        bridge::Reader r{w.bytes.data() + 17, w.bytes.size() - 17};
        CHECK(bridge::readFrame(r) == frame);
        CHECK(r.done());
        // The same frame closes a window and rides along with its requests.
        bridge::Writer closed;
        bridge::writeFrame(closed, frame);
        CHECK(bridge::validReply(bridge::Op::EditorClose,
                                 {closed.bytes.data(), closed.bytes.size()}));
        bridge::Writer requests;
        for (int i = 0; i < 2; ++i) requests.u8(0);
        requests.u32(0);
        requests.u32(0);
        requests.u8(0);
        requests.u8(0);
        bridge::writeFrame(requests, frame);
        CHECK(bridge::validReply(bridge::Op::EditorRequests,
                                 {requests.bytes.data(), requests.bytes.size()}));
    }
    // Malformed: a flag above 1, an absent frame with values, absurd sizes, and
    // the pre-frame payloads of an older peer.
    auto bad = request(std::nullopt);
    bad.bytes[17] = 2;
    CHECK_FALSE(bridge::validRequest(bridge::Op::EditorOpen, {bad.bytes.data(), bad.bytes.size()}));
    bad = request(std::nullopt);
    bad.bytes[18] = 1;  // x != 0 without a frame
    CHECK_FALSE(bridge::validRequest(bridge::Op::EditorOpen, {bad.bytes.data(), bad.bytes.size()}));
    bad = request(Frame{0, 0, bridge::kMaxEditorFrame + 1, 10});
    CHECK_FALSE(bridge::validRequest(bridge::Op::EditorOpen, {bad.bytes.data(), bad.bytes.size()}));
    bridge::Writer old;
    for (int i = 0; i < 12; ++i) old.u8(0);
    CHECK_FALSE(
        bridge::validReply(bridge::Op::EditorRequests, {old.bytes.data(), old.bytes.size()}));
    CHECK_FALSE(bridge::validReply(bridge::Op::EditorClose, {nullptr, 0}));
}

TEST_CASE("the latest editor window frame survives merged requests") {
    Instance::EditorRequests pending;
    CHECK_FALSE(bridge::hasEditorRequests(pending));
    Instance::EditorRequests moved;
    moved.frame = Instance::EditorFrame{10, 20, 300, 200};
    CHECK(bridge::hasEditorRequests(moved));  // a frame alone is worth a wake
    bridge::mergeEditorRequests(pending, moved);
    Instance::EditorRequests resized;
    resized.frame = Instance::EditorFrame{10, 20, 640, 480};
    bridge::mergeEditorRequests(pending, resized);
    Instance::EditorRequests other;  // later requests without a frame keep it
    other.show = true;
    bridge::mergeEditorRequests(pending, other);
    REQUIRE(pending.frame);
    CHECK(pending.frame->width == 640);
    CHECK(pending.frame->height == 480);
    CHECK(pending.show);
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
    // Protocol 19/20 ends the payload with the window frame to restore.
    CHECK_FALSE(
        bridge::validRequest(bridge::Op::EditorOpen, {typed.bytes.data(), typed.bytes.size()}));
    bridge::writeFrame(typed, std::nullopt);
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
        bridge::writeFrame(payload, std::nullopt);
        REQUIRE(bridge::validRequest(bridge::Op::EditorOpen,
                                     {payload.bytes.data(), payload.bytes.size()}));
        bridge::Reader read{payload.bytes.data(), payload.bytes.size()};
        CHECK(read.u8() == static_cast<std::uint8_t>(NativeWindow::Kind::Floating));
        CHECK(read.u64() == number);
        CHECK(read.f64() == 1);
        CHECK_FALSE(bridge::readFrame(read));
        CHECK(read.done());
    }
    const NativeWindow invalid{NativeWindow::Kind::Floating, 0x100000000ULL};
    CHECK_FALSE(validFloatingEditorParent(invalid));
    CHECK_FALSE(validFloatingEditorParent({NativeWindow::Kind::Cocoa, 1234}));
    bridge::Writer payload;
    payload.u8(static_cast<std::uint8_t>(invalid.kind));
    payload.u64(invalid.handle);
    payload.f64(1);
    bridge::writeFrame(payload, std::nullopt);
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
