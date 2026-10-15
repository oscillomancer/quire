// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <portaudio.h>
#include <cstdint>
#include <memory>
#include "supple/base/result.h"
namespace supple::engine::detail {
// Setup/teardown belong to the owner; stream info/state may also be read by
// the monitor. Native driver rate queries belong exclusively to the monitor.
// A device's audio workgroup (CoreAudio's IO thread workgroup). Threads that
// do work for the device deadline join it so the scheduler places them with
// the callback. Non-RT; each thread joins and leaves for itself.
class AudioWorkgroup {
  public:
    virtual ~AudioWorkgroup() = default;
    // Joins the calling thread (which must already have its real-time policy).
    // An error's code is "workgroup.join" and its path the OS error number.
    virtual Status join() = 0;
    // Leaves what this thread joined through this object; otherwise a no-op.
    virtual void leave() noexcept = 0;
};
struct PortAudioApi {
    decltype(&Pa_Initialize) initialize;
    decltype(&Pa_Terminate) terminate;
    decltype(&Pa_GetErrorText) errorText;
    decltype(&Pa_GetHostApiCount) hostApiCount;
    decltype(&Pa_GetDefaultHostApi) defaultHostApi;
    decltype(&Pa_GetHostApiInfo) hostApiInfo;
    decltype(&Pa_GetDeviceCount) deviceCount;
    decltype(&Pa_GetDeviceInfo) deviceInfo;
    decltype(&Pa_IsFormatSupported) formatSupported;
    decltype(&Pa_OpenStream) open;
    decltype(&Pa_SetStreamFinishedCallback) finished;
    decltype(&Pa_StartStream) start;
    decltype(&Pa_StopStream) stop;
    decltype(&Pa_CloseStream) close;
    decltype(&Pa_GetStreamInfo) info;
    decltype(&Pa_IsStreamActive) active;
    // Optional native rate observation, only called by the monitor thread.
    double (*deviceRate)(PaStream*, PaDeviceIndex) = nullptr;
    bool native = false;
    bool threaded = true;
    // Optional: the opened stream's audio workgroup (null when none applies).
    Result<std::shared_ptr<AudioWorkgroup>> (*workgroup)(PaStream*) = nullptr;
};
const PortAudioApi* nativePortAudioApi();
#if defined(__APPLE__)
// Wraps an os_workgroup_t (passed as void*). `owned`: release it with the
// wrapper; the CoreAudio device property's reference is deliberately kept.
std::shared_ptr<AudioWorkgroup> wrapOsWorkgroup(void* workgroup, bool owned);
#endif
Status prepareDeviceThread(double rate, std::uint32_t block, bool live);
void releaseDeviceThread();
}  // namespace supple::engine::detail
