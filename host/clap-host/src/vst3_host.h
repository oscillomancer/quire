#pragma once

#include "supple/host/clap_host.h"

namespace supple::host {

// Local VST3 backing. No SDK types cross the host library's public ABI.
class Vst3Instance {
  public:
    static Result<std::unique_ptr<Vst3Instance>> create(const PluginInfo& info,
                                                        const HostIdentity& host);
    ~Vst3Instance();
    std::uint32_t inputChannels() const;
    std::uint32_t outputChannels() const;
    bool acceptsNotes() const;
    std::uint32_t sidechainChannels() const;  // the first auxiliary input bus (ADR-0018)
    Status activate(double rate, std::uint32_t maxFrames, bool sidechainActive);
    void deactivate();
    std::uint32_t latency() const;
    std::uint32_t tail() const;
    std::vector<ParamInfo> params() const;
    Result<double> paramValue(std::uint32_t id) const;
    Result<std::string> paramText(std::uint32_t id, double plain) const;
    Result<double> paramFromText(std::uint32_t id, std::string_view text) const;
    Status setParam(std::uint32_t id, double plain);
    // Main-thread domain conversions (plain <-> normalized), for callers that
    // hold values in the other domain than the events need.
    Result<double> normalizedParam(std::uint32_t id, double plain) const;
    Result<double> plainParam(std::uint32_t id, double normalized) const;
    Result<std::vector<std::uint8_t>> saveState();
    Status loadState(std::span<const std::uint8_t> bytes);
    bool startProcessing() SUPPLE_RT;
    void stopProcessing() SUPPLE_RT;
    bool process(const ProcessBlock& block, OutputEvents* out) SUPPLE_RT;
    void flushParams() SUPPLE_RT;
    void reset() SUPPLE_RT;
    bool serviceMainThread();  // also runs the editor's due run-loop timers
    // Editor (main thread): see Instance's editor API.
    bool hasEditor();
    Result<Instance::EditorSize> openEditor(unsigned long x11Parent, double scale);
    Result<Instance::EditorSize> resizeEditor(std::uint32_t width, std::uint32_t height);
    void closeEditor();
    bool editorOpen() const;
    Instance::EditorRequests takeEditorRequests();
    std::vector<std::pair<int, std::uint32_t>> watchedFds() const;
    void onFd(int fd);
    int nextTimerMs() const;
    bool takeEdited();  // the editor changed a parameter since the last call
    Instance::Notifications takeNotifications();

  private:
    Vst3Instance();
    struct State;
    std::unique_ptr<State> state_;
};

Result<std::vector<PluginInfo>> scanVst3(std::span<const std::filesystem::path> paths);
std::vector<std::filesystem::path> defaultVst3SearchPath();

}  // namespace supple::host
