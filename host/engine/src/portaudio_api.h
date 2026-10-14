// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <portaudio.h>
#include <cstdint>
#include "supple/base/result.h"
namespace supple::engine::detail {
// Setup/teardown belong to the owner; stream info/state may also be read by
// the monitor. Native driver rate queries belong exclusively to the monitor.
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
};
const PortAudioApi* nativePortAudioApi();
Status prepareDeviceThread(double rate, std::uint32_t block, bool live);
void releaseDeviceThread();
}  // namespace supple::engine::detail
