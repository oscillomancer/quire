// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/base/thread.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#if defined(__linux__)
#include <sys/resource.h>
#endif
#endif

#include <algorithm>
#include <bit>
#include <cstring>
#include <string>

namespace supple {

void nameThread(std::string_view name) {
    char text[16] = {};  // the kernel's TASK_COMM_LEN, with its terminator
    std::memcpy(text, name.data(), std::min(name.size(), sizeof(text) - 1));
#if defined(_WIN32)
    wchar_t wide[16]{};
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wide, 16);
    SetThreadDescription(GetCurrentThread(), wide);
#elif defined(__APPLE__)
    ::pthread_setname_np(text);
#else
    ::pthread_setname_np(::pthread_self(), text);
#endif
}

std::string_view policyName(ThreadPolicy::Kind kind) {
    switch (kind) {
        case ThreadPolicy::Kind::Normal:
            return "normal";
        case ThreadPolicy::Kind::Fifo:
            return "fifo";
        case ThreadPolicy::Kind::RoundRobin:
            return "rr";
        case ThreadPolicy::Kind::High:
            return "high";
        case ThreadPolicy::Kind::None:
            break;
    }
    return "none";
}

ThreadPolicy threadPolicy(std::thread::native_handle_type thread) {
#if defined(_WIN32)
    const int priority = GetThreadPriority(thread);
    if (priority == THREAD_PRIORITY_ERROR_RETURN) return {};
    return {
        priority > THREAD_PRIORITY_NORMAL ? ThreadPolicy::Kind::High : ThreadPolicy::Kind::Normal,
        0};
#elif defined(__APPLE__)
    qos_class_t qos = QOS_CLASS_UNSPECIFIED;
    int relativePriority = 0;
    if (pthread_get_qos_class_np(thread, &qos, &relativePriority) != 0) return {};
    return {
        qos == QOS_CLASS_USER_INTERACTIVE ? ThreadPolicy::Kind::High : ThreadPolicy::Kind::Normal,
        0};
#else
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
#endif
}

Status setThreadPolicy(std::thread::native_handle_type thread, ThreadPolicy policy) {
#if defined(_WIN32) || defined(__APPLE__)
    if (policy.realtime())
        return Error("thread.policy", "POSIX FIFO/RR scheduling is unavailable on this platform",
                     {}, "use normal scheduling or the platform high-priority policy");
    const bool high = policy.kind == ThreadPolicy::Kind::High;
#if defined(_WIN32)
    const int requested = high ? THREAD_PRIORITY_HIGHEST : THREAD_PRIORITY_NORMAL;
    if (!SetThreadPriority(thread, requested))
        return Error("thread.policy", "SetThreadPriority failed: " + std::to_string(GetLastError()),
                     {}, "check the thread handle and process permissions");
    if (GetThreadPriority(thread) != requested)
        return Error("thread.policy", "the thread did not take the requested Windows priority", {},
                     "check that nothing overrides this thread's scheduling");
#else
    if (high && !pthread_equal(thread, pthread_self()))
        return Error("thread.policy", "QoS must be set by the owning thread on macOS", {},
                     "apply the high policy in the thread's setup callback");
    sched_param params{};
    const int error = pthread_equal(thread, pthread_self())
                          ? pthread_set_qos_class_self_np(
                                high ? QOS_CLASS_USER_INTERACTIVE : QOS_CLASS_DEFAULT, 0)
                          : pthread_setschedparam(thread, SCHED_OTHER, &params);
    if (error)
        return Error("thread.policy",
                     "could not set thread policy: " + std::string(std::strerror(error)), {},
                     "check that the thread still exists and its QoS is not overridden");
#endif
    policy = {high ? ThreadPolicy::Kind::High : ThreadPolicy::Kind::Normal, 0};
    if (threadPolicy(thread) != policy)
        return Error("thread.policy", "the thread did not take the requested priority", {},
                     "check that nothing overrides this thread's scheduling");
    return okStatus();
#else
    if (policy.kind == ThreadPolicy::Kind::High)
        return Error("thread.policy", "the high policy is for macOS/Windows", {},
                     "use FIFO/RR on Linux");
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
#endif
}

unsigned allowedCpuCount() {
#if defined(__linux__)
    cpu_set_t allowed;
    CPU_ZERO(&allowed);
    if (::sched_getaffinity(0, sizeof(allowed), &allowed) == 0)
        return static_cast<unsigned>(CPU_COUNT(&allowed));
    return 0;
#elif defined(_WIN32)
    DWORD_PTR processMask = 0, systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask, &systemMask))
        return static_cast<unsigned>(std::popcount(processMask));
#endif
    return std::max(1u, std::thread::hardware_concurrency());
}
unsigned availableCpus() {
    const unsigned cpus = allowedCpuCount();
    return cpus ? cpus : std::max(1u, std::thread::hardware_concurrency());
}
int realtimePriorityLimit(int requested) {
#if defined(__linux__)
    rlimit limit{};
    if (::getrlimit(RLIMIT_RTPRIO, &limit) == 0 && limit.rlim_cur != RLIM_INFINITY)
        return static_cast<int>(std::min<rlim_t>(static_cast<rlim_t>(requested), limit.rlim_cur));
#endif
    return requested;
}
}  // namespace supple
