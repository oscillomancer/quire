// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The system's file dialogs, behind Ui's FileDialogs, and the URI lists
// files arrive as when dropped.
//
// None of them blocks the host. On Linux the dialog is zenity's or
// kdialog's, run as a child process and read when it exits (so a desktop
// without either has none: open() returns false). On Windows it is the
// common item dialog, run on a thread of its own. On macOS it is an
// NSOpenPanel or NSSavePanel, which runs in the host's event loop. Results
// are delivered from poll(), which Window calls every tick, on the main
// thread; a dialog still open when the editor closes is closed with it.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gui/Ui.h"

namespace supple::gui {

class NativeFiles : public FileDialogs {
  public:
    // `parent`: the editor's native view (an X11 Window, an HWND, an NSView*).
    explicit NativeFiles(uintptr_t parent);
    ~NativeFiles() override;
    NativeFiles(const NativeFiles&) = delete;
    NativeFiles& operator=(const NativeFiles&) = delete;

    bool open(const Options& options, Done done) override;
    bool save(const Options& options, Done done) override;
    // Main thread, every tick: delivers a finished dialog's paths.
    void poll();

    struct Platform;  // what is open, per system

  private:
    bool start(const Options& options, bool saving, Done done);

    uintptr_t parent_;
    Done done_;
    std::unique_ptr<Platform> platform_;
};

// The paths in a text/uri-list (RFC 2483), as UTF-8: file: URIs only,
// percent-decoded, comments and blank lines skipped. "file:///a%20b" and
// "file://localhost/a%20b" are "/a b"; on Windows "file:///C:/x" is "C:/x".
std::vector<std::string> pathsFromUriList(std::string_view list);

// The command line a Linux dialog is run with (tested; the first element
// is the program), for the dialog program named.
std::vector<std::string> linuxDialogCommand(std::string_view program, const FileDialogs::Options& options, bool saving,
                                            uintptr_t parent);

}  // namespace supple::gui
