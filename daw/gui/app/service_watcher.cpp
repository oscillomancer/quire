// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "service_watcher.h"
#include <clap/clap.h>
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>
#include <utility>
namespace supple::gui {
// --- FdWatcher

Result<std::unique_ptr<FdWatcher>> FdWatcher::create(std::function<void()> wake, Wait wait) {
    auto watcher = std::unique_ptr<FdWatcher>(new FdWatcher(std::move(wake)));
    if (!watcher->changed_.waitable().valid())
        return Error("gui.editor.fds", "cannot start GUI wait service", {},
                     "release native wait resources and retry");
    watcher->wait_ = std::move(wait);
    try {
        watcher->thread_ =
            JoiningThread([owner = watcher.get()](StopToken stop) { owner->run(stop); });
    } catch (const std::system_error& e) {
        return Error("gui.editor.fds", std::string("cannot start GUI wait worker: ") + e.what());
    }
    return watcher;
}

std::optional<Error> FdWatcher::error() {
    std::lock_guard lock(mutex_);
    return error_;
}

FdWatcher::~FdWatcher() {
    thread_.request_stop();
    notify();
    if (thread_.joinable()) thread_.join();
}

void FdWatcher::notify() { changed_.notify(); }

void FdWatcher::watch(std::vector<FdWatch> fds) {
    auto same = [](const FdWatch& a, const FdWatch& b) {
        return a.fd == b.fd && a.owner == b.owner;
    };
    {
        std::lock_guard lock(mutex_);
        // Readiness of a registration that is gone is stale; ready
        // descriptors of current ones stay out until dispatched.
        std::erase_if(ready_, [&](const FdWatch& r) {
            return std::none_of(fds.begin(), fds.end(),
                                [&](const FdWatch& f) { return same(f, r); });
        });
        std::erase_if(fds, [&](const FdWatch& f) {
            return std::any_of(ready_.begin(), ready_.end(),
                               [&](const FdWatch& r) { return same(f, r); });
        });
        if (fds == fds_) return;
        fds_ = std::move(fds);
        ++generation_;
    }
    notify();
}

std::vector<FdWatch> FdWatcher::takeReady() {
    std::lock_guard lock(mutex_);
    return std::exchange(ready_, {});
}

void FdWatcher::run(StopToken stop) {
    while (!stop.stop_requested()) {
        std::vector<WaitItem> polled;
        std::vector<std::uint64_t> owners;
        std::uint64_t generation = 0;
        {
            std::lock_guard lock(mutex_);
            generation = generation_;
            for (const FdWatch& w : fds_) {
                short events = 0;
                if (w.flags & CLAP_POSIX_FD_READ) events |= kWaitRead;
                if (w.flags & CLAP_POSIX_FD_WRITE) events |= kWaitWrite;
                polled.push_back({w.fd, events, 0});
                owners.push_back(w.owner);
            }
        }
        polled.push_back({changed_.waitable(), kWaitRead, 0});
        if ((wait_ ? wait_(polled) : waitFor(polled, -1)) < 0) {
            if (errno == EINTR) continue;
            {
                std::lock_guard lock(mutex_);
                error_ = Error("gui.editor.fds",
                               std::string("editor FD poll failed: ") + std::strerror(errno), {},
                               "close and reopen the editor after restarting FD service");
            }
            if (wake_) wake_();
            return;
        }
        if (polled.back().ready & kWaitRead) changed_.drain();
        bool any = false;
        {
            std::lock_guard lock(mutex_);
            // The set changed while polling: these results may belong to
            // registrations that are gone. Poll the new set (level-triggered,
            // so what is still ready is found again).
            if (generation != generation_) continue;
            for (std::size_t i = 0; i + 1 < polled.size(); ++i) {
                const short r = polled[i].ready;
                if (!r) continue;
                std::uint32_t flags = 0;
                if (r & kWaitRead) flags |= CLAP_POSIX_FD_READ;
                if (r & kWaitWrite) flags |= CLAP_POSIX_FD_WRITE;
                if (r & (kWaitError | kWaitHangup | kWaitInvalid)) flags |= CLAP_POSIX_FD_ERROR;
                ready_.push_back({polled[i].source, flags, owners[i]});
                std::erase_if(fds_, [&](const FdWatch& f) {
                    return f.fd == polled[i].source && f.owner == owners[i];
                });
                any = true;
            }
        }
        if (any && wake_) wake_();
    }
}

}  // namespace supple::gui
