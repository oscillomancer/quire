// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <time.h>
#include <unistd.h>
#include <chrono>
#include <fstream>
#include <sstream>
#include "supple/gui/process.h"
#if defined(__APPLE__)
#include <libproc.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/time.h>
#endif
namespace supple::gui {
std::tm localTime(std::time_t time) {
    std::tm result{};
    localtime_r(&time, &result);
    return result;
}

unsigned long processId() { return static_cast<unsigned long>(::getpid()); }
// Milliseconds since the kernel started this process (exec and dynamic
// loading included), for the cold-start gate (ADR-0005 §5: < 300 ms).
double millisecondsSinceProcessStart() {
#if defined(__linux__)
    std::ifstream stat("/proc/self/stat");
    std::string line;
    std::getline(stat, line);
    const auto close = line.rfind(')');
    if (close == std::string::npos) return -1;
    std::istringstream fields(line.substr(close + 2));
    std::string field;
    for (int i = 3; i <= 22 && fields >> field; ++i)
        if (i == 22) {
            timespec boot{};
            clock_gettime(CLOCK_BOOTTIME, &boot);
            const double nowS =
                static_cast<double>(boot.tv_sec) + 1e-9 * static_cast<double>(boot.tv_nsec);
            const double startS = std::stod(field) / static_cast<double>(sysconf(_SC_CLK_TCK));
            return 1000.0 * (nowS - startS);
        }
#elif defined(__APPLE__)
    proc_bsdinfo info{};
    timeval now{};
    constexpr int size = static_cast<int>(sizeof(info));
    if (proc_pidinfo(::getpid(), PROC_PIDTBSDINFO, 0, &info, size) != size ||
        gettimeofday(&now, nullptr) != 0)
        return -1;
    const double elapsed =
        1000.0 * (static_cast<double>(now.tv_sec) - static_cast<double>(info.pbi_start_tvsec)) +
        0.001 * (static_cast<double>(now.tv_usec) - static_cast<double>(info.pbi_start_tvusec));
    return elapsed >= 0 ? elapsed : -1;
#endif
    return -1;
}

double processCpuMs() {
    timespec t{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
    return 1000.0 * static_cast<double>(t.tv_sec) + 1e-6 * static_cast<double>(t.tv_nsec);
}

double wallMs() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return 1000.0 * static_cast<double>(t.tv_sec) + 1e-6 * static_cast<double>(t.tv_nsec);
}

double threadCpuMs() {
#if defined(__APPLE__)
    thread_basic_info_data_t info{};
    mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
    const auto thread = mach_thread_self();
    const auto result =
        thread_info(thread, THREAD_BASIC_INFO, reinterpret_cast<thread_info_t>(&info), &count);
    mach_port_deallocate(mach_task_self(), thread);
    if (result != KERN_SUCCESS) return 0;
    return 1000.0 * (info.user_time.seconds + info.system_time.seconds) +
           0.001 * (info.user_time.microseconds + info.system_time.microseconds);
#else
    timespec t{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t);
    return 1000.0 * static_cast<double>(t.tv_sec) + 1e-6 * static_cast<double>(t.tv_nsec);
#endif
}

}  // namespace supple::gui
