// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <windows.h>
#include <chrono>
#include "supple/gui/process.h"
namespace supple::gui {
std::tm localTime(std::time_t time) {
    std::tm result{};
    localtime_s(&result, &time);
    return result;
}

namespace {
double milliseconds(FILETIME t) {
    ULARGE_INTEGER ticks{};
    ticks.LowPart = t.dwLowDateTime;
    ticks.HighPart = t.dwHighDateTime;
    return static_cast<double>(ticks.QuadPart) / 10000.0;
}
double cpu(bool thread) {
    FILETIME start{}, end{}, kernel{}, user{};
    const BOOL ok = thread ? GetThreadTimes(GetCurrentThread(), &start, &end, &kernel, &user)
                           : GetProcessTimes(GetCurrentProcess(), &start, &end, &kernel, &user);
    return ok ? milliseconds(kernel) + milliseconds(user) : 0;
}
}  // namespace
unsigned long processId() { return GetCurrentProcessId(); }
double millisecondsSinceProcessStart() {
    FILETIME start{}, end{}, kernel{}, user{}, now{};
    if (!GetProcessTimes(GetCurrentProcess(), &start, &end, &kernel, &user)) return -1;
    GetSystemTimeAsFileTime(&now);
    return milliseconds(now) - milliseconds(start);
}
double processCpuMs() { return cpu(false); }
double threadCpuMs() { return cpu(true); }
double wallMs() {
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
}  // namespace supple::gui
