// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>
#include <cstdlib>
#include <string_view>
#include "../src/editor_platform.h"
#include "supple/host/clap_host.h"
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
                REQUIRE_MESSAGE(opened, opened ? "" : opened.error().toString());
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
