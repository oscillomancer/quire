// The sandbox session (spec: docs/specs/sandbox.md §2–4): loaded plugin
// instances, the processing chain, renders kept for comparison, settings, and
// transactional snapshots. Main thread only. The sandbox commands are thin
// wrappers over this class; it knows nothing about the command bus.
//
// IDs: instances `in_<n>`, renders `rd_<n>`; counters never go backwards
// (not even on rollback), so an ID is never reused within a session.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "supple/base/result.h"
#include "supple/host/clap_host.h"
#include "supple/json/json.h"
#include "supple/midi/midi.h"
#include "supple/sandbox/render.h"

namespace supple::sandbox {

struct SessionSettings {
    double sampleRate = 48000.0;
    std::uint32_t blockSize = 256;
    double tempo = 120.0;
    std::uint16_t numerator = 4, denominator = 4;
    double tailLimitSeconds = 5.0;
};
Status validate(const SessionSettings& s);  // errors carry field paths (/sample_rate, …)
json::Value toJson(const SessionSettings& s);

struct SessionOptions {
    std::filesystem::path workdir;  // required; created if missing
    // Plugin search path (dirs or .clap files). Empty = host::defaultSearchPath().
    std::vector<std::filesystem::path> pluginPath;
    std::filesystem::path presetDir;  // empty = <workdir>/presets
    host::HostIdentity identity;
    // Non-RT diagnostic hook for observing reference preparation in tests.
    // Called on the job worker before the file read and after analysis.
    std::function<void(std::string_view)> referencePreparationHook;
};

// What a render plays. At most one of `notes` / `audio` (neither: a silent
// render of `seconds`, useful for tails and noise floors).
struct RenderInput {
    // A MIDI item (ticks at its ppq, converted at the render tempo) for the
    // first instance, which must accept notes. Its length is the musical
    // length (at least the end of the last event).
    std::optional<midi::Sequence> notes;
    std::optional<audio::AudioData> audio;  // for the first instance (an effect)
    std::optional<double> seconds;          // overrides the musical length
    std::optional<double> tempo;            // this render only
    std::optional<midi::Meter> meter;       // transport meter, this render only
    std::optional<double> tailSeconds;      // this render only (<= 60)
    std::string name;                       // free label kept with the render
};

struct RenderRecord {
    std::string id, name;
    std::filesystem::path path;  // float32 WAV in <workdir>/renders/
    double sampleRate = 0;
    std::int64_t frames = 0;
    double seconds = 0, peakDbfs = -1000;
    std::uint32_t latency = 0;
    double tailSeconds = 0;
    bool tailCapped = false;
    std::vector<std::string> chain;  // instance ids at render time
    std::vector<std::string> log;    // plugin log lines during the render
    json::Value analysis;            // null until analyzed (core-analysis)
};

struct PresetInfo {
    std::string plugin, name;
    std::vector<std::string> tags;
    std::filesystem::path path;
};

class Session {
  public:
    static Result<std::unique_ptr<Session>> open(SessionOptions options);
    ~Session();
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    const SessionOptions& options() const { return options_; }

    // --- plugins ---------------------------------------------------------------
    // The scanned catalogue (scanned on first use; `rescan` forces it).
    Result<std::span<const host::PluginInfo>> plugins(bool rescan = false);
    // Resolves a plugin id (exact, then case-insensitive) or a .clap path.
    // Error "sandbox.unknown_plugin" with the closest ids in the hint.
    Result<host::PluginInfo> findPlugin(std::string_view idOrPath);

    // --- instances and chain ---------------------------------------------------
    // Default position: an instrument goes first, anything else last.
    Result<std::string> load(std::string_view pluginIdOrPath,
                             std::optional<std::size_t> position = {});
    Status remove(std::string_view instance);
    Status move(std::string_view instance, std::size_t position);
    const std::vector<std::string>& chain() const { return chain_; }
    // Error "sandbox.unknown_instance" listing the loaded instances.
    Result<host::Instance*> instance(std::string_view id);
    // The semantic schema captured at load (null for third-party plugins).
    const json::Value& schema(std::string_view id) const;

    // --- parameters ------------------------------------------------------------
    // Semantic plugins: keys are semantic keys and values physical (numbers,
    // unit strings, enum keys, bools), validated by the plugin. Third-party
    // plugins: keys are CLAP ids ("12") or exact parameter names; values are
    // CLAP values or {"text": "..."}. Results: {key: {"value": v, "text": t}}.
    Result<json::Value> paramGet(std::string_view instance, std::span<const std::string> keys = {});
    Result<json::Value> paramSet(std::string_view instance, const json::Value& values);

    // --- patches (semantic plugins only: "sandbox.not_semantic") ----------------
    Result<json::Value> patchGet(std::string_view instance, bool changedOnly);
    Result<std::string> patchSet(std::string_view instance,
                                 const json::Value& patch);  // → describe
    struct MergeResult {
        json::Value changed;  // the effective change as a minimal merge patch ({} if none)
        std::string description;
    };
    Result<MergeResult> patchMerge(std::string_view instance, const json::Value& mergePatch);
    Result<std::string> describe(std::string_view instance, bool changedOnly);

    // --- presets ---------------------------------------------------------------
    // <presetDir>/<plugin id>/<name>.json: the patch (semantic) or the base64
    // CLAP state (third-party). Names: 1–64 of [A-Za-z0-9 _.+-], not starting
    // with '.' or ' '.
    Result<std::filesystem::path> presetSave(std::string_view instance, std::string_view name,
                                             std::span<const std::string> tags = {},
                                             std::string_view notes = {});
    Result<std::string> presetLoad(std::string_view instance, std::string_view name);
    Result<std::vector<PresetInfo>> presetList(std::string_view plugin = {});

    // --- rendering -------------------------------------------------------------
    Result<const RenderRecord*> render(const RenderInput& input);
    // Validates `input` and renders it through `chain` with `settings`,
    // recording nothing (usable from a worker thread that owns the chain).
    static Result<RenderResult> renderThrough(std::span<host::Instance* const> chain,
                                              const RenderInput& input,
                                              const SessionSettings& settings);
    // Snapshot only the source state on the model thread. The worker creates,
    // initializes, loads and destroys its own CLAP instance on one thread.
    struct CloneState {
        host::PluginInfo info;
        host::HostIdentity identity;
        std::vector<std::uint8_t> bytes;
    };
    Result<CloneState> snapshotInstance(std::string_view id);
    // Background renders: reserve an id and path on the model thread, write
    // the WAV and submit the record from a worker; the record joins the
    // session when the model thread next calls adoptBackgroundRenders().
    RenderRecord reserveRender(std::string name);
    void submitRender(RenderRecord record);  // any thread
    void adoptBackgroundRenders();           // model thread
    std::string ridPrefixForTests() const { return "rd_"; }
    Result<const RenderRecord*> findRender(std::string_view id) const;
    RenderRecord* mutableRender(std::string_view id);  // e.g. to attach analysis
    const std::map<std::string, RenderRecord>& renders() const { return renders_; }

    // --- settings / whole session ------------------------------------------------
    const SessionSettings& settings() const { return settings_; }
    Status setSettings(const SessionSettings& s);  // deactivates instances if rate/block changed
    json::Value summary() const;                   // settings, chain, instances, renders
    void reset();                                  // unload everything, delete renders

    // --- background jobs ----------------------------------------------------------
    // Runs `work` on its own thread; stopJobs() requests stop and joins all.
    void startJob(std::function<void(std::stop_token)> work);
    void stopJobs();
    std::size_t jobThreadCountForTests() const { return jobs_.size(); }
    std::size_t activeJobThreadCountForTests() const {
        std::size_t active = 0;
        for (const auto& job : jobs_)
            if (!job.finished->load(std::memory_order_acquire)) ++active;
        return active;
    }

    // --- transactions ----------------------------------------------------------
    // One level (the bus wraps each edit or batch in one). Rollback restores
    // the chain, settings, the state of every instance that existed at begin,
    // re-inserts instances removed since, drops instances loaded since, and
    // deletes renders made since.
    Status begin();
    void commit();
    void rollback();
    bool inTransaction() const { return txn_ != nullptr; }

  private:
    struct Loaded;
    struct Txn;
    explicit Session(SessionOptions options);
    Result<Loaded*> loaded(std::string_view id);
    const Loaded* findLoaded(std::string_view id) const;
    Result<Loaded*> semantic(std::string_view id);
    std::string candidates() const;

    SessionOptions options_;
    SessionSettings settings_;
    std::optional<std::vector<host::PluginInfo>> catalogue_;
    std::map<std::string, std::unique_ptr<Loaded>> instances_;
    std::vector<std::string> chain_;
    std::map<std::string, RenderRecord> renders_;
    std::uint64_t nextInstance_ = 1, nextRender_ = 1;
    std::unique_ptr<Txn> txn_;
    std::mutex backgroundMutex_;
    std::vector<RenderRecord> backgroundRenders_;
    struct JobThread {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<JobThread> jobs_;
};

// Tick → sample at a constant tempo: round(tick · 60 · rate / (tempo · ppq)).
std::int64_t ticksToSamples(std::int64_t ticks, int ppq, double tempo, double sampleRate);

// Events for the chain's first stage: notes as note on/off (offs before ons
// at the same sample; CLAP note ids 0, 1, 2, … in note order; a zero-length
// note lasts one sample) and controls (CC, program, pressure, pitch bend) as
// MIDI 1.0 events between them. Sysex is not sent.
std::vector<TimedEvent> sequenceEvents(const midi::Sequence& sequence, double tempo,
                                       double sampleRate);

}  // namespace supple::sandbox
