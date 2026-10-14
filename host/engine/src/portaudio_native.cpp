// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <mutex>
#include "portaudio_api.h"
#if defined(__APPLE__)
#include <CoreAudio/CoreAudio.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pa_mac_core.h>
#include <pthread.h>
#elif defined(_WIN32)
#include <windows.h>

#include <asiosys.h>

#include <asio.h>
#include <avrt.h>
#endif
namespace supple::engine::detail {
#if defined(__APPLE__) || defined(_WIN32)
namespace {
std::mutex initializationMutex;
PaError initialize() {
    std::lock_guard lock(initializationMutex);
    return Pa_Initialize();
}
PaError terminate() {
    std::lock_guard lock(initializationMutex);
    return Pa_Terminate();
}
#if defined(__APPLE__)
double deviceRate(PaStream* stream, PaDeviceIndex) {
    AudioDeviceID device = PaMacCore_GetStreamOutputDevice(stream);
    AudioObjectPropertyAddress address{kAudioDevicePropertyNominalSampleRate,
                                       kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    Float64 rate = 0;
    UInt32 size = sizeof(rate);
    return AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &rate) == noErr ? rate
                                                                                           : 0;
}
#elif defined(_WIN32)
double deviceRate(PaStream*, PaDeviceIndex device) {
    const auto* info = Pa_GetDeviceInfo(device);
    const auto* host = info ? Pa_GetHostApiInfo(info->hostApi) : nullptr;
    ASIOSampleRate rate = 0;
    return host && host->type == paASIO && ASIOGetSampleRate(&rate) == ASE_OK ? rate : 0;
}
#endif
const PortAudioApi api{initialize,
                       terminate,
                       Pa_GetErrorText,
                       Pa_GetHostApiCount,
                       Pa_GetDefaultHostApi,
                       Pa_GetHostApiInfo,
                       Pa_GetDeviceCount,
                       Pa_GetDeviceInfo,
                       Pa_IsFormatSupported,
                       Pa_OpenStream,
                       Pa_SetStreamFinishedCallback,
                       Pa_StartStream,
                       Pa_StopStream,
                       Pa_CloseStream,
                       Pa_GetStreamInfo,
                       Pa_IsStreamActive,
                       deviceRate,
                       true};
#if defined(_WIN32)
thread_local HANDLE mmcss = nullptr;
#endif
}  // namespace
#endif
const PortAudioApi* nativePortAudioApi() {
#if defined(__APPLE__) || defined(_WIN32)
    return &api;
#else
    return nullptr;
#endif
}
Status prepareDeviceThread(double rate, std::uint32_t block, bool live) {
#if defined(_WIN32)
    (void)rate;
    (void)block;
    DWORD index = 0;
    mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &index);
    if (!mmcss ||
        !AvSetMmThreadPriority(mmcss, live ? AVRT_PRIORITY_CRITICAL : AVRT_PRIORITY_HIGH)) {
        const auto code = GetLastError();
        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
        mmcss = nullptr;
        return Error("engine.audio_policy", "MMCSS Pro Audio setup failed: " + std::to_string(code),
                     {}, "use Audio processing threads Limit 1 or check the Windows audio service");
    }
    return okStatus();
#elif defined(__APPLE__)
    // Apply on the owning thread. The period comes from the opened device,
    // avoiding QoS's inability to elevate another pthread (ADR-0035).
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    const double ticks = double(block) / rate * 1e9 * timebase.denom / timebase.numer;
    const auto period = static_cast<std::uint32_t>(std::clamp(ticks, 1.0, double(UINT32_MAX)));
    thread_time_constraint_policy_data_t policy{};
    policy.period = period;
    policy.computation = std::max<std::uint32_t>(1, period / (live ? 3 : 6));
    policy.constraint = std::max(policy.computation, period / 2);
    policy.preemptible = TRUE;
    const auto thread = pthread_mach_thread_np(pthread_self());
    const auto result = thread_policy_set(thread, THREAD_TIME_CONSTRAINT_POLICY,
                                          reinterpret_cast<thread_policy_t>(&policy),
                                          THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (result != KERN_SUCCESS)
        return Error("engine.audio_policy",
                     "device time-constraint setup failed: " + std::to_string(result), {},
                     "use Audio processing threads Limit 1 or choose another device block size");
    thread_time_constraint_policy_data_t effective{};
    mach_msg_type_number_t count = THREAD_TIME_CONSTRAINT_POLICY_COUNT;
    boolean_t useDefault = FALSE;
    if (thread_policy_get(thread, THREAD_TIME_CONSTRAINT_POLICY,
                          reinterpret_cast<thread_policy_t>(&effective), &count,
                          &useDefault) != KERN_SUCCESS ||
        useDefault || effective.period != policy.period ||
        effective.computation != policy.computation || effective.constraint != policy.constraint) {
        (void)thread_policy_set(thread, THREAD_STANDARD_POLICY, nullptr,
                                THREAD_STANDARD_POLICY_COUNT);
        return Error("engine.audio_policy", "device time-constraint policy could not be verified",
                     {},
                     "use Audio processing threads Limit 1 or choose another device block size");
    }
    return okStatus();
#else
    (void)rate;
    (void)block;
    (void)live;
    return okStatus();
#endif
}
void releaseDeviceThread() {
#if defined(_WIN32)
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    mmcss = nullptr;
#elif defined(__APPLE__)
    (void)thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_STANDARD_POLICY, nullptr,
                            THREAD_STANDARD_POLICY_COUNT);
#endif
}
}  // namespace supple::engine::detail
