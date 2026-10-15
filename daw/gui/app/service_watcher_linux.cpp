// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Compatibility injection for the pre-port editor tests.
#include <unistd.h>
#include <cerrno>
#include <cstring>
#include "service_watcher.h"
namespace supple::gui {
Result<std::unique_ptr<FdWatcher>> FdWatcher::create(std::function<void()> wake, Pipe pipe,
                                                     Poll poll) {
    if (pipe) {
        int descriptors[2] = {-1, -1};
        const auto result = pipe(descriptors);
        const auto error = errno;
        for (int fd : descriptors)
            if (fd >= 0) close(fd);
        if (result != 0)
            return Error("gui.editor.fds",
                         std::string("cannot start editor FD service: ") + std::strerror(error), {},
                         "release file descriptors and retry editor activation");
    }
    Wait wait;
    if (poll)
        wait = [poll = std::move(poll)](std::span<WaitItem> items) {
            std::vector<pollfd> fds;
            for (const auto& item : items) fds.push_back({item.source.fd(), item.events, 0});
            const auto result = poll(fds);
            for (std::size_t i = 0; i < items.size(); ++i) items[i].ready = fds[i].revents;
            return result;
        };
    return create(std::move(wake), std::move(wait));
}
}  // namespace supple::gui
