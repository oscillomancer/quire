// Offline rendering of a session version (spec: docs/specs/engine.md §1, §3).
//
// The session is compiled into a plan: tracks in topological order (sources
// before the tracks they feed, the master last), each track summing its
// inputs (routes and children's parent sends, in a fixed order), playing its
// MIDI items into its FX chain, then width, volume, pan (project pan law) and
// mute. Routes tap pre-FX, pre-fader or post-fader. Plugin delay
// compensation aligns every track's inputs by delaying edges, shifts a
// track's own item events by its input latency, and discards the master's
// total latency, so the output lines up with the timeline to the sample.
//
// Deterministic: fixed summation order, one audio thread, fixed block size.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
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
    std::size_t retired() const { return retired_.size(); }
    std::size_t size() const { return instances_.size(); }

  private:
    struct Entry {
        std::unique_ptr<host::Instance> instance;
        std::string pluginId;
        bool isolated = false;
        std::string appliedState;  // canonical patch text or blob hash last applied
    };
    Resolver resolve_;
    host::HostIdentity identity_;
    std::map<model::Id, Entry> instances_;
    std::vector<std::pair<std::uint64_t, std::unique_ptr<host::Instance>>> retired_;
    std::uint64_t generation_ = 0;
};

// Decoded audio media, keyed by resolved path (main thread). Reused across
// renders; the engine's disk-backed, resampling media cache replaces it.
class MediaCache {
  public:
    // Decoded audio at `rate` (resampled once, then kept; 0: the file's own
    // rate). The resampler is deterministic, so every cache gives the same
    // samples (offline ≡ real time).
    Result<std::shared_ptr<const audio::AudioData>> get(const std::filesystem::path& path,
                                                        double rate = 0);
    std::size_t size() const { return entries_.size(); }

  private:
    std::map<std::string, std::shared_ptr<const audio::AudioData>> entries_;
};

struct OfflineSettings {
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
