// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cerrno>
#include <system_error>
#include "private_config.h"
namespace supple::agent {
namespace {

Error writeError(const std::filesystem::path& file, int error) {
    return Error("agent.config",
                 "cannot write " + file.string() + ": " + std::generic_category().message(error),
                 {}, "check that the configuration directory exists and is writable");
}

// Publishes `text` at `file` without ever exposing a partial or world-readable
// file: a temporary file in the same directory, created exclusively and 0600
// before any secret is written into it, written in full and fsynced, renamed
// over the target, then the directory fsynced. Any failure removes the
// temporary file and leaves the previous configuration untouched.
Status writePrivate(const std::filesystem::path& file, std::string_view text) {
    if (!file.has_filename()) return writeError(file, EINVAL);
    const std::filesystem::path dir = file.has_parent_path() ? file.parent_path() : ".";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string temp = (dir / ("." + file.filename().string() + ".XXXXXX")).string();
    const int fd = ::mkstemp(temp.data());  // O_EXCL, mode 0600
    if (fd < 0) return writeError(file, errno);
    if (fd >= 0) (void)::fcntl(fd, F_SETFD, FD_CLOEXEC);
    auto fail = [&](int error) {
        ::close(fd);
        ::unlink(temp.c_str());
        return writeError(file, error);
    };
    if (::fchmod(fd, 0600) != 0) return fail(errno);  // whatever the C library chose
    for (std::size_t done = 0; done < text.size();) {
        const ssize_t n = ::write(fd, text.data() + done, text.size() - done);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return fail(n < 0 ? errno : EIO);
        done += static_cast<std::size_t>(n);
    }
    if (::fsync(fd) != 0) return fail(errno);
    if (::close(fd) != 0) {
        const int error = errno;
        ::unlink(temp.c_str());
        return writeError(file, error);
    }
    if (::rename(temp.c_str(), file.c_str()) != 0) {
        const int error = errno;
        ::unlink(temp.c_str());
        return writeError(file, error);
    }
    // The rename is durable once the directory is; a failure here leaves the
    // new file in place (readers already see it), so it is not reported.
    if (const int d = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC); d >= 0) {
        ::fsync(d);
        ::close(d);
    }
    return {};
}

}  // namespace

Status writePrivateConfig(const std::filesystem::path& file, std::string_view text) {
    return writePrivate(file, text);
}
}  // namespace supple::agent
