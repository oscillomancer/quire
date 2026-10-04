// CLAP and VST3 hosting (docs/specs/sandbox.md §1, ADR-0017). Shared by the
// sandbox and engine.
//
// Threading follows CLAP: an Instance is created, configured, activated,
// saved/loaded and destroyed on the thread that created it (its main
// thread); process() and start/stopProcessing() run on one audio thread.
// The host callbacks a plugin makes are answered according to that split;
// audio-thread callbacks never allocate or lock.
#pragma once

#include <clap/clap.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "supple/base/result.h"
#include "supple/base/rt.h"
#include "supple/base/spsc.h"
#include "supple/clap/semantic_params.h"
#include "supple/json/value.h"

namespace supple::host {

struct PluginInfo {
    std::string id, name, vendor, version, description;
    // CLAP by default; VST3 class ids use vst3:<32 hex digits>.
    std::string format = "clap";
    std::filesystem::path path;
    std::vector<std::string> features;
    bool instrument = false;  // declares the "instrument" feature
    bool semantic = false;    // implements com.suppleaudio.semantic-params/1
};

// Scans files or directories (recursively) for .clap binaries and .vst3 bundles.
Result<std::vector<PluginInfo>> scan(std::span<const std::filesystem::path> dirsOrFiles);
// CLAP and VST3 standard search paths, plus CLAP_PATH/VST3_PATH (existing only).
std::vector<std::filesystem::path> defaultSearchPath();
// The stock plugins built beside the running binary: build/<preset>/plugins/clap,
// two folders above daw/gui/ and host/engine/, when it exists. The apps search it
// before defaultSearchPath(), so an older stock copy in ~/.clap never wins.
std::optional<std::filesystem::path> stockPluginDir();
// Orders a scan result as a catalogue: by id, and within one id in search-path
// order, so a lookup by id takes the first location that has it (an explicit
// --plugin-path before ~/.clap). `shadowed` marks the later copies.
void orderCatalogue(std::vector<PluginInfo>& list);
bool shadowed(std::span<const PluginInfo> catalogue, std::size_t index);

struct HostIdentity {
    std::string name = "supple-host";  // callers pass the product name
    std::string vendor = "Supple Audio";
    std::string url;
    std::string version = "0.1.0";
};

struct ParamInfo {
    std::uint32_t id;
    std::string name, module;
    double min, max, def;
    std::uint32_t flags;  // CLAP_PARAM_IS_*; VST3 flags map to the same bits
};

// --- events ----------------------------------------------------------------------

struct Event {
    enum class Kind : std::uint8_t { NoteOn, NoteOff, NoteChoke, Param, Midi };
    Kind kind = Kind::NoteOn;
    std::uint32_t time = 0;  // sample offset within the block
    // notes
    std::int16_t port = 0, channel = 0, key = 60;
    std::int32_t noteId = -1;
    double velocity = 1.0;  // 0..1
    // params
    std::uint32_t paramId = 0;
    double value = 0.0;  // CLAP value or VST3 plain value
    // MIDI 1.0
    std::uint8_t midi[3] = {0, 0, 0};
};

struct Transport {
    bool playing = true;
    double tempo = 120.0;  // BPM
    double beat = 0.0;     // song position in quarter-note beats at block start
    double barStartBeat = 0.0;
    std::int32_t bar = 0;  // 0-based bar index
    std::uint16_t numerator = 4, denominator = 4;
    double seconds = 0.0;     // song position in seconds at block start
    bool loopActive = false;  // the transport loop (CLAP_TRANSPORT_IS_LOOP_ACTIVE)
    double loopStartBeat = 0.0, loopEndBeat = 0.0, loopStartSeconds = 0.0, loopEndSeconds = 0.0;
};

struct ProcessBlock {
    const float* const* inputs = nullptr;  // main input channels (may be null for instruments)
    std::uint32_t inputChannels = 0;
    float* const* outputs = nullptr;  // main output channels
    std::uint32_t outputChannels = 0;
    // The key (ADR-0018): the plug-in's sidechain input; null = silence.
    const float* const* sidechain = nullptr;
    std::uint32_t sidechainChannels = 0;
    std::uint32_t frames = 0;
    std::span<const Event> events;         // sorted by time
    const Transport* transport = nullptr;  // null: no transport info
    std::int64_t steadyTime = -1;          // sample counter, or -1
};

// Events the plugin emits in process() or params.flush(). Flags retain CLAP's
// event flags; DontRecord forbids automation recording and IsLive identifies
// a live user event. NoteEnd uses the input-note (port, channel, key, noteId)
// address, including -1 wildcards. ParamValue retains its target address too.
// VST3 parameter outputs are normalized, have zero flags and no note address.
struct OutputEvent {
    enum class Kind : std::uint8_t { ParamValue, GestureBegin, GestureEnd, NoteEnd };
    static constexpr std::uint32_t IsLive = CLAP_EVENT_IS_LIVE, DontRecord = CLAP_EVENT_DONT_RECORD;
    Kind kind = Kind::ParamValue;
    std::uint32_t time = 0;
    std::uint32_t paramId = CLAP_INVALID_ID;
    double value = 0;
    std::int32_t noteId = -1;
    std::uint32_t flags = 0;
    std::int16_t port = -1, channel = -1, key = -1;
};

// Caller-owned, preallocated sink for output events (the audio thread never
// allocates): events beyond storage.size() are dropped and counted.
struct OutputEvents {
    std::span<OutputEvent> storage;
    std::size_t count = 0;
    std::size_t dropped = 0;
    void clear() { count = dropped = 0; }
};

class Library;         // a loaded .clap binary (shared by its instances)
class RemoteInstance;  // an instance in a bridge process
class BridgeProcess;   // owns a bridge process and its control socket
class Vst3Instance;    // local VST3 backing

// The bridge executable for isolated instances (process-wide, set at startup).
void setBridgeExecutable(std::filesystem::path path);
// Finds the bridge named `name` (<slug>-bridge) next to the running
// executable (installed) or in the build tree's host/clap-host directory, and
// sets it. False when there is none.
bool locateBridgeExecutable(std::string_view name);
Result<std::shared_ptr<BridgeProcess>> createBridgeProcess();
bool bridgeProcessFailed(const std::shared_ptr<BridgeProcess>& process);

class Instance {
  public:
    // `isolated`: the plugin runs in a bridge process. Pass `bridge` to share
    // one process with other isolated instances; otherwise one is spawned.
    static Result<std::unique_ptr<Instance>> create(const PluginInfo& info,
                                                    const HostIdentity& host, bool isolated = false,
                                                    std::shared_ptr<BridgeProcess> bridge = {});
    ~Instance();
    Instance(const Instance&) = delete;
    Instance& operator=(const Instance&) = delete;

    const PluginInfo& info() const { return info_; }

    // --- lifecycle (main thread) ---
    // Deactivation (and destruction) require processing to have stopped on the
    // audio thread first; deactivate() while processing is ignored and logged.
    Status activate(double sampleRate, std::uint32_t minFrames, std::uint32_t maxFrames);
    void deactivate();
    bool active() const { return active_; }
    double sampleRate() const { return sampleRate_; }  // of the current/last activation
    std::uint32_t maxFrames() const { return maxFrames_; }
    std::uint32_t latency() const;
    std::uint32_t tail() const;  // UINT32_MAX = infinite; 0 when not reported
    bool reportsTail() const;
    // Isolated instances: the bridge crashed, hung or exited. The instance
    // then outputs silence and every call fails; re-create it to recover.
    bool failed() const;
    std::string failure() const;
    bool isolated() const { return remote_ != nullptr; }
    int bridgePid() const;  // -1 for in-process

    // --- ports (valid after create; a stereo configuration is selected when offered) ---
    std::uint32_t audioInputChannels() const { return inChannels_; }
    std::uint32_t audioOutputChannels() const { return outChannels_; }
    bool acceptsNotes() const { return acceptsNotes_; }
    // The sidechain (key) input: a CLAP plug-in's first non-main input port, a
    // VST3's first auxiliary input bus; 0 when it has none (ADR-0018).
    std::uint32_t sidechainChannels() const { return keyChannels_; }
    // Whether a key is connected. VST3 plug-ins see it as their auxiliary bus
    // being active (some then key from it); set before activate().
    Status setSidechainActive(bool on);
    bool sidechainActive() const { return keyActive_; }

    // --- standard parameters (main thread) ---
    std::vector<ParamInfo> params() const;
    Result<double> paramValue(std::uint32_t id) const;
    Result<std::string> paramText(std::uint32_t id, double hostValue) const;
    Result<double> paramFromText(std::uint32_t id, std::string_view text) const;
    // Queues a value change for the next processed block; when not processing,
    // applies it at once through params.flush.
    Status setParam(std::uint32_t id, double hostValue);

    // A VST3 has two parameter domains: the plain value it declares (min..max,
    // what ParamInfo and every command carries) and the normalized 0..1 value
    // its processor is given with its events. paramsNormalized() says which
    // domain `Event::Param` values must be in for this instance; CLAP has no
    // such split (the host value is the event value). The two conversions are
    // main-thread calls — an edit controller is not the processor, and the host
    // never calls it from the audio thread.
    bool paramsNormalized() const;
    Result<double> normalizedParam(std::uint32_t id, double plain) const;
    Result<double> plainParam(std::uint32_t id, double normalized) const;

    // --- semantic view (only when info().semantic) ---
    Result<json::Value> semanticSchema();
    Result<std::string> getPatch(bool changedOnly);
    Status setPatch(std::string_view json, std::uint32_t mode);  // CLAP_SUPPLE_SET_*
    Result<std::string> describe(bool changedOnly);

    // --- state (main thread) ---
    // Inactive saves flush acknowledged main-thread edits into component state.
    // Active saves require those edits to have been delivered by the audio consumer.
    Result<std::vector<std::uint8_t>> saveState();
    // Restoring state invalidates edits queued before this call.
    Status loadState(std::span<const std::uint8_t> bytes);

    // --- processing (audio thread) ---
    // start_processing; false if inactive or the plugin refused (then
    // process() must not be called).
    bool startProcessing() SUPPLE_RT;
    void stopProcessing() SUPPLE_RT;
    bool processing() const { return processing_; }
    // The host's executor that currently processes this instance (an opaque
    // token, null for none): a host with several audio threads uses it so
    // that exactly one of them calls the audio-thread API at a time.
    void setOwner(const void* owner) SUPPLE_RT { owner_.store(owner, std::memory_order_release); }
    const void* owner() const SUPPLE_RT { return owner_.load(std::memory_order_acquire); }
    // Returns false on CLAP_PROCESS_ERROR, or when not processing. Output
    // events are appended to `out` (may be null). Queued setParam() changes
    // are delivered first, at time 0, as far as the event arena has room
    // after the block's own events (the rest wait for the next block); block
    // events beyond the arena are dropped and counted (Notifications).
    bool process(const ProcessBlock& block, OutputEvents* out = nullptr) SUPPLE_RT;
    // Delivers queued setParam() changes through params.flush while active but
    // not processing (CLAP requires the audio thread for that case).
    // Also serves a plugin's request_flush while active (see needsAudioFlush()).
    void flushParams() SUPPLE_RT;
    bool needsAudioFlush() const;  // active, a flush was requested, nothing processed since
    void reset() SUPPLE_RT;        // clap_plugin.reset: [audio-thread & active]

    // --- main-thread servicing (call regularly: after renders, from the app loop) ---
    // Handles request_callback, request_restart (while active), pending flushes
    // and due timers. Returns true if anything was done.
    bool serviceMainThread();
    // Flags the plugin raised since the last call (cleared on read).
    struct Notifications {
        bool stateDirty = false, paramsRescan = false, latencyChanged = false,
             semanticChanged = false, tailChanged = false;
        std::optional<Error> restartError;  // failed automatic reactivation, cleared on read
        std::uint32_t paramsRescanFlags = 0;
        std::uint32_t droppedEvents = 0;  // input events that did not fit a block's arena
    };
    Notifications takeNotifications();
    struct IdleOutputs {
        static constexpr std::size_t kCapacity = 1024;
        std::vector<OutputEvent> events;
        std::uint64_t dropped = 0;
    };
    // Main thread: drains CLAP flush outputs, including active-but-stopped
    // audio-thread flushes. FIFO, at most kCapacity events; overflow drops the
    // newest event and is counted since the previous drain. Flush times are 0.
    // Bridged instances drain over the control channel; bridge failure is
    // observable through failed(). VST3 has no CLAP flush outputs.
    IdleOutputs takeIdleOutputs();

    // --- editor (main thread): the plugin's GUI embedded in an X11 window
    // (CLAP gui extension, VST3 IPlugView). An isolated instance's editor is
    // embedded by its bridge process into the same window (X11 window ids are
    // the server's); its requests are polled through the bridge (gap log #39).
    struct EditorSize {
        std::uint32_t width = 0, height = 0;
        bool resizable = false;
    };
    bool hasEditor() const;
    Result<EditorSize> openEditor(unsigned long x11Parent, double scale);
    // Host-driven resize; returns the size the plugin accepted.
    Result<EditorSize> resizeEditor(std::uint32_t width, std::uint32_t height);
    void closeEditor();
    bool editorOpen() const { return editorOpen_; }
    // What the plugin asked for since the last call; the window owner applies show/hide.
    struct EditorRequests {
        bool resize = false, closed = false, show = false, hide = false;
        std::uint32_t width = 0, height = 0;
    };
    EditorRequests takeEditorRequests();
    // clap.posix-fd-support: descriptors to watch (fd, CLAP_POSIX_FD_* flags),
    // and the callback when one is ready.
    std::vector<std::pair<int, std::uint32_t>> watchedFds() const;
    void onFd(int fd, std::uint32_t flags);
    // Milliseconds until the next plugin timer is due (-1: none): the UI
    // loop's deadline while editors are open.
    int nextTimerMs() const;

    // Log lines the plugin wrote (from any thread), drained on the main thread.
    std::vector<std::string> takeLog();

    const clap_plugin_t* raw() const { return plugin_; }

    struct HostState;  // opaque host-side state (callbacks, queues, event arena)

  private:
    Instance() = default;
    friend struct HostCallbacks;

    PluginInfo info_;
    std::shared_ptr<Library> library_;
    std::unique_ptr<HostState> hostState_;
    const clap_plugin_t* plugin_ = nullptr;
    const clap_plugin_params_t* extParams_ = nullptr;
    const clap_plugin_state_t* extState_ = nullptr;
    const clap_plugin_latency_t* extLatency_ = nullptr;
    const clap_plugin_tail_t* extTail_ = nullptr;
    const clap_plugin_timer_support_t* extTimer_ = nullptr;
    const clap_plugin_supple_semantic_params_t* extSemantic_ = nullptr;
    const clap_plugin_gui_t* extGui_ = nullptr;
    const clap_plugin_posix_fd_support_t* extFd_ = nullptr;
    bool editorOpen_ = false;
    std::uint32_t inChannels_ = 0, outChannels_ = 0, keyChannels_ = 0;
    bool acceptsNotes_ = false, keyActive_ = false;
    bool active_ = false;
    std::atomic<bool> processing_{false};  // written on the audio thread
    std::uint32_t maxFrames_ = 0;
    double sampleRate_ = 0.0;
    std::unique_ptr<RemoteInstance> remote_;
    std::unique_ptr<Vst3Instance> vst3_;
    std::atomic<const void*> owner_{nullptr};
    Notifications remoteNotes_;
    std::vector<std::string> remoteLog_;
    bool remoteEditor_ = false;        // the bridged plugin has an embeddable editor
    std::int64_t remotePolledMs_ = 0;  // when its editor requests were last asked for
};

}  // namespace supple::host
