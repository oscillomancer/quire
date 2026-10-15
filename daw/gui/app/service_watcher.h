// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#if defined(__linux__)
#include <poll.h>  // compatibility injection used by existing Linux editor tests
#endif
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <thread>
#include <vector>
#include "supple/base/result.h"
#include "supple/base/stop.h"
#include "supple/base/waitable.h"
namespace supple::gui {
// Polls app/model and plugin descriptors on one thread and wakes the UI when one
// is ready; a ready descriptor is not polled again until the UI thread has
// dispatched it (so a level-triggered fd cannot spin).
// One descriptor with its registration's serial (`owner`), so old readiness
// never reaches a later one that reuses the descriptor number.
struct FdWatch {
    Waitable fd;
    std::uint32_t flags = 0;  // CLAP_POSIX_FD_*; in takeReady(), what was ready
    std::uint64_t owner = 0;
    bool operator==(const FdWatch&) const = default;
};

class FdWatcher {
  public:
    using Wait = std::function<int(std::span<WaitItem>)>;
    static Result<std::unique_ptr<FdWatcher>> create(std::function<void()> wake, Wait wait = {});
#if defined(__linux__)
    // Compatibility for existing editor tests; production uses typed waits.
    using Pipe = std::function<int(int*)>;
    using Poll = std::function<int(std::span<pollfd>)>;
    static Result<std::unique_ptr<FdWatcher>> create(std::function<void()> wake, Pipe pipe,
                                                     Poll poll = {});
#endif
    ~FdWatcher();
    std::optional<Error> error();
    void watch(std::vector<FdWatch> fds);  // UI thread: the set to poll
    std::vector<FdWatch> takeReady();      // UI thread: what was ready, per registration

  private:
    explicit FdWatcher(std::function<void()> wake) : wake_(std::move(wake)) {}
    void run(StopToken stop);
    void notify();
    Wait wait_;
    std::function<void()> wake_;
    std::mutex mutex_;
    std::vector<FdWatch> fds_, ready_;
    std::uint64_t generation_ = 0;  // bumped when the set changes: older polls are stale
    WakeEvent changed_;             // interrupts the wait on set changes or shutdown
    JoiningThread thread_;
    std::optional<Error> error_;
};

}  // namespace supple::gui
