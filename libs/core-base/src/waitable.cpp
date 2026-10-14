// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/base/waitable.h"

#include <algorithm>
#include <cerrno>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#endif
#endif

namespace supple {
#if !defined(_WIN32)
static_assert(kWaitRead == POLLIN && kWaitWrite == POLLOUT && kWaitError == POLLERR &&
              kWaitHangup == POLLHUP && kWaitInvalid == POLLNVAL);
#endif
int waitFor(std::span<WaitItem> items, int timeoutMs) {
#if defined(_WIN32)
    std::vector<HANDLE> handles;
    int ready = 0;
    for (std::size_t i = 0; i < items.size(); ++i) {
        auto& item = items[i];
        item.ready = 0;
        if (!item.source.valid()) continue;
        if (!item.source.nativeHandle()) {
            item.ready = kWaitInvalid;
            ++ready;
            continue;
        }
        if (!(item.events & kWaitRead)) continue;
        if (std::find(handles.begin(), handles.end(), item.source.nativeHandle()) == handles.end())
            handles.push_back(item.source.nativeHandle());
    }
    if (handles.size() > MAXIMUM_WAIT_OBJECTS) {
        errno = EINVAL;
        return -1;
    }
    if (handles.empty()) {
        if (!ready) Sleep(timeoutMs < 0 ? INFINITE : static_cast<DWORD>(timeoutMs));
        return ready;
    }
    const DWORD result =
        WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE,
                               ready           ? 0
                               : timeoutMs < 0 ? INFINITE
                                               : static_cast<DWORD>(timeoutMs));
    if (result == WAIT_FAILED) {
        errno = EIO;
        return -1;
    }
    // Enumerate all currently ready sources, preserving poll's ready snapshot.
    for (auto& item : items)
        if (item.source.valid() && item.source.nativeHandle() && (item.events & kWaitRead) &&
            WaitForSingleObject(item.source.nativeHandle(), 0) == WAIT_OBJECT_0) {
            item.ready = kWaitRead;
            ++ready;
        }
    return ready;
#elif defined(__APPLE__)
    // Darwin poll reports duplicate registrations through only one entry.
    // Register each descriptor once, then fan its readiness out to the watches.
    std::vector<pollfd> fds;
    fds.reserve(items.size());
    for (auto& item : items) {
        item.ready = 0;
        const int fd = item.source.fd();
        const auto found = std::find_if(fds.begin(), fds.end(),
                                        [fd](const auto& entry) { return entry.fd == fd; });
        if (found == fds.end())
            fds.push_back({fd, item.events, 0});
        else
            found->events |= item.events;
    }
    const int result = ::poll(fds.data(), fds.size(), timeoutMs);
    if (result < 0) return result;
    int ready = 0;
    for (auto& item : items) {
        const int fd = item.source.fd();
        const auto found = std::find_if(fds.begin(), fds.end(),
                                        [fd](const auto& entry) { return entry.fd == fd; });
        item.ready =
            static_cast<short>(found->revents & (item.events | POLLERR | POLLHUP | POLLNVAL));
        if (item.ready) ++ready;
    }
    return ready;
#else
    std::vector<pollfd> fds;
    fds.reserve(items.size());
    for (auto& item : items) {
        item.ready = 0;
        fds.push_back({item.source.fd(), item.events, 0});
    }
    const int result = ::poll(fds.data(), fds.size(), timeoutMs);
    if (result >= 0)
        for (std::size_t i = 0; i < items.size(); ++i) items[i].ready = fds[i].revents;
    return result;
#endif
}
WakeEvent::WakeEvent() {
#if defined(_WIN32)
    source_ = Waitable::handle(CreateEventW(nullptr, TRUE, FALSE, nullptr));
#elif defined(__linux__)
    writeFd_ = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    source_ = Waitable(writeFd_);
#else
    int fds[2];
    if (::pipe(fds) != 0) return;
    for (int fd : fds) {
        if (::fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || ::fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) {
            ::close(fds[0]);
            ::close(fds[1]);
            return;
        }
    }
    source_ = Waitable(fds[0]);
    writeFd_ = fds[1];
#endif
}
WakeEvent::~WakeEvent() {
#if defined(_WIN32)
    if (source_.valid()) CloseHandle(source_.nativeHandle());
#else
    if (source_.valid()) ::close(source_.fd());
    if (writeFd_ >= 0 && writeFd_ != source_.fd()) ::close(writeFd_);
#endif
}
void WakeEvent::notify() const {
#if defined(_WIN32)
    if (source_.valid()) SetEvent(source_.nativeHandle());
#else
    const std::uint64_t one = 1;
    while (::write(writeFd_, &one, sizeof(one)) < 0 && errno == EINTR) {
    }
#endif
}
bool WakeEvent::drain() {
#if defined(_WIN32)
    if (WaitForSingleObject(source_.nativeHandle(), 0) != WAIT_OBJECT_0) return false;
    return ResetEvent(source_.nativeHandle()) != 0;
#elif defined(__APPLE__)
    bool consumed = false;
    char bytes[256];
    for (;;) {
        const auto result = ::read(source_.fd(), bytes, sizeof(bytes));
        if (result > 0)
            consumed = true;
        else if (result < 0 && errno == EINTR)
            continue;
        else
            return consumed;
    }
#else
    std::uint64_t count;
    decltype(::read(0, nullptr, 0)) result;
    do result = ::read(source_.fd(), &count, sizeof(count));
    while (result < 0 && errno == EINTR);
    return result == sizeof(count);
#endif
}
}  // namespace supple
