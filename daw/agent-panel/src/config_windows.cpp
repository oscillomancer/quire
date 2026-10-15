// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Atomic secret publication with a protected DACL for the current token user.
#include <aclapi.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <system_error>
#include <vector>
#include "private_config.h"
namespace supple::agent {
namespace {
Error failure(const std::filesystem::path& file, DWORD error) {
    return Error(
        "agent.config",
        "cannot write " + file.string() + ": " +
            std::system_category().message(static_cast<int>(error)),
        {}, "choose a writable configuration directory on a filesystem supporting Windows ACLs");
}
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE && value) CloseHandle(value);
    }
};
}  // namespace
Status writePrivateConfig(const std::filesystem::path& file, std::string_view text) {
    if (!file.has_filename()) return failure(file, ERROR_INVALID_NAME);
    std::error_code ec;
    const auto dir = file.has_parent_path() ? file.parent_path() : std::filesystem::path(L".");
    std::filesystem::create_directories(dir, ec);
    if (ec) return failure(file, static_cast<DWORD>(ec.value()));
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
        return failure(file, GetLastError());
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> bytes(size);
    if (!GetTokenInformation(token.value, TokenUser, bytes.data(), size, &size))
        return failure(file, GetLastError());
    EXPLICIT_ACCESSW access{};
    access.grfAccessPermissions = GENERIC_ALL;
    access.grfAccessMode = SET_ACCESS;
    access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
    access.Trustee.TrusteeType = TRUSTEE_IS_USER;
    access.Trustee.ptstrName =
        static_cast<LPWSTR>(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid);
    PACL acl = nullptr;
    const auto aclError = SetEntriesInAclW(1, &access, nullptr, &acl);
    if (aclError != ERROR_SUCCESS) return failure(file, aclError);
    struct FreeAcl {
        PACL value;
        ~FreeAcl() { LocalFree(value); }
    } freeAcl{acl};
    SECURITY_DESCRIPTOR descriptor{};
    if (!InitializeSecurityDescriptor(&descriptor, SECURITY_DESCRIPTOR_REVISION) ||
        !SetSecurityDescriptorDacl(&descriptor, TRUE, acl, FALSE) ||
        !SetSecurityDescriptorControl(&descriptor, SE_DACL_PROTECTED, SE_DACL_PROTECTED))
        return failure(file, GetLastError());
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), &descriptor, FALSE};
    static std::atomic<unsigned long> serial{0};
    std::filesystem::path temp;
    Handle output;
    for (int attempt = 0; attempt < 128; ++attempt) {
        temp = dir /
               (L"." + file.filename().wstring() + L"." + std::to_wstring(GetCurrentProcessId()) +
                L"." + std::to_wstring(serial.fetch_add(1)));
        output.value = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, &attributes, CREATE_NEW,
                                   FILE_ATTRIBUTE_NORMAL, nullptr);
        if (output.value != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) return failure(file, GetLastError());
    }
    if (output.value == INVALID_HANDLE_VALUE) return failure(file, ERROR_FILE_EXISTS);
    auto fail = [&](DWORD error) -> Status {
        CloseHandle(output.value);
        output.value = INVALID_HANDLE_VALUE;
        DeleteFileW(temp.c_str());
        return failure(file, error);
    };
    for (std::size_t done = 0; done < text.size();) {
        DWORD written = 0;
        const auto count = static_cast<DWORD>(std::min<std::size_t>(text.size() - done, MAXDWORD));
        if (!WriteFile(output.value, text.data() + done, count, &written, nullptr))
            return fail(GetLastError());
        if (!written) return fail(ERROR_WRITE_FAULT);
        done += written;
    }
    if (!FlushFileBuffers(output.value)) return fail(GetLastError());
    const auto handle = output.value;
    output.value = INVALID_HANDLE_VALUE;
    if (!CloseHandle(handle)) {
        const auto error = GetLastError();
        DeleteFileW(temp.c_str());
        return failure(file, error);
    }
    if (!MoveFileExW(temp.c_str(), file.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        DeleteFileW(temp.c_str());
        return failure(file, error);
    }
    return {};
}
}  // namespace supple::agent
