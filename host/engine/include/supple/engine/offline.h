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
    // Destroys instances whose fx no longer exists in `session`.
    void prune(const model::Session& session);
    std::size_t size() const { return instances_.size(); }

  private:
    struct Entry {
        std::unique_ptr<host::Instance> instance;
        std::string pluginId;
        std::string appliedState;  // canonical patch text or blob hash last applied
    };
    Resolver resolve_;
    host::HostIdentity identity_;
    std::map<model::Id, Entry> instances_;
};

struct OfflineSettings {
    double startSeconds = 0.0;
    double endSeconds = 0.0;   // exclusive; the tail follows
    double tailSeconds = 0.0;  // rendered after endSeconds
    std::uint32_t blockSize = 512;
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
