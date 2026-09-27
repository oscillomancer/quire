#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <supple/rpc/model_loop.h>
#include <unistd.h>
#include <cerrno>
#include <deque>
#include <exception>
#include <mutex>

namespace supple::rpc {
namespace {
volatile sig_atomic_t signalFd = -1;
volatile sig_atomic_t stopping = 0;
void onSignal(int) {
    stopping = 1;
    char byte = 1;
    if (signalFd >= 0) (void)::write(signalFd, &byte, 1);
}
}  // namespace
struct ModelLoop::Impl {
    int wake[2] = {-1, -1};
    std::mutex mutex;
    std::deque<std::function<void()>> tasks;
    struct sigaction oldInt{}, oldTerm{};
};
ModelLoop::ModelLoop() : impl_(new Impl) {
    if (::pipe2(impl_->wake, O_NONBLOCK | O_CLOEXEC) != 0) std::terminate();
    struct sigaction action{};
    action.sa_handler = onSignal;
    sigemptyset(&action.sa_mask);
    stopping = 0;
    signalFd = impl_->wake[1];
    ::sigaction(SIGINT, &action, &impl_->oldInt);
    ::sigaction(SIGTERM, &action, &impl_->oldTerm);
}
ModelLoop::~ModelLoop() {
    signalFd = -1;
    ::sigaction(SIGINT, &impl_->oldInt, nullptr);
    ::sigaction(SIGTERM, &impl_->oldTerm, nullptr);
    ::close(impl_->wake[0]);
    ::close(impl_->wake[1]);
    delete impl_;
}
void ModelLoop::post(std::function<void()> task) {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->tasks.push_back(std::move(task));
    }
    char byte = 1;
    (void)::write(impl_->wake[1], &byte, 1);
}
void ModelLoop::run() {
    while (true) {
        if (!stopping) {
            pollfd descriptor{impl_->wake[0], POLLIN, 0};
            int ready = ::poll(&descriptor, 1, -1);
            if (ready < 0 && errno == EINTR) continue;
            if (ready < 0) break;
        }
        char bytes[256];
        while (::read(impl_->wake[0], bytes, sizeof(bytes)) > 0) {
        }
        while (true) {
            std::function<void()> task;
            {
                std::lock_guard lock(impl_->mutex);
                if (impl_->tasks.empty()) break;
                task = std::move(impl_->tasks.front());
                impl_->tasks.pop_front();
            }
            task();
        }
        if (stopping) break;
    }
}
}  // namespace supple::rpc
