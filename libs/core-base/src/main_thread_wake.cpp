// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/base/main_thread_wake.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cerrno>

#include "supple/base/thread.h"

namespace supple {

static_assert(std::atomic<bool>::is_always_lock_free);
static_assert(std::atomic<MainThreadWake*>::is_always_lock_free);

MainThreadWake::MainThreadWake() : MainThreadWake(nullptr, nullptr) {}

MainThreadWake::MainThreadWake(RelayHook hook, void* context)
    : relayHook_(hook), relayContext_(context), fd_(::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {
    if (fd_ >= 0) relay_ = std::thread([this] { relay(); });
}

MainThreadWake::~MainThreadWake() {
    quit_.store(true, std::memory_order_release);
    sequence_.fetch_add(1, std::memory_order_release);
    rt::wake(sequence_);
    observe(RelayStep::Shutdown);
    if (relay_.joinable()) relay_.join();
    if (fd_ >= 0) ::close(fd_);
}

void MainThreadWake::notify() SUPPLE_RT {
    // All request data is published before this RMW. Even coalesced producers
    // participate in the release sequence acquired by drain().
    if (!pending_.exchange(true, std::memory_order_acq_rel)) {
        sequence_.fetch_add(1, std::memory_order_release);
        rt::wake(sequence_);
    }
}

void MainThreadWake::relay() {
    nameThread("main-wake");
    std::uint32_t seen = 0;
    for (;;) {
        rt::wait(sequence_, seen);
        observe(RelayStep::BeforeCapture);
        // Capture before writing: drain + another notify may race with the
        // write, and the new sequence must produce another event, not be lost.
        seen = sequence_.load(std::memory_order_acquire);
        observe(RelayStep::AfterCapture);
        // Capture before checking quit too: otherwise destruction can advance
        // the sequence after the check, and we would consume its final wake
        // as an ordinary notification, then park forever on that same word.
        if (quit_.load(std::memory_order_acquire)) return;
        observe(RelayStep::BeforeWrite);
        const std::uint64_t one = 1;
        while (::write(fd_, &one, sizeof(one)) < 0 && errno == EINTR) {
        }
    }
}

bool MainThreadWake::drain() {
    std::uint64_t count;
    ssize_t read;
    do {
        read = ::read(fd_, &count, sizeof(count));
    } while (read < 0 && errno == EINTR);
    if (read != sizeof(count)) return false;
    // Clear before consuming work. A producer that coalesced before this is
    // acquired here; one arriving afterwards signals a fresh loop pass.
    pending_.exchange(false, std::memory_order_acq_rel);
    return true;
}

}  // namespace supple
