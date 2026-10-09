#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <supple/rpc/model_loop.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <climits>
#include <deque>
#include <exception>
#include <mutex>
#include <vector>

namespace supple::rpc {
namespace {
// Signal state: atomics so a handler racing the model thread is well defined
// (both are lock-free; the handler only stores and writes to the pipe).
std::atomic<int> signalFd{-1};
std::atomic<bool> stopping{false};
using Clock = std::chrono::steady_clock;
// `from + by`, saturating at the clock's end instead of overflowing.
Clock::time_point later(Clock::time_point from, Clock::duration by) {
    return by >= Clock::time_point::max() - from ? Clock::time_point::max() : from + by;
}
void onSignal(int) {
    stopping.store(true, std::memory_order_relaxed);
    const int fd = signalFd.load(std::memory_order_relaxed);
    if (fd >= 0) {
        char byte = 1;
        (void)::write(fd, &byte, 1);
    }
}
}  // namespace

int detail::waitMilliseconds(Clock::duration remaining) {
    if (remaining <= Clock::duration::zero()) return 0;
    const auto ms = std::chrono::ceil<std::chrono::milliseconds>(remaining);
    return ms.count() >= INT_MAX ? INT_MAX : static_cast<int>(ms.count());
}

detail::Wake::~Wake() {
    if (readFd >= 0) ::close(readFd);
    if (writeFd >= 0) ::close(writeFd);
}

void detail::Wake::notify() const {
    if (writeFd < 0) return;
    char byte = 1;
    (void)::write(writeFd, &byte, 1);
    if (waker) waker();
}

void Timer::cancel() {
    if (!state_) return;
    state_->cancelled.store(true, std::memory_order_release);
    if (auto wake = state_->wake.lock()) wake->notify();
}

Timer& Timer::operator=(Timer&& other) noexcept {
    if (this != &other) {
        cancel();
        state_ = std::move(other.state_);
    }
    return *this;
}

void FdWatch::cancel() {
    if (!state_) return;
    state_->cancelled.store(true, std::memory_order_release);
    if (auto wake = state_->wake.lock()) wake->notify();
}
FdWatch& FdWatch::operator=(FdWatch&& other) noexcept {
    if (this != &other) {
        cancel();
        state_ = std::move(other.state_);
    }
    return *this;
}

struct ModelLoop::Impl {
    std::shared_ptr<detail::Wake> wake = std::make_shared<detail::Wake>();
    std::mutex mutex;
    std::deque<std::function<void()>> tasks;
    std::vector<std::shared_ptr<detail::TimerState>> timers;
    std::vector<std::shared_ptr<detail::FdState>> watches;
    struct sigaction oldInt{}, oldTerm{};

    // Milliseconds until the nearest timer, or -1 when there is none. Called
    // with the lock held.
    int nextTimeoutMs() const {
        const auto now = Clock::now();
        int best = -1;
        for (const auto& timer : timers) {
            if (timer->cancelled.load(std::memory_order_relaxed)) continue;
            const int ms = timer->next <= now ? 0 : detail::waitMilliseconds(timer->next - now);
            best = best < 0 ? ms : std::min(best, ms);
        }
        return best;
    }

    // Runs the timers that are due now, on this thread. Their callbacks may
    // register or cancel more timers, so nothing is held while they run. Each
    // timer's cancellation is checked again right before its callback runs
    // (the guarantee in model_loop.h).
    void runDueTimers() {
        std::vector<std::shared_ptr<detail::TimerState>> due;
        {
            std::lock_guard lock(mutex);
            const auto now = Clock::now();
            for (auto it = timers.begin(); it != timers.end();) {
                const auto& timer = *it;
                if (timer->cancelled.load(std::memory_order_acquire)) {
                    it = timers.erase(it);
                    continue;
                }
                if (timer->next <= now) {
                    due.push_back(timer);
                    timer->next =
                        timer->once ? Clock::time_point::max() : later(now, timer->interval);
                }
                ++it;
            }
        }
        for (const auto& timer : due)
            if (timer->once) {
                if (!timer->cancelled.exchange(true, std::memory_order_acq_rel)) timer->fn();
            } else if (!timer->cancelled.load(std::memory_order_acquire))
                timer->fn();
    }

    void drainTasks() {
        while (true) {
            std::function<void()> task;
            {
                std::lock_guard lock(mutex);
                if (tasks.empty()) break;
                task = std::move(tasks.front());
                tasks.pop_front();
            }
            task();
        }
    }

    bool pollOnce(bool block) {
        std::vector<std::shared_ptr<detail::FdState>> readyWatches;
        std::vector<pollfd> fds{{wake->readFd, POLLIN, 0}};
        int timeout = 0;
        {
            std::lock_guard lock(mutex);
            std::erase_if(watches, [](const auto& w) {
                return w->cancelled.load(std::memory_order_acquire);
            });
            readyWatches = watches;
            for (const auto& w : readyWatches) fds.push_back({w->fd, w->events, 0});
            if (block && tasks.empty()) timeout = nextTimeoutMs();
        }
        const int result = ::poll(fds.data(), fds.size(), timeout);
        if (result < 0) return errno == EINTR;
        drainWake();
        drainTasks();
        for (std::size_t i = 0; i < readyWatches.size(); ++i) {
            const auto& w = readyWatches[i];
            if (fds[i + 1].revents && !w->cancelled.load(std::memory_order_acquire))
                w->fn(fds[i + 1].revents);
        }
        runDueTimers();
        drainTasks();
        return true;
    }

    void drainWake() const {
        char bytes[256];
        while (::read(wake->readFd, bytes, sizeof(bytes)) > 0) {
        }
    }
};

ModelLoop::ModelLoop() : impl_(new Impl) {
    int fds[2];
    if (::pipe2(fds, O_NONBLOCK | O_CLOEXEC) != 0) std::terminate();
    impl_->wake->readFd = fds[0];
    impl_->wake->writeFd = fds[1];
    struct sigaction action{};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    stopping.store(false, std::memory_order_relaxed);
    signalFd.store(impl_->wake->writeFd, std::memory_order_relaxed);
    ::sigaction(SIGINT, &action, &impl_->oldInt);
    ::sigaction(SIGTERM, &action, &impl_->oldTerm);
}

ModelLoop::~ModelLoop() {
    signalFd.store(-1, std::memory_order_relaxed);
    ::sigaction(SIGINT, &impl_->oldInt, nullptr);
    ::sigaction(SIGTERM, &impl_->oldTerm, nullptr);
    // Timers keep their handles (they see the wake go away and stop signalling);
    // the pipe closes with the last reference to it.
    {
        std::lock_guard lock(impl_->mutex);
        impl_->timers.clear();
    }
    delete impl_;
}

Timer ModelLoop::every(std::chrono::milliseconds interval, std::function<void()> fn) {
    auto state = std::make_shared<detail::TimerState>();
    // Converting to the clock's finer unit must not overflow either.
    const auto longest =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::duration::max());
    state->interval = std::clamp(interval, std::chrono::milliseconds(1), longest);
    state->next = later(Clock::now(), state->interval);
    state->fn = std::move(fn);
    state->wake = impl_->wake;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->timers.push_back(state);
    }
    impl_->wake->notify();  // the wait may be unbounded right now
    return Timer(std::move(state));
}

Timer ModelLoop::at(Clock::time_point deadline, std::function<void()> fn) {
    auto state = std::make_shared<detail::TimerState>();
    state->once = true;
    state->next = deadline;
    state->fn = std::move(fn);
    state->wake = impl_->wake;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->timers.push_back(state);
    }
    impl_->wake->notify();
    return Timer(std::move(state));
}
FdWatch ModelLoop::watch(int fd, short events, std::function<void(short)> fn) {
    auto state = std::make_shared<detail::FdState>();
    state->fd = fd;
    state->events = events;
    state->fn = std::move(fn);
    state->wake = impl_->wake;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->watches.push_back(state);
    }
    impl_->wake->notify();
    return FdWatch(std::move(state));
}

void ModelLoop::post(std::function<void()> task) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->tasks.push_back(std::move(task));
    }
    impl_->wake->notify();
}

void ModelLoop::setWaker(std::function<void()> waker) { impl_->wake->waker = std::move(waker); }

int ModelLoop::pump() {
    impl_->pollOnce(false);
    std::lock_guard lock(impl_->mutex);
    return impl_->nextTimeoutMs();
}

bool ModelLoop::stopRequested() const { return stopping.load(std::memory_order_relaxed); }

void ModelLoop::run() {
    while (!stopRequested())
        if (!impl_->pollOnce(true)) break;
    impl_->drainTasks();
}
}  // namespace supple::rpc
