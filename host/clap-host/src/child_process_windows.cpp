// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <windows.h>
#include <algorithm>
#include <array>
#include <vector>
#include "platform.h"
#include "supple/base/waitable.h"
#include "supple/host/child_process.h"
namespace supple::host {
namespace {
struct Handle {
    HANDLE value = nullptr;
    ~Handle() {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
};
Error failure(const char* code, const char* message) {
    return Error(code, std::string(message) + ": Windows error " + std::to_string(GetLastError()),
                 "/path", "check the child executable and its permissions");
}
// CommandLineToArgvW/CRT quoting, including trailing slashes and empty args.
std::wstring quote(const std::wstring& text) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (wchar_t c : text) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        if (c == L'"') result += L'\\';
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    return result + L'"';
}
}  // namespace
struct ChildProcess::Impl {
    Handle process, job, output;
};
ChildProcess::ChildProcess() : impl_(std::make_unique<Impl>()) {}
void ChildProcess::killTree() {
    if (impl_->job.value) TerminateJobObject(impl_->job.value, 1);
}
ChildProcess::~ChildProcess() {
    killTree();
    if (impl_->process.value) WaitForSingleObject(impl_->process.value, INFINITE);
}
Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const std::filesystem::path& executable,
                                                          std::span<const std::string> arguments) {
    return spawn(executable, arguments, LaunchOptions{});
}
int ChildProcess::pid() const { return static_cast<int>(GetProcessId(impl_->process.value)); }
std::intptr_t ChildProcess::nativeProcess() const {
    return reinterpret_cast<std::intptr_t>(impl_->process.value);
}
Result<std::unique_ptr<ChildProcess>> ChildProcess::spawn(const std::filesystem::path& executable,
                                                          std::span<const std::string> arguments,
                                                          LaunchOptions options) {
    std::unique_ptr<ChildProcess> child(new ChildProcess);
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle writer, input, error;
    if (!CreatePipe(&child->impl_->output.value, &writer.value, &security, 0) ||
        !SetHandleInformation(child->impl_->output.value, HANDLE_FLAG_INHERIT, 0))
        return failure("child.spawn", "cannot create stdout pipe");
    input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                              OPEN_EXISTING, 0, nullptr);
    error.value = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                              OPEN_EXISTING, 0, nullptr);
    if (input.value == INVALID_HANDLE_VALUE || error.value == INVALID_HANDLE_VALUE)
        return failure("child.spawn", "cannot open null device");
    child->impl_->job.value = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags =
        JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_DIE_ON_UNHANDLED_EXCEPTION;
    if (!child->impl_->job.value ||
        !SetInformationJobObject(child->impl_->job.value, JobObjectExtendedLimitInformation,
                                 &limits, sizeof(limits)))
        return failure("child.spawn", "cannot configure child job");
    SIZE_T size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<std::byte> storage(size);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(attributes, 1, 0, &size))
        return failure("child.spawn", "cannot initialize child attributes");
    struct AttributeCleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST value;
        ~AttributeCleanup() { DeleteProcThreadAttributeList(value); }
    } cleanup{attributes};
    struct InheritControl {
        HANDLE handle = nullptr;
        DWORD flags = 0;
        ~InheritControl() {
            if (handle)
                SetHandleInformation(handle, HANDLE_FLAG_INHERIT, flags & HANDLE_FLAG_INHERIT);
        }
    } control;
    std::vector<HANDLE> inherited{input.value, options.captureOutput ? writer.value : error.value};
    if (options.captureOutput) inherited.push_back(error.value);
    if (options.control >= 0) {
        control.handle = reinterpret_cast<HANDLE>(options.control);
        if (!GetHandleInformation(control.handle, &control.flags) ||
            !SetHandleInformation(control.handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT))
            return failure("child.spawn", "cannot prepare bridge control inheritance");
        inherited.push_back(control.handle);
    }
    if (!UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited.data(), inherited.size() * sizeof(HANDLE), nullptr,
                                   nullptr))
        return failure("child.spawn", "cannot restrict inherited handles");
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input.value;
    startup.StartupInfo.hStdOutput = options.captureOutput ? writer.value : error.value;
    startup.StartupInfo.hStdError = error.value;
    startup.lpAttributeList = attributes;
    std::wstring command = quote(executable.wstring());
    for (const auto& arg : arguments)
        command += L" " + quote(platform::pathFromText(arg).wstring());
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr,
                        nullptr, &startup.StartupInfo, &process))
        return failure("child.spawn", "cannot start child");
    child->impl_->process.value = process.hProcess;
    Handle thread;
    thread.value = process.hThread;
    // Assign before user code can spawn descendants, or terminate the suspended child.
    if (!AssignProcessToJobObject(child->impl_->job.value, process.hProcess)) {
        auto problem = failure("child.spawn", "cannot assign child job");
        TerminateProcess(process.hProcess, 1);
        return problem;
    }
    if (ResumeThread(process.hThread) == static_cast<DWORD>(-1))
        return failure("child.spawn", "cannot resume child");
    return child;
}
Result<ChildProcess::Output> ChildProcess::collect(std::chrono::steady_clock::time_point deadline,
                                                   std::size_t maxBytes) {
    Output output;
    std::array<char, 8192> bytes;
    bool eof = false, exited = false;
    for (;;) {
        const auto remaining = std::chrono::ceil<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now())
                                   .count();
        if (remaining <= 0)
            return Error("child.timeout", "child exceeded its deadline", "/path",
                         "repair the child module before retrying");
        if (!eof) {
            DWORD available = 0;
            if (!PeekNamedPipe(impl_->output.value, nullptr, 0, nullptr, &available, nullptr)) {
                if (GetLastError() == ERROR_BROKEN_PIPE)
                    eof = true;
                else
                    return failure("child.io", "cannot inspect child output");
            } else if (available > 0) {
                DWORD count = 0;
                if (!ReadFile(impl_->output.value, bytes.data(),
                              std::min<DWORD>(available, static_cast<DWORD>(bytes.size())), &count,
                              nullptr))
                    return failure("child.io", "cannot read child output");
                if (count > maxBytes - output.text.size())
                    return Error("child.output_limit", "child output exceeds its byte limit",
                                 "/path", "update the child module or increase its output limit");
                output.text.append(bytes.data(), count);
                continue;
            }
        }
        if (WaitForSingleObject(impl_->process.value, 0) == WAIT_OBJECT_0) {
            exited = true;
            DWORD code = 0;
            if (!GetExitCodeProcess(impl_->process.value, &code))
                return failure("child.wait", "cannot inspect child exit");
            output.exitCode = code;
            output.crashed = code >= 0x80000000u;
            if (output.crashed || eof) return output;
        }
        WaitItem item{Waitable::handle(impl_->process.value)};
        if (waitFor(exited ? std::span<WaitItem>{} : std::span<WaitItem>(&item, 1),
                    static_cast<int>(std::min<std::int64_t>(remaining, 10))) < 0)
            return failure("child.wait", "cannot wait for child");
    }
}
}  // namespace supple::host
