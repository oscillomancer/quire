// Offline rendering of a session version (spec: docs/specs/engine.md §1, §3).
//
// The session is compiled into a plan: tracks in topological order (sources
// before the tracks they feed, the master last), each track summing its
// inputs (routes and children's parent sends, in a fixed order), playing its
// MIDI items into its FX chain, then its typed Console 9 strip stages and
// fader/pan/mute (ADR-0019). Routes tap pre-FX, pre-fader or post-fader. Plugin delay
// compensation aligns every track's inputs by delaying edges, shifts a
// track's own item events by its input latency, and discards the master's
// total latency, so the output lines up with the timeline to the sample.
//
// Deterministic: fixed summation order, independent of worker scheduling.
#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "supple/audio/audio_file.h"
#include "supple/base/result.h"
#include "supple/host/clap_host.h"
#include "supple/model/session.h"

namespace supple::engine {

// Owns the plugin instances for a session's fx objects (main thread). An
// instance is created on first use and reused while its plugin stays the
// same; its patch or state is (re)applied when the model's changes.
class InstancePool {
  public:
    using Resolver = std::function<Result<host::PluginInfo>(const model::PluginRef&)>;
    InstancePool(Resolver resolve, host::HostIdentity identity = {});
    ~InstancePool();

    // The instance for `id`, configured from `fx` (patch or state blob from
    // `session`). A plugin that cannot be found is an error the caller may
    // turn into a pass-through placeholder.
    Result<host::Instance*> get(model::Id id, const model::FxInstance& fx,
                                const model::Session& session);
    // Destroys instances whose fx no longer exists in `session` (call it once
    // no plan uses them), and replaced instances retired at a generation up
    // to `liveGeneration` (the newest plan the audio side has adopted).
    void prune(const model::Session& session, std::uint64_t liveGeneration = UINT64_MAX);
    // Instances replaced from now on may still be used by plans before
    // `generation` (the real-time engine sets the next plan's generation).
    void setGeneration(std::uint64_t generation) { generation_ = generation; }
    // The next get() for `id` creates a new instance (fx.restart); the old
    // one lives until prune().
    void restart(model::Id id);
    // Isolated instances whose bridge crashed or hung, with the reason.
    std::vector<std::pair<model::Id, std::string>> failed() const;
    host::Instance* find(model::Id id) const;
    // Main thread: service and retain restart failures until fx.restart/replacement.
    host::Instance::Notifications service(model::Id id);
    std::vector<std::pair<model::Id, Error>> failedErrors() const;
    const Error* failure(model::Id id) const;
    std::uint32_t rememberedLatency(model::Id id) const;
    bool automationCleared(model::Id id, std::uint32_t param) const;
    void resetAutomation(model::Id id);  // explicit model reload
    // Main thread, only after applying plug-in-originated values to its model
    // patch: acknowledge that patch without sending it back on the next get().
    void acknowledgeOutputPatch(model::Id id, const json::Value& patch);
    // The equivalent acknowledgement for captured opaque state.
    void acknowledgeOutputState(model::Id id, const std::string& hash);

    std::size_t retired() const { return retired_.size(); }
    std::size_t size() const { return instances_.size(); }

  private:
    struct Entry {
        std::unique_ptr<host::Instance> instance;
        std::string pluginId;
        model::Isolation isolation = model::Isolation::InProcess;
        std::uint32_t latency = 0;
        std::optional<Error> failure;
        std::vector<std::uint32_t> clearedAutomation;
        std::string appliedState;  // canonical patch text or blob hash last applied
    };
    Resolver resolve_;
    host::HostIdentity identity_;
    std::shared_ptr<host::BridgeProcess> sharedBridge_;
    std::map<model::Id, Entry> instances_;
    std::vector<std::pair<std::uint64_t, std::unique_ptr<host::Instance>>> retired_;
    std::uint64_t generation_ = 0;
};

// How a take's audio is played back (session-model §1.5). `rate` is the rate
// the clip reads the variant at, one frame per timeline sample:
//  * varispeed — `pitch_mode: "resample"` or `preserve_pitch: false` — the
//    source is resampled to `rate / speed()`, so reading it at `rate` plays
//    it `speed()` times faster and the pitch follows the rate;
//  * stretch — `preserve_pitch` with the `"signalsmith"` mode — the source is
//    stretched (duration / playrate) and transposed by `pitch` semitones with
//    the pitch preserved, deterministically.
// An item with nothing to apply (playrate 1, no pitch) is the plain
// resample, and a `pitch` offset that a resample cannot honour is ignored
// (the compiler warns).
struct MediaVariant {
    double playrate = 1.0;      // 0.01 .. 100
    double pitch = 0.0;         // semitones
    bool preservePitch = true;  // ask for a stretch (needs a stretch mode)
    std::string pitchMode = "signalsmith";

    bool stretch() const {
        return preservePitch && pitchMode == "signalsmith" && (playrate != 1.0 || pitch != 0.0);
    }
    // Source seconds played per timeline second.
    double speed() const { return playrate * (preservePitch ? 1.0 : std::pow(2.0, pitch / 12.0)); }
};

// Decoded audio media, keyed by resolved path and variant (main thread).
// Reused across renders; the engine's disk-backed, resampling media cache
// replaces it.
class MediaCache {
  public:
    // Decoded audio at `rate` (resampled once, then kept; 0: the file's own
    // rate), with `variant` applied (a variant that asks for nothing is the
    // plain resample). The resampler and the stretch are deterministic, so
    // every cache gives the same samples (offline ≡ real time).
    Result<std::shared_ptr<const audio::AudioData>> get(const std::filesystem::path& path,
                                                        double rate = 0,
                                                        const MediaVariant& variant = {});
    std::size_t size() const { return entries_.size(); }

  private:
    std::map<std::string, std::shared_ptr<const audio::AudioData>> entries_;
};

struct OfflineSettings {
    model::Id master = model::kMaster;  // mix output root, ignored for isolated track taps
    double startSeconds = 0.0;
    double endSeconds = 0.0;   // exclusive; the tail follows
    double tailSeconds = 0.0;  // rendered after endSeconds
    std::uint32_t blockSize = 512;
    // Relative media paths resolve against <projectDir>/<project.media_dir>
    // (or <projectDir> itself when the file is found there).
    std::filesystem::path projectDir;
    MediaCache* media = nullptr;  // null: a cache for this render only
    // Threads rendering the graph; 0: every core but two. The output is
    // identical for any count.
    unsigned threads = 0;
    // Select one track's output without its sends or master processing.
    std::optional<model::Id> track;
    // Render this track through the master while silencing other top-level tracks.
    std::optional<model::Id> throughMaster;
    model::Tap tap = model::Tap::PostFader;
    std::shared_ptr<const std::atomic<bool>> cancelFlag;  // checked between offline blocks
    std::shared_ptr<std::atomic<double>> progressFlag;
    double progressBase = 0.0, progressScale = 1.0;
    std::stop_token stopToken;
};

struct OfflineResult {
    audio::AudioData audio;             // master output, stereo, at the project rate
    std::uint32_t outputLatency = 0;    // the compensated master latency (samples)
    std::vector<std::string> warnings;  // e.g. missing plugins rendered as pass-through
    std::vector<std::string> log;       // plugin log lines
    // The compiled graph, one line per track in processing order:
    // "tr_3 in=[tr_2 pre_fader d0 g0.707/0.707] lat_in=0 lat_chain=0" (engine.graph).
    std::vector<std::string> graph;
};

Result<OfflineResult> renderOffline(const model::Session& session, InstancePool& pool,
                                    const OfflineSettings& settings);

// Track pan (the master pans as a balance control instead).
// Constant-power pan with the project's centre attenuation (pan law, dB):
// -3 dB is classic constant power (centre 0.707, hard side 1.0); 0 dB keeps
// the centre at unity. `pan` in [-1, 1]. Returns {left gain, right gain}.
std::pair<double, double> panGains(double pan, double panLawDb);

}  // namespace supple::engine
