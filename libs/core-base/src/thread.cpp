#include "supple/base/thread.h"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

#include <algorithm>
#include <cstring>
#include <string>

namespace supple {

void nameThread(std::string_view name) {
    char text[16] = {};  // the kernel's TASK_COMM_LEN, with its terminator
    std::memcpy(text, name.data(), std::min(name.size(), sizeof(text) - 1));
    ::pthread_setname_np(::pthread_self(), text);
}

std::string_view policyName(ThreadPolicy::Kind kind) {
    switch (kind) {
        case ThreadPolicy::Kind::Normal:
            return "normal";
        case ThreadPolicy::Kind::Fifo:
            return "fifo";
        case ThreadPolicy::Kind::RoundRobin:
            return "rr";
        case ThreadPolicy::Kind::None:
            break;
    }
    return "none";
}

ThreadPolicy threadPolicy(std::thread::native_handle_type thread) {
    int policy = 0;
    sched_param params{};
    if (::pthread_getschedparam(thread, &policy, &params) != 0) return {};
    switch (policy & ~SCHED_RESET_ON_FORK) {
        case SCHED_FIFO:
            return {ThreadPolicy::Kind::Fifo, params.sched_priority};
        case SCHED_RR:
            return {ThreadPolicy::Kind::RoundRobin, params.sched_priority};
        default:
            return {ThreadPolicy::Kind::Normal, 0};
    }
}

Status setThreadPolicy(std::thread::native_handle_type thread, ThreadPolicy policy) {
    const std::string what = policy.realtime() ? std::string(policyName(policy.kind)) + " " +
                                                     std::to_string(policy.priority)
                                               : std::string("normal");
    int native = SCHED_OTHER;
    sched_param params{};
    if (policy.realtime()) {
        rlimit rttime{};
        if (::getrlimit(RLIMIT_RTTIME, &rttime) == 0 && rttime.rlim_cur != RLIM_INFINITY)
            return Error("thread.rttime",
                         "RLIMIT_RTTIME is " + std::to_string(rttime.rlim_cur) +
                             " us: a real-time thread that runs that long without blocking "
                             "would be killed",
                         {},
                         "remove the process's real-time CPU time limit (ulimit -R unlimited), "
                         "for example through the audio or realtime group's limits");
        native = policy.kind == ThreadPolicy::Kind::Fifo ? SCHED_FIFO : SCHED_RR;
        params.sched_priority = policy.priority;
    } else {
        policy = {ThreadPolicy::Kind::Normal, 0};
    }
    if (const int error = ::pthread_setschedparam(thread, native, &params)) {
        rlimit rtprio{};
        const bool limit = ::getrlimit(RLIMIT_RTPRIO, &rtprio) == 0;
        return Error("thread.policy",
                     "could not schedule the thread " + what + ": " + std::strerror(error) +
                         (limit && rtprio.rlim_cur != RLIM_INFINITY
                              ? " (RLIMIT_RTPRIO is " + std::to_string(rtprio.rlim_cur) + ")"
                              : std::string()),
                     {},
                     "grant real-time scheduling up to this priority (RLIMIT_RTPRIO, for example "
                     "through the audio or realtime group's limits)");
    }
    if (threadPolicy(thread) != policy)
        return Error("thread.policy", "the thread did not take scheduling " + what, {},
                     "check that nothing else changes this process's scheduling");
    return okStatus();
}

}  // namespace supple
