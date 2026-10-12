// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio

#include "gui/NativeFiles.h"

#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#include <atomic>
#include <mutex>
#include <thread>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shobjidl.h>
#elif !defined(__APPLE__)
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;
#endif

namespace supple::gui {

// --- shared ------------------------------------------------------------------------

std::vector<std::string> pathsFromUriList(std::string_view list) {
    std::vector<std::string> paths;
    while (!list.empty()) {
        const size_t end = list.find('\n');
        std::string_view line = list.substr(0, end);
        list = end == std::string_view::npos ? std::string_view{} : list.substr(end + 1);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\0' || line.back() == ' ')) line.remove_suffix(1);
        if (line.empty() || line.front() == '#') continue;
        if (line.substr(0, 5) != "file:") continue;
        line.remove_prefix(5);
        if (line.substr(0, 2) == "//") {
            line.remove_prefix(2);
            const size_t slash = line.find('/');
            if (slash == std::string_view::npos) continue;
            const std::string_view host = line.substr(0, slash);
            if (!host.empty() && host != "localhost") continue;  // another machine's file
            line.remove_prefix(slash);
        }
        auto hex = [](char c) {
            return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        };
        std::string path;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '%' && i + 2 < line.size()) {
                const int hi = hex(line[i + 1]), lo = hex(line[i + 2]);
                if (hi >= 0 && lo >= 0) {
                    path += static_cast<char>(hi * 16 + lo);
                    i += 2;
                    continue;
                }
            }
            path += line[i];
        }
        // "/C:/Users/..." is a Windows drive path.
        if (path.size() >= 3 && path[0] == '/' && path[2] == ':') path.erase(0, 1);
        if (!path.empty()) paths.push_back(std::move(path));
    }
    return paths;
}

std::vector<std::string> linuxDialogCommand(std::string_view program, const FileDialogs::Options& o, bool saving,
                                            uintptr_t parent) {
    std::vector<std::string> args{std::string(program)};
    std::string start = o.folder;
    if (saving && !o.name.empty()) start = start.empty() ? o.name : start + "/" + o.name;
    std::string patterns;
    for (const std::string& e : o.extensions) patterns += (patterns.empty() ? "*" : " *") + e;
    if (program == "kdialog") {
        args.push_back(saving ? "--getsavefilename" : "--getopenfilename");
        args.push_back(start.empty() ? "." : start);
        if (!patterns.empty()) args.push_back(patterns);
        if (!saving && o.multiple) {
            args.push_back("--multiple");
            args.push_back("--separate-output");
        }
        if (!o.title.empty()) {
            args.push_back("--title");
            args.push_back(o.title);
        }
        if (parent) {
            args.push_back("--attach");
            args.push_back(std::to_string(parent));
        }
    } else {
        args.push_back("--file-selection");
        if (!o.title.empty()) args.push_back("--title=" + o.title);
        if (saving) {
            args.push_back("--save");
            args.push_back("--confirm-overwrite");
        } else if (o.multiple) {
            args.push_back("--multiple");
            args.push_back("--separator=\n");
        }
        if (!start.empty()) args.push_back("--filename=" + start);
        if (!patterns.empty()) args.push_back("--file-filter=" + patterns);
        if (parent) args.push_back("--attach=" + std::to_string(parent));
    }
    return args;
}

bool NativeFiles::open(const Options& options, Done done) { return start(options, false, std::move(done)); }
bool NativeFiles::save(const Options& options, Done done) { return start(options, true, std::move(done)); }

#if defined(_WIN32)

// --- Windows: the common item dialog, on a thread of its own -------------------------------

namespace {
std::wstring wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}
std::string utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::string pathOf(IShellItem* item) {
    PWSTR name = nullptr;
    std::string path;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &name)) && name) {
        path = utf8(name);
        CoTaskMemFree(name);
    }
    return path;
}
}  // namespace

struct NativeFiles::Platform {
    std::thread thread;
    std::atomic<DWORD> threadId{0};
    std::atomic<bool> finished{false};
    std::mutex lock;
    std::vector<std::string> paths;

    ~Platform() {
        if (!thread.joinable()) return;
        // Closing the dialog's windows ends its modal loop; then it returns.
        while (!finished.load()) {
            EnumThreadWindows(threadId.load(), [](HWND w, LPARAM) -> BOOL { PostMessageW(w, WM_CLOSE, 0, 0); return TRUE; }, 0);
            Sleep(10);
        }
        thread.join();
    }
};

bool NativeFiles::start(const Options& o, bool saving, Done done) {
    if (platform_) return false;
    platform_ = std::make_unique<Platform>();
    done_ = std::move(done);
    Platform* p = platform_.get();
    const HWND owner = GetAncestor(reinterpret_cast<HWND>(parent_), GA_ROOT);
    p->thread = std::thread([p, o, saving, owner] {
        p->threadId.store(GetCurrentThreadId());
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE));
        std::vector<std::string> chosen;
        IFileDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(saving ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                       saving ? IID_IFileSaveDialog : IID_IFileOpenDialog,
                                       reinterpret_cast<void**>(&dialog)))) {
            DWORD flags = 0;
            dialog->GetOptions(&flags);
            flags |= FOS_FORCEFILESYSTEM;
            if (saving) flags |= FOS_OVERWRITEPROMPT;
            else if (o.multiple) flags |= FOS_ALLOWMULTISELECT;
            dialog->SetOptions(flags);
            if (!o.title.empty()) dialog->SetTitle(wide(o.title).c_str());
            if (saving && !o.name.empty()) dialog->SetFileName(wide(o.name).c_str());
            std::wstring spec;
            for (const std::string& e : o.extensions) spec += (spec.empty() ? L"*" : L";*") + wide(e);
            const COMDLG_FILTERSPEC filter{L"Files", spec.c_str()};
            if (!spec.empty()) {
                dialog->SetFileTypes(1, &filter);
                if (saving) dialog->SetDefaultExtension(wide(o.extensions.front().substr(1)).c_str());
            }
            if (!o.folder.empty()) {
                IShellItem* folder = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(wide(o.folder).c_str(), nullptr, IID_IShellItem,
                                                          reinterpret_cast<void**>(&folder)))) {
                    dialog->SetFolder(folder);
                    folder->Release();
                }
            }
            if (SUCCEEDED(dialog->Show(owner))) {
                if (saving) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(dialog->GetResult(&item))) {
                        chosen.push_back(pathOf(item));
                        item->Release();
                    }
                } else {
                    IShellItemArray* items = nullptr;
                    if (SUCCEEDED(static_cast<IFileOpenDialog*>(dialog)->GetResults(&items))) {
                        DWORD count = 0;
                        items->GetCount(&count);
                        for (DWORD i = 0; i < count; ++i) {
                            IShellItem* item = nullptr;
                            if (SUCCEEDED(items->GetItemAt(i, &item))) {
                                chosen.push_back(pathOf(item));
                                item->Release();
                            }
                        }
                        items->Release();
                    }
                }
            }
            dialog->Release();
        }
        if (com) CoUninitialize();
        {
            std::lock_guard<std::mutex> guard(p->lock);
            p->paths = std::move(chosen);
        }
        p->finished.store(true);
    });
    return true;
}

void NativeFiles::poll() {
    if (!platform_ || !platform_->finished.load()) return;
    platform_->thread.join();
    std::vector<std::string> paths;
    {
        std::lock_guard<std::mutex> guard(platform_->lock);
        paths = std::move(platform_->paths);
    }
    platform_.reset();
    Done done = std::move(done_);
    done_ = nullptr;
    if (done) done(paths);
}

NativeFiles::NativeFiles(uintptr_t parent) : parent_(parent) {}
NativeFiles::~NativeFiles() = default;

#elif !defined(__APPLE__)

// --- Linux and the BSDs: zenity or kdialog, as a child process -----------------------------

namespace {
bool onPath(const char* program) {
    const char* path = std::getenv("PATH");
    if (!path) return false;
    for (std::string_view rest = path; !rest.empty();) {
        const size_t colon = rest.find(':');
        const std::string dir(rest.substr(0, colon));
        rest = colon == std::string_view::npos ? std::string_view{} : rest.substr(colon + 1);
        const std::string candidate = (dir.empty() ? "." : dir) + "/" + program;
        if (access(candidate.c_str(), X_OK) == 0) return true;
    }
    return false;
}
}  // namespace

struct NativeFiles::Platform {
    pid_t pid = -1;
    int out = -1;  // the child's stdout
    std::string text;

    ~Platform() {
        if (out >= 0) close(out);
        if (pid > 0) {
            kill(pid, SIGTERM);
            for (int i = 0; i < 50 && waitpid(pid, nullptr, WNOHANG) == 0; ++i) usleep(2000);
            if (waitpid(pid, nullptr, WNOHANG) == 0) {
                kill(pid, SIGKILL);
                waitpid(pid, nullptr, 0);
            }
        }
    }
};

bool NativeFiles::start(const Options& o, bool saving, Done done) {
    if (platform_) return false;
    // KDE's own on Plasma, else GTK's.
    const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
    const bool kde = desktop && std::strstr(desktop, "KDE");
    const char* program = kde && onPath("kdialog") ? "kdialog" : onPath("zenity") ? "zenity" : onPath("kdialog") ? "kdialog" : nullptr;
    if (!program) return false;
    std::vector<std::string> args = linuxDialogCommand(program, o, saving, parent_);
    std::vector<char*> argv;
    for (std::string& a : args) argv.push_back(a.data());
    argv.push_back(nullptr);

    int pipeFds[2];
    if (pipe(pipeFds) != 0) return false;
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_adddup2(&actions, pipeFds[1], 1);
    posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addclose(&actions, pipeFds[0]);
    posix_spawn_file_actions_addclose(&actions, pipeFds[1]);
    pid_t pid = -1;
    const int failed = posix_spawnp(&pid, program, &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipeFds[1]);
    if (failed) {
        close(pipeFds[0]);
        return false;
    }
    fcntl(pipeFds[0], F_SETFL, fcntl(pipeFds[0], F_GETFL) | O_NONBLOCK);
    fcntl(pipeFds[0], F_SETFD, FD_CLOEXEC);
    platform_ = std::make_unique<Platform>();
    platform_->pid = pid;
    platform_->out = pipeFds[0];
    done_ = std::move(done);
    return true;
}

void NativeFiles::poll() {
    if (!platform_) return;
    char buffer[4096];
    // Whatever the dialog has printed; true once it has closed its end.
    const auto drain = [&] {
        ssize_t n;
        while ((n = read(platform_->out, buffer, sizeof(buffer))) > 0) platform_->text.append(buffer, static_cast<size_t>(n));
        return n == 0;
    };
    const bool closed = drain();
    int status = 0;
    const pid_t reaped = waitpid(platform_->pid, &status, WNOHANG);
    if (reaped == 0) return;  // still open
    if (reaped == platform_->pid) {
        drain();
    } else {
        // The host collected it first: a SIGCHLD handler that reaps every
        // child, or SA_NOCLDWAIT. The exit status is gone, but the dialog
        // prints only what was chosen (nothing when cancelled), so its
        // output, once all of it is here, is the answer.
        if (errno != ECHILD || !closed) return;
        status = 0;
    }
    platform_->pid = -1;
    std::vector<std::string> paths;
    if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {  // 1 is cancelled
        std::string_view rest = platform_->text;
        while (!rest.empty()) {
            const size_t end = rest.find('\n');
            const std::string_view line = rest.substr(0, end);
            rest = end == std::string_view::npos ? std::string_view{} : rest.substr(end + 1);
            if (!line.empty()) paths.emplace_back(line);
        }
    }
    platform_.reset();
    Done done = std::move(done_);
    done_ = nullptr;
    if (done) done(paths);
}

NativeFiles::NativeFiles(uintptr_t parent) : parent_(parent) {}
NativeFiles::~NativeFiles() = default;

#endif

}  // namespace supple::gui
