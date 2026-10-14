// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include "portaudio_api.h"
#include "supple/base/rt.h"
#include "supple/engine/portaudio.h"

namespace supple::engine {
namespace {
Error unsupported() {
    return Error("backend.unsupported", "native device audio is unavailable on this platform",
                 "/backend", "select jack on Linux, or dummy without a device");
}
Error failure(const detail::PortAudioApi& api, PaError error, const char* path) {
    return Error("backend.open", api.errorText(error), path,
                 "use engine.devices to select a device and host API; check rate and channels");
}
struct Initialization {
    const detail::PortAudioApi* api;
    ~Initialization() {
        if (api) api->terminate();
    }
};
std::uint32_t latency(double seconds, double rate) {
    return static_cast<std::uint32_t>(
        std::clamp(std::ceil(seconds * rate), 0.0, double(UINT32_MAX)));
}
}  // namespace
struct PortAudioBackend::Impl {
    PortAudioOptions options;
    const detail::PortAudioApi* api;
    BackendConfig config;
    Callback callback;
    PaStream* stream = nullptr;
    bool initialized = false;
    MainThreadWake* wake = nullptr;
    std::string inputName;
    std::atomic<bool> stopping{false}, gone{false};
    std::atomic<std::uint32_t> final{0}, period{0};
    std::atomic<std::uint64_t> xruns{0};
    std::uint32_t inputLatency = 0, outputLatency = 0;
    PaDeviceIndex outputDevice = paNoDevice;
    std::thread monitor;
    std::atomic<std::uint32_t> monitorStop{0};
    std::atomic<bool> rateChanged{false};
    void notify() SUPPLE_RT {
        if (wake) wake->notify();
    }
    static int process(const void* input, void* output, unsigned long frames,
                       const PaStreamCallbackTimeInfo*, PaStreamCallbackFlags flags,
                       void* context) SUPPLE_RT {
        auto& self = *static_cast<Impl*>(context);
        auto* out = static_cast<float* const*>(output);
        auto* in = static_cast<const float* const*>(input);
        if (self.stopping.load(std::memory_order_acquire)) {
            if (!self.final.load(std::memory_order_acquire)) {
                self.callback(nullptr, nullptr, 0);
                self.final.store(1, std::memory_order_release);
                rt::wake(self.final);
            }
            if (out)
                for (std::uint32_t c = 0; c < self.config.outputs; ++c)
                    std::fill_n(out[c], frames, 0.0f);
            return paComplete;
        }
        const auto n = static_cast<std::uint32_t>(std::min<unsigned long>(frames, UINT32_MAX));
        bool changed = self.period.exchange(n, std::memory_order_relaxed) != n;
        if (flags & (paInputUnderflow | paInputOverflow | paOutputUnderflow | paOutputOverflow)) {
            self.xruns.fetch_add(1, std::memory_order_relaxed);
            changed = true;
        }
        std::array<const float*, 64> inputs{};
        std::array<float*, 64> outputs{};
        for (std::uint32_t offset = 0; offset < n;) {
            const auto count = std::min(self.config.blockSize, n - offset);
            for (std::uint32_t c = 0; c < self.config.inputs; ++c)
                inputs[c] = in ? in[c] + offset : nullptr;
            for (std::uint32_t c = 0; c < self.config.outputs; ++c) outputs[c] = out[c] + offset;
            self.callback(in ? inputs.data() : nullptr, outputs.data(), count);
            offset += count;
        }
        if (changed) self.notify();
        return paContinue;
    }
    static void finished(void* context) SUPPLE_RT {
        auto& self = *static_cast<Impl*>(context);
        if (!self.stopping.load(std::memory_order_acquire)) {
            self.gone.store(true, std::memory_order_release);
            self.notify();
        }
    }
};
PortAudioBackend::PortAudioBackend(PortAudioOptions options) : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    impl_->api = detail::nativePortAudioApi();
}
PortAudioBackend::PortAudioBackend(PortAudioOptions options, const detail::PortAudioApi& api)
    : impl_(std::make_unique<Impl>()) {
    impl_->options = std::move(options);
    impl_->api = &api;
}
PortAudioBackend::~PortAudioBackend() { stop(); }
void PortAudioBackend::setMainThreadWake(MainThreadWake* wake) { impl_->wake = wake; }
BackendConfig PortAudioBackend::config() const { return impl_->config; }
bool PortAudioBackend::ownsThread() const { return !impl_->api || impl_->api->threaded; }
Status PortAudioBackend::start(const BackendConfig& config, Callback callback) {
    auto& self = *impl_;
    if (self.stream)
        return Error("engine.running", "the device stream is already running", "/backend",
                     "stop the engine before starting another stream");
    if (!self.api) return unsupported();
    if (config.midiInputs)
        return Error("backend.config", "native MIDI inputs are not implemented", "/midi_inputs",
                     "disable MIDI inputs for PortAudio; Linux JACK supports device MIDI");
    if (!std::isfinite(config.sampleRate) || config.sampleRate < 1 || config.sampleRate > 768000 ||
        !config.blockSize || config.blockSize > 65536 || config.inputs > 64 || config.outputs < 2 ||
        config.outputs > 64 || !callback || !std::isfinite(self.options.stopTimeoutSeconds) ||
        self.options.stopTimeoutSeconds <= 0)
        return Error("backend.config", "invalid rate, block, channels or stop timeout", "/backend",
                     "use 1..768000 Hz, a positive block, 0..64 inputs and 2..64 outputs");
    const auto& api = *self.api;
    PaError error = api.initialize();
    if (error != paNoError) return failure(api, error, "/backend");
    self.initialized = true;
    auto fail = [&](Error reason) -> Status {
        if (self.stream) {
            api.close(self.stream);
            self.stream = nullptr;
        }
        api.terminate();
        self.initialized = false;
        self.callback = {};
        return reason;
    };
    int host = api.defaultHostApi();
    if (!self.options.hostApi.empty()) {
        host = -1;
        for (int i = 0; i < api.hostApiCount(); ++i) {
            const auto* info = api.hostApiInfo(i);
            if (info && self.options.hostApi == info->name) host = i;
        }
    }
    const auto* hostInfo = api.hostApiInfo(host);
    if (!hostInfo)
        return fail(Error("backend.device", "host API was not found", "/host_api",
                          "choose an exact host_api from engine.devices"));
    auto select = [&](const std::string& name, bool input) -> Result<int> {
        int found = input ? hostInfo->defaultInputDevice : hostInfo->defaultOutputDevice;
        if (!name.empty()) {
            found = paNoDevice;
            for (int i = 0; i < api.deviceCount(); ++i) {
                const auto* device = api.deviceInfo(i);
                if (!device || device->hostApi != host || name != device->name ||
                    (input ? device->maxInputChannels : device->maxOutputChannels) == 0)
                    continue;
                if (found != paNoDevice)
                    return Error(
                        "backend.device", "device name is ambiguous",
                        input ? "/input_device" : "/output_device",
                        "rename duplicate devices in their driver, or choose another host API");
                found = i;
            }
        }
        if (found == paNoDevice)
            return Error(
                "backend.device", "device was not found",
                input ? "/input_device" : "/output_device",
                "choose an exact name from engine.devices; set inputs to zero for playback only");
        return found;
    };
    PaStreamParameters input{}, output{};
    auto setup = [&](PaStreamParameters& parameters, bool isInput) -> Status {
        auto device =
            select(isInput ? self.options.inputDevice : self.options.outputDevice, isInput);
        if (!device) return std::move(device).error();
        const auto* info = api.deviceInfo(*device);
        const auto channels = isInput ? config.inputs : config.outputs;
        if (!info || channels > static_cast<unsigned>(isInput ? info->maxInputChannels
                                                              : info->maxOutputChannels))
            return Error("backend.config", "device has too few channels",
                         isInput ? "/inputs" : "/outputs",
                         "reduce the channel count to the capacity shown by engine.devices");
        parameters.device = *device;
        parameters.channelCount = static_cast<int>(channels);
        parameters.sampleFormat = paFloat32 | paNonInterleaved;
        parameters.suggestedLatency =
            isInput ? info->defaultLowInputLatency : info->defaultLowOutputLatency;
        if (isInput) self.inputName = info->name;
        return okStatus();
    };
    if (config.inputs) {
        auto s = setup(input, true);
        if (!s) return fail(std::move(s).error());
    }
    auto s = setup(output, false);
    if (!s) return fail(std::move(s).error());
    error = api.formatSupported(config.inputs ? &input : nullptr, &output, config.sampleRate);
    if (error != paFormatIsSupported) return fail(failure(api, error, "/rate"));
    self.outputDevice = output.device;
    self.config = config;
    self.callback = std::move(callback);
    self.stopping.store(false);
    self.gone.store(false);
    self.final.store(0);
    self.period.store(config.blockSize);
    self.xruns.store(0);
    self.rateChanged.store(false);
    self.monitorStop.store(0);
    error = api.open(&self.stream, config.inputs ? &input : nullptr, &output, config.sampleRate,
                     config.blockSize, paClipOff | paDitherOff, Impl::process, &self);
    if (error != paNoError) return fail(failure(api, error, "/backend"));
    const auto* info = api.info(self.stream);
    if (!info || !std::isfinite(info->sampleRate) || info->sampleRate < 1 ||
        info->sampleRate > 768000)
        return fail(Error("backend.config", "device returned an invalid stream rate", "/rate",
                          "select another device or supported sample rate"));
    self.config.sampleRate = info->sampleRate;
    self.inputLatency = latency(info->inputLatency, info->sampleRate);
    self.outputLatency = latency(info->outputLatency, info->sampleRate);
    error = api.finished(self.stream, Impl::finished);
    if (error != paNoError) return fail(failure(api, error, "/backend"));
    error = api.start(self.stream);
    if (error != paNoError) return fail(failure(api, error, "/backend"));
    if (api.native) {
        try {
            self.monitor = std::thread([this] {
                auto& state = *impl_;
                double startedDeviceRate = 0;
                while (!state.monitorStop.load(std::memory_order_acquire)) {
                    const auto observed = status();
                    bool deviceRateChanged = false;
                    if (!observed.restartNeeded && state.api->deviceRate) {
                        const double rate = state.api->deviceRate(state.stream, state.outputDevice);
                        if (rate > 0) {
                            if (startedDeviceRate <= 0) startedDeviceRate = rate;
                            deviceRateChanged = rate != startedDeviceRate;
                        }
                    }
                    if (observed.restartNeeded || deviceRateChanged) {
                        if (deviceRateChanged || observed.restartReason == "sample rate changed")
                            state.rateChanged.store(true, std::memory_order_release);
                        else
                            state.gone.store(true, std::memory_order_release);
                        state.notify();
                        break;
                    }
                    (void)rt::waitUntil(
                        state.monitorStop, 0,
                        std::chrono::steady_clock::now() + std::chrono::milliseconds(250));
                }
            });
        } catch (const std::system_error& monitorError) {
            return fail(Error("backend.open",
                              "device monitor creation failed: " + std::string(monitorError.what()),
                              "/backend", "reduce the process thread count and retry"));
        }
    }
    return okStatus();
}
void PortAudioBackend::stop() {
    auto& self = *impl_;
    if (!self.stream) return;
    self.monitorStop.store(1, std::memory_order_release);
    rt::wake(self.monitorStop);
    if (self.monitor.joinable()) self.monitor.join();
    self.stopping.store(true, std::memory_order_release);
    if (!self.api->threaded) {
        self.callback(nullptr, nullptr, 0);
        self.final.store(1, std::memory_order_release);
    } else if (self.api->active(self.stream) == 1 && !self.final.load(std::memory_order_acquire)) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                  std::chrono::duration<double>(self.options.stopTimeoutSeconds));
        (void)rt::waitUntil(self.final, 0, deadline);
    }
    // Closing joins the callback even after device loss. As with JACK, a dead
    // device cannot supply a final callback: only then lifecycle falls back to main.
    self.api->stop(self.stream);
    self.api->close(self.stream);
    self.stream = nullptr;
    if (!self.final.load(std::memory_order_acquire)) self.callback(nullptr, nullptr, 0);
    self.callback = {};
    if (self.initialized) self.api->terminate();
    self.initialized = false;
}
BackendStatus PortAudioBackend::status() const {
    const auto& self = *impl_;
    bool gone = self.gone.load(std::memory_order_acquire);
    bool rateChanged = self.rateChanged.load(std::memory_order_acquire);
    if (self.stream) {
        gone = gone || self.api->active(self.stream) != 1;
        const auto* info = self.api->info(self.stream);
        rateChanged = rateChanged || (info && info->sampleRate != self.config.sampleRate);
    }
    return {self.period.load(),  self.xruns.load(),
            self.outputLatency,  self.inputLatency,
            gone || rateChanged, gone ? "device lost" : rateChanged ? "sample rate changed" : ""};
}
std::vector<std::string> PortAudioBackend::inputNames() const {
    return std::vector<std::string>(impl_->config.inputs, impl_->inputName);
}
bool PortAudioBackend::nativeThreadPolicy() const { return impl_->api && impl_->api->native; }
Status PortAudioBackend::prepareCurrentThread(bool live) {
    return nativeThreadPolicy() ? detail::prepareDeviceThread(impl_->config.sampleRate,
                                                              impl_->config.blockSize, live)
                                : okStatus();
}
void PortAudioBackend::releaseCurrentThread() {
    if (nativeThreadPolicy()) detail::releaseDeviceThread();
}
Status PortAudioBackend::prepareLiveThread(std::thread::native_handle_type thread) {
    if (nativeThreadPolicy())
        return Error("engine.live_threads", "native device helpers must apply their own policy", {},
                     "use prepareCurrentThread on each helper before publishing live work");
    return setThreadPolicy(thread, {ThreadPolicy::Kind::Normal, 0});
}
ThreadPolicy PortAudioBackend::callbackPolicy() {
    // PortAudio does not expose its callback thread handle/policy. Do not
    // claim a priority we cannot verify; native helper setup is independent.
    return nativeThreadPolicy() ? ThreadPolicy{} : ThreadPolicy{ThreadPolicy::Kind::Normal, 0};
}
Result<std::vector<AudioDevice>> PortAudioBackend::driverDevices() const {
    const auto* api = impl_->api;
    if (!api) return unsupported();
    auto error = api->initialize();
    if (error != paNoError) return failure(*api, error, "/backend");
    Initialization cleanup{api};
    const int count = api->deviceCount();
    if (count < 0) return failure(*api, count, "/backend");
    std::vector<AudioDevice> devices;
    for (int i = 0; i < count; ++i) {
        const auto* info = api->deviceInfo(i);
        if (!info) continue;
        const auto* host = api->hostApiInfo(info->hostApi);
        if (!host) continue;
        devices.push_back({host->name, info->name, static_cast<unsigned>(info->maxInputChannels),
                           static_cast<unsigned>(info->maxOutputChannels), info->defaultSampleRate,
                           host->defaultInputDevice == i, host->defaultOutputDevice == i});
    }
    return devices;
}
Result<std::vector<AudioDevice>> PortAudioBackend::devices() {
    return PortAudioBackend().driverDevices();
}
}  // namespace supple::engine
