// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "bridge_protocol.h"
#include "supple/base/main_thread_wake.h"
#include "supple/host/clap_host.h"

namespace supple::host {
struct RemotePorts {
    std::uint32_t inChannels = 0, outChannels = 0, keyChannels = 0;
    std::vector<AudioPortInfo> auxOutputs;
    bool acceptsNotes = false, semantic = false, editor = false;
};
// No remote instance can exist until the platform bridge is implemented.
// Pure virtual calls deliberately provide no substitute audio implementation.
class RemoteInstance {
  public:
    static Result<std::unique_ptr<RemoteInstance>> spawn(const PluginInfo&, const HostIdentity&,
                                                         RemotePorts&,
                                                         std::shared_ptr<BridgeProcess>);
    virtual ~RemoteInstance() = default;
    virtual void setMainThreadWake(MainThreadWake& wake) = 0;
    virtual Status activate(double rate, std::uint32_t minFrames, std::uint32_t maxFrames,
                            std::uint32_t inChannels, std::uint32_t outChannels,
                            std::uint32_t keyChannels, bool keyActive, std::uint32_t auxOutChannels,
                            RemotePorts* settled) = 0;
    virtual void deactivate() = 0;
    virtual std::vector<ParamInfo> params() = 0;
    virtual Result<double> paramValue(std::uint32_t id) = 0;
    virtual Result<std::string> paramText(std::uint32_t id, double value) = 0;
    virtual Result<double> paramFromText(std::uint32_t id, std::string_view text) = 0;
    virtual Status setParam(std::uint32_t id, double value) = 0;
    virtual Result<double> normalizedParam(std::uint32_t id, double plain) = 0;
    virtual Result<double> plainParam(std::uint32_t id, double normalized) = 0;
    virtual void refreshLatency() = 0;
    virtual std::uint32_t latency() const = 0;
    virtual std::uint32_t tail() const = 0;
    virtual bool reportsTail() const = 0;
    virtual Result<std::string> text(bridge::Op op, std::optional<bool> flag = {}) = 0;
    virtual Status setPatch(std::string_view json, std::uint32_t mode) = 0;
    virtual Result<std::vector<std::uint8_t>> saveState() = 0;
    virtual Status loadState(std::span<const std::uint8_t> bytes) = 0;
    virtual bool service(Instance::Notifications& notes, std::vector<std::string>& log) = 0;
    virtual bool needsAudioFlush() const = 0;
    virtual bool restartPending() const = 0;
    virtual bool processRequested() const SUPPLE_RT = 0;
    virtual bool takeProcessRequest() SUPPLE_RT = 0;
    virtual Instance::IdleOutputs takeIdleOutputs() = 0;
    virtual Result<Instance::EditorSize> openEditor(unsigned long x11Parent, double scale) = 0;
    virtual Result<Instance::EditorSize> resizeEditor(std::uint32_t width,
                                                      std::uint32_t height) = 0;
    virtual void closeEditor() = 0;
    virtual Instance::EditorRequests editorRequests() = 0;
    virtual bool startProcessing() SUPPLE_RT = 0;
    virtual void stopProcessing() SUPPLE_RT = 0;
    virtual void reset() SUPPLE_RT = 0;
    virtual void flush() SUPPLE_RT = 0;
    virtual bool process(const ProcessBlock& block, OutputEvents* out) SUPPLE_RT = 0;
    virtual bool failed() const = 0;
    virtual std::string failure() const = 0;
    virtual void setDeadlineMs(int ms) = 0;
    virtual int bridgePid() const = 0;
};
std::filesystem::path bridgeExecutable();
}  // namespace supple::host
