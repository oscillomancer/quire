// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <algorithm>
#include <cstdlib>
#include <string_view>
#include "../src/editor_platform.h"
#include "supple/host/clap_host.h"
#if defined(__APPLE__)
#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#include "../src/bridge_client.h"
#endif
#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
using namespace supple;
using namespace supple::host;
TEST_CASE("native stock editors repeatedly open locally and in a bridge (display opt-in)") {
    const char* enabled = std::getenv("SUPPLE_GUI_DISPLAY_TEST");
    if (!enabled || std::string_view(enabled) != "1") return;
#if !defined(__linux__) && SUPPLE_TEST_STOCK_EDITORS
    REQUIRE(initializeEditorPlatform());
    auto directory = stockPluginDir();
    REQUIRE(directory);
    ScanOptions options;
    options.forceInProcess = true;
    auto list = scan(std::span(&*directory, 1), options);
    REQUIRE(list);
    setBridgeExecutable(BRIDGE_PATH);
    for (bool isolated : {false, true}) {
        for (const auto& info : *list) {
            auto made = Instance::create(info, {}, isolated);
            REQUIRE(made);
            if (!(*made)->hasEditor()) continue;
            CAPTURE(info.name);
            CAPTURE(isolated);
#if defined(__APPLE__)
            auto window = FloatingEditorWindow::create(info.name);
            REQUIRE(window);
            NativeWindow parent =
                isolated ? NativeWindow{NativeWindow::Kind::Floating, 0} : (*window)->parent();
#else
            HWND window = CreateWindowExW(0, L"STATIC", L"Editor host test", WS_OVERLAPPEDWINDOW,
                                          CW_USEDEFAULT, CW_USEDEFAULT, 640, 400, nullptr, nullptr,
                                          GetModuleHandleW(nullptr), nullptr);
            REQUIRE(window);
            struct Cleanup {
                HWND window;
                ~Cleanup() { DestroyWindow(window); }
            } cleanup{window};
            NativeWindow parent{NativeWindow::Kind::Win32,
                                reinterpret_cast<std::uintptr_t>(window)};
#endif
            for (int cycle = 0; cycle < 3; ++cycle) {
                auto opened = (*made)->openEditor(parent, 1);
                REQUIRE_MESSAGE(opened, (opened ? "" : opened.error().toString()));
#if defined(__APPLE__)
                if (!isolated) {
                    (*window)->resize(opened->width, opened->height, opened->resizable);
                    (*window)->show();
                }
#else
                RECT rect{0, 0, static_cast<LONG>(opened->width),
                          static_cast<LONG>(opened->height)};
                AdjustWindowRectEx(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0);
                SetWindowPos(window, nullptr, 0, 0, rect.right - rect.left, rect.bottom - rect.top,
                             SWP_NOMOVE | SWP_NOZORDER);
                ShowWindow(window, SW_SHOW);
#endif
                for (int event = 0; event < 16; ++event) {
                    waitEditorEvents({}, 10);
                    (*made)->serviceMainThread();
                    (*made)->takeEditorRequests();
                }
                if (opened->resizable)
                    REQUIRE((*made)->resizeEditor(opened->width, opened->height));
                (*made)->closeEditor();
                CHECK_FALSE((*made)->editorOpen());
                CHECK_FALSE((*made)->failed());
            }
        }
    }
#endif
}

#if defined(__APPLE__)
TEST_CASE(
    "Cocoa bridged editors use normal level and reorder without activation (display opt-in)") {
    const char* enabled = std::getenv("SUPPLE_GUI_DISPLAY_TEST");
    if (!enabled || std::string_view(enabled) != "1") return;
    @autoreleasepool {
        auto owner = FloatingEditorWindow::create("Editor ordering owner");
        auto other = FloatingEditorWindow::create("Unrelated normal window");
        REQUIRE(owner);
        REQUIRE(other);
        auto nsWindow = [](const FloatingEditorWindow& window) {
            return [(__bridge NSView*)reinterpret_cast<void*>(window.parent().handle) window];
        };
        NSWindow* ownerWindow = nsWindow(**owner);
        NSWindow* otherWindow = nsWindow(**other);
        [ownerWindow makeKeyAndOrderFront:nil];
        [NSApp activate];
        [NSApp updateWindows];
        const auto activePid = NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier;
        ScanOptions options;
        options.forceInProcess = true;
        const std::filesystem::path path = FIXTURE_CLAP_PATH;
        auto scanned = scan(std::span(&path, 1), options);
        REQUIRE(scanned);
        const auto found = std::find_if(scanned->begin(), scanned->end(), [](const auto& info) {
            return info.id == "com.suppleaudio.test.edge";
        });
        REQUIRE(found != scanned->end());
        setBridgeExecutable(BRIDGE_PATH);
        auto process = createBridgeProcess();
        REQUIRE(process);
        auto instance = Instance::create(*found, {}, true, *process);
        REQUIRE(instance);
        const NativeWindow parent{NativeWindow::Kind::Floating,
                                  static_cast<std::uintptr_t>(ownerWindow.windowNumber)};
        REQUIRE((*instance)->openEditor(parent, 1));
        auto windows = [] {
            return CFBridgingRelease(
                CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly, kCGNullWindowID));
        };
        auto editorInfo = [&](NSArray* list) -> NSDictionary* {
            for (NSDictionary* info in list)
                if ([info[(__bridge NSString*)kCGWindowOwnerPID] intValue] == (*process)->pid())
                    return info;
            return nil;
        };
        NSArray* list = windows();
        NSDictionary* editor = editorInfo(list);
        REQUIRE_MESSAGE(editor, "run in an interactive macOS desktop with window-list access");
        NSNumber* editorNumber = editor[(__bridge NSString*)kCGWindowNumber];
        auto position = [](NSArray* entries, NSNumber* number) {
            for (NSUInteger i = 0; i < entries.count; ++i)
                if ([entries[i][(__bridge NSString*)kCGWindowNumber] isEqual:number]) return i;
            return NSNotFound;
        };
        REQUIRE(position(list, @(ownerWindow.windowNumber)) != NSNotFound);
        CHECK([editor[(__bridge NSString*)kCGWindowLayer] intValue] == NSNormalWindowLevel);
        CHECK(position(list, editorNumber) < position(list, @(ownerWindow.windowNumber)));
        CHECK(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == activePid);
        // A different normal-level window can cover the editor. Raising it again
        // places it just above its owner, without overtaking that unrelated window.
        [otherWindow orderFrontRegardless];
        [NSApp updateWindows];
        REQUIRE((*instance)->openEditor(parent, 1));
        list = windows();
        REQUIRE(editorInfo(list));
        REQUIRE(position(list, @(otherWindow.windowNumber)) != NSNotFound);
        REQUIRE(position(list, @(ownerWindow.windowNumber)) != NSNotFound);
        CHECK([editorInfo(list)[(__bridge NSString*)kCGWindowNumber] isEqual:editorNumber]);
        CHECK(position(list, @(otherWindow.windowNumber)) < position(list, editorNumber));
        CHECK(position(list, editorNumber) < position(list, @(ownerWindow.windowNumber)));
        CHECK(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == activePid);
        // Reopen can replace the owner without recreating the plugin view/window.
        const NativeWindow otherParent{NativeWindow::Kind::Floating,
                                       static_cast<std::uintptr_t>(otherWindow.windowNumber)};
        REQUIRE((*instance)->openEditor(otherParent, 1));
        list = windows();
        REQUIRE(position(list, editorNumber) != NSNotFound);
        REQUIRE(position(list, @(otherWindow.windowNumber)) != NSNotFound);
        CHECK(position(list, editorNumber) < position(list, @(otherWindow.windowNumber)));
        CHECK(NSWorkspace.sharedWorkspace.frontmostApplication.processIdentifier == activePid);
        (*instance)->closeEditor();
    }
}
#endif
