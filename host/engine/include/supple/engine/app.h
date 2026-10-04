// The DAW application core without a GUI: the session history, plugins,
// rendering and (optionally) the real-time engine behind the command bus
// (session-model §6–7, engine.md §6). Model thread only.
//
// Every bus transaction (one edit command, or one batch) builds one EditTxn
// on the current branch head and commits as one version: one undo step, with
// its resolved invocations recorded for replay merges. Queries inside a batch
// read the transaction's preview, so they see the earlier steps.
//
// Plugin state lives in the model (FxInstance.patch / state blob). Patch
// edits are merged and validated on a scratch instance of the plugin; the
// instances that make sound follow the model on the next plan (InstancePool).
#pragma once

#include <atomic>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "supple/cmd/bus.h"
#include "supple/engine/offline.h"
#include "supple/engine/realtime.h"
#include "supple/lua/runtime.h"
#include "supple/model/versions.h"

namespace supple::engine {

struct AppOptions {
    std::vector<std::filesystem::path> pluginPath;  // empty = host::defaultSearchPath()
    host::HostIdentity identity;
    std::filesystem::path chainDir;     // empty = the user config directory / <slug> / chains
    std::filesystem::path templateDir;  // empty = … / <slug> / track-templates
    std::filesystem::path workdir;      // renders are written to <workdir>/renders
    // An unsaved project's own folder (the GUI's, one per run): its media (the
    // recordings) live in <scratchDir>/<media_dir> until the first save moves
    // them beside the project file; clearScratch() deletes it when the
    // unsaved project is discarded. Empty: <workdir>/media, never deleted.
    std::filesystem::path scratchDir;
    std::string jackLibrary = "libjack.so.0";  // tests use a stand-in
    std::function<void()> jobStartedHook;      // worker-entry barrier for deterministic tests
};

// Meter entries a full ring dropped (P5.3c): the renderer's ring and the audio
// callback's. A drop means a strip's peak/RMS missed a block, not that audio
// was lost.
struct MeterDrops {
    std::int64_t renderer = 0, callback = 0;
};

// The real-time engine as engine.status reports it.
struct EngineStatus {
    bool running = false;
    std::string backend;
    double sampleRate = 0;
    std::int64_t block = 0, period = 0;
    std::int64_t planLatency = 0, deviceLatency = 0;  // frames
    std::int64_t xruns = 0, cycles = 0, processErrors = 0;
    bool restartNeeded = false;
    bool anticipative = false;  // rendering ahead on the worker pool (ADR-0012)
    std::int64_t anticipationMisses = 0;
    MeterDrops meterDrops;              // meter entries dropped by a full ring
    std::vector<std::string> failedFx;  // isolated instances whose bridge crashed or hung
};

struct RecordedInfo {
    std::string track, item, take, file;  // take: the active one
    double seconds = 0;
    std::int64_t lostFrames = 0;     // input lost (the disk fell behind, playback dropped out)
    std::vector<std::string> takes;  // every take (one per loop pass)
};

// A transport range (loop or punch), in seconds (ADR-0015).
struct TransportRange {
    bool enabled = false;
    double start = 0, end = 0;
};

// The metronome (transport state like the loop: not versioned, not saved).
struct MetronomeSettings {
    bool enabled = false;
    double volumeDb = -6.0;
    int countInBars = 0;           // clicks before a recording starts (0 = none)
    bool countInPlayback = false;  // and before plain playback
};

// A recording no version of the session uses (after a crash: a take that
// was never committed, or committed but never saved), and where its sidecar
// says it belongs.
struct RecoverableInfo {
    std::string file;  // relative to the project's media directory ("rec/Vocal-1.wav")
    std::string track;
    double startSeconds = 0, seconds = 0;
    std::int64_t channels = 0;
};

struct TransportInfo {
    bool playing = false, recording = false;
    bool countingIn = false;
    double seconds = 0, beat = 0;
    std::int64_t sample = 0;
    TransportRange loop, punch;
    std::optional<std::vector<RecordedInfo>> recorded;  // takes a stop/pause just committed
};

// One strip's meter reading (P5.3c): the peak and RMS of the audio of that
// strip's post signal (post-fader, post-pan) that has been *heard* since the
// last call. The engine does no decay or peak hold — the GUI does. A strip
// with no heard audio reports zeros. `track` is the strip's track id.
struct MeterReading {
    model::Id track;
    float peak[2] = {0.0f, 0.0f};
    float rms[2] = {0.0f, 0.0f};
};

class EngineApp : public cmd::Host {
  public:
    static Result<std::unique_ptr<EngineApp>> create(AppOptions options,
                                                     model::Project project = {});
    ~EngineApp() override;
    EngineApp(const EngineApp&) = delete;
    EngineApp& operator=(const EngineApp&) = delete;

    // --- state ------------------------------------------------------------------
    const model::Session& head() const;
    model::Session view() const;  // the open transaction's preview, else the head
    model::EditTxn& txn();        // the open transaction (edit commands only)
    bool inTransaction() const { return txn_ != nullptr; }
    model::VersionStore& versions() { return *versions_; }
    const AppOptions& options() const { return options_; }
    const std::filesystem::path& projectPath() const { return projectPath_; }
    // Where relative media paths resolve: the saved project's folder, or the
    // scratch folder of an unsaved one (empty without either).
    std::filesystem::path projectDir() const;
    // Deletes the scratch folder (an unsaved project's recordings): called when
    // the unsaved project is discarded (quit, new, open). Does nothing once the
    // project is saved, and never touches a folder holding the work directory.
    void clearScratch();

    // --- plugins ----------------------------------------------------------------
    Result<std::span<const host::PluginInfo>> plugins(bool rescan = false);
    // By id (exact, then case-insensitive) or display name; hint lists the closest ids.
    Result<host::PluginInfo> findPlugin(std::string_view idOrName);
    InstancePool& instances() { return *pool_; }
    MediaCache& media() { return media_; }
    // The plugin's semantic schema (null for plugins without the extension).
    Result<json::Value> fxSchema(const model::FxInstance& fx);
    // The plugin's CLAP parameters (from a scratch instance).
    Result<std::vector<host::ParamInfo>> fxParams(const model::FxInstance& fx);
    Result<std::uint32_t> fxSidechainChannels(const model::FxInstance& fx);  // 0: no key input
    // The instance's reported latency in samples (the running one when there is
    // one, else a throwaway activation at the project rate).
    Result<std::uint32_t> fxLatencySamples(model::Id id, const model::FxInstance& fx);
    // fx's patch after applying `patch` (a merge patch, or a complete patch
    // when `replace`), validated by the plugin; the complete canonical patch.
    Result<json::Value> mergedPatch(const model::FxInstance& fx, const json::Value& patch,
                                    bool replace);
    Result<std::string> describeFx(const model::FxInstance& fx, bool changedOnly);

    // --- project files ------------------------------------------------------------
    Status save(const std::filesystem::path& path);  // atomic; becomes the project path
    Status open(const std::filesystem::path& path,
                std::vector<schema::Warning>* warnings = nullptr);  // replaces the history
    void newProject(model::Project project);

    // --- branches ------------------------------------------------------------------
    // Re-executes recorded invocations on `base` as `version` (the replayer
    // for merges): commands run against a temporary transaction on that base.
    Result<model::Replayed> replay(const cmd::Registry& registry, const model::Session& base,
                                   const std::vector<model::Invocation>& invocations,
                                   std::uint64_t version);
    Status switchBranch(std::string_view name);  // reloads the engine
    model::MergeOutcome mergeBranch(const cmd::Registry& registry, std::string_view from,
                                    std::string_view into);

    // --- rendering ---------------------------------------------------------------
    Result<OfflineResult> renderMix(const OfflineSettings& settings);
    Result<OfflineResult> renderSession(const model::Session& session,
                                        const OfflineSettings& settings);
    std::filesystem::path rendersDir() const { return options_.workdir / "renders"; }
    std::string rememberRender(const std::filesystem::path& path);
    std::string reserveRenderId();
    void publishRender(std::string id, const std::filesystem::path& path);
    Result<std::filesystem::path> renderPath(std::string_view reference) const;
    void startJob(std::function<void(std::stop_token)> work);
    void stopJobs() override;

    // --- real-time engine (optional) ------------------------------------------------
    Status startEngine(Backend& backend, const BackendConfig& config);
    Status startEngine(Backend& backend, const BackendConfig& config, const EngineOptions& options);
    // On a backend the app owns: "jack" (the audio device, at the graph's
    // rate) or "dummy" (no device, paced like one, at the project's rate).
    Status startEngine(std::string_view backend, std::uint32_t block, bool anticipative = true);
    void stopEngine();
    Engine* engine() { return engine_.get(); }
    const Backend* backend() const { return backend_; }  // null while stopped
    EngineStatus engineStatus();
    // The meters (P5.3c): one reading per strip of the newest plan, covering
    // the audio heard since the last call. Call it from this (the model)
    // thread, the way the GUI's tick does; empty when the engine is stopped.
    // Main-thread work only: it drains the engine's meter rings.
    std::vector<MeterReading> meters();
    // fx.restart: the next plan gets a new instance of `fx` (the failed one is
    // freed once no plan uses it).
    void restartFx(model::Id fx);
    // Emits fx.failed once for each isolated instance that has failed.
    void checkFailures();
    // Commits recorded takes as items on their tracks (one version).
    Result<std::vector<RecordedInfo>> commitTakes(const std::vector<RecordedTake>& takes,
                                                  std::string_view label = "record");

    // --- recordings (crash recovery, ADR-0014 §3) -----------------------------------
    // Where media lives: next to the project file, or the work directory's
    // media/ before the project has one. Recordings go to its rec/.
    std::filesystem::path mediaDir() const;
    std::filesystem::path recordDir() const { return mediaDir() / "rec"; }
    // Recordings in recordDir() with a sidecar that no version uses and that
    // are not being recorded now, oldest name first.
    std::vector<RecoverableInfo> recoverable() const;
    // Repairs their headers (the frames written after the last rewrite count)
    // and commits them as one version ("recover"), each on its sidecar's track
    // (or `track`) at its recorded position. `files` as recoverable() lists
    // them; empty: all.
    Result<std::vector<RecordedInfo>> recover(const std::vector<std::string>& files,
                                              std::optional<model::Id> track);
    // The real-time engine's instance of `fx` (tests and tools), or null.
    host::Instance* instanceOf(model::Id fx) const { return pool_ ? pool_->find(fx) : nullptr; }
    // Transport requests. With a device, each waits (at most a second) until
    // the audio thread has acted on it, so the answer is the new state.
    Result<TransportInfo> transport(std::string_view action, std::optional<double> seconds = {});
    // Automation writing (P5.14): while the transport plays, a change to a
    // strip's volume/pan/mute (inside the edit's transaction) is recorded into
    // its envelope when that envelope is in touch, latch or write mode. At
    // pause/stop latch and write hold the last value to the stop position, and
    // a write envelope never touched is flattened over the pass (one
    // transaction, "Automation written").
    void recordAutomation(model::Id object, const std::string& param, double value);
    // Whether a change to this parameter would be recorded now.
    bool writesAutomation(model::Id object, const std::string& param) const;
    // transport.loop / transport.punch (ADR-0015): transport state, not
    // versioned; the loop reaches the engine (now, and whenever it starts),
    // the punch range trims takes when a recording is committed.
    void setLoop(const TransportRange& loop);
    void setPunch(const TransportRange& punch) { punch_ = punch; }
    const TransportRange& loop() const { return loop_; }
    const MetronomeSettings& metronome() const { return metronome_; }
    void setMetronome(const MetronomeSettings& m);  // reaches the engine now and on every start
    const TransportRange& punch() const { return punch_; }

    // --- periodic service (the servers' tick, every 100 ms on the model thread) ------
    // Frees retired plans, emits fx.failed (so subscribers see failures without
    // asking), restarts the engine after a device change, and autosaves the
    // project when its head changed and an autosave interval has passed.
    void tick();

    // Where autosaves go, next to the project file (<file>-data); empty when
    // the project has no file yet.
    std::filesystem::path autosaveDir() const;
    // The newest autosave of the project file `project` that is newer than the
    // file itself, or empty. project.open reports it instead of loading it.
    std::filesystem::path newerAutosave(const std::filesystem::path& project) const;

    // --- scripting ------------------------------------------------------------------
    // The Lua runtime scripts run in (ADR-0006). Attach the command dispatcher
    // once it exists: scripts then reach the bus and receive its events after
    // the command that emitted them. Null before that; script.run says so.
    void attachCommands(cmd::Dispatcher& dispatcher);
    lua::Runtime* scripting() { return scripts_.get(); }

    // --- cmd::Host ------------------------------------------------------------------
    cmd::Context makeContext(const cmd::Origin& origin) override;
    Result<std::uint64_t> beginTransaction(const cmd::Origin& origin,
                                           std::string_view label) override;
    Status commitTransaction(std::uint64_t id) override;
    void rollbackTransaction(std::uint64_t id) override;
    void log(const cmd::LogRecord& record) override;
    void emit(std::string_view topic, json::Value payload) override;
    void commandFinished() override;
    bool actionTransactional(std::string_view command) const override;
    std::uint64_t clockMs() const override;
    void invoked(std::string_view command, int version, const json::Value& args,
                 cmd::Kind kind) override;
    // Example fixtures: empty, saved, song (tr_1 "Bass"), song_with_item
    // (+ it_1: a MIDI bar), song_with_fx (+ fx_1 sine, fx_2 gain), song_with_env (+ en_1
    // on fx_2 gain), song_undone,
    // engine_running (the dummy backend), engine_armed (+ tr_1 armed),
    // recording_interrupted (tr_1 + rec/Vocal-1.wav left by a crash), meter_change
    // (3/4 from bar 9), and the
    // bus's job_running. Built by running commands.
    void setupFixture(cmd::Dispatcher& dispatcher, std::string_view fixture) override;
    // Relative project paths resolve against the work directory.
    std::filesystem::path resolve(const std::filesystem::path& path) const;

    // Tests and tools.
    // The command log (log.list): the most recent kMaxRecords invocations.
    const std::deque<cmd::LogRecord>& records() const { return records_; }
    static constexpr std::size_t kMaxRecords = 2000;
    std::vector<std::pair<std::string, json::Value>> takeEvents();
    void setFixedClock(std::uint64_t ms) { fixedClock_ = ms; }

  private:
    void startPlaying(int countInBars);  // engine play(), after a count-in when > 0
    explicit EngineApp(AppOptions options);
    Result<host::Instance*> scratch(const model::FxInstance& fx);
    bool inRecordDir(const std::filesystem::path& file) const;
    void headChanged();  // reload the real-time engine
    void haltEngine();   // tears the engine down (stopEngine also cancels a restart)
    // The device changed under the running engine: stop it and remember to
    // start it again (same backend kind, block and options).
    void beginRestart(std::string reason, double seconds);
    // Tries a pending restart now (with backoff until 30 s have passed).
    void retryRestart();
    // Writes one autosave (rotating: the newest 5 stay) and marks the head
    // saved. Errors are emitted as engine.error.
    void autosave();
    void maybeAutosave();  // autosave() when the head changed and an interval passed

    AppOptions options_;
    std::unique_ptr<model::VersionStore> versions_;
    std::unique_ptr<model::EditTxn> txn_;
    model::TxnRecord record_;
    std::uint64_t txnId_ = 0;
    std::optional<std::vector<host::PluginInfo>> catalogue_;
    std::unique_ptr<InstancePool> pool_;
    MediaCache media_;
    std::map<std::string, std::filesystem::path> renders_;
    mutable std::mutex rendersMutex_;
    std::uint64_t nextRender_ = 1;
    struct JobThread {
        std::jthread thread;
        std::shared_ptr<std::atomic<bool>> finished;
    };
    std::vector<JobThread> jobs_;
    std::map<std::string, std::unique_ptr<host::Instance>> scratch_;
    Status moveScratchMedia(const std::filesystem::path& dest);
    std::unique_ptr<Engine> engine_;
    Backend* backend_ = nullptr;
    std::unique_ptr<Backend> ownedBackend_;
    double playStart_ = 0.0;  // transport.stop returns here
    // Automation written during this pass, per envelope (transient).
    struct AutoWrite {
        double startBeat = 0, lastBeat = 0, lastValue = 0;
        bool touched = false;
    };
    std::map<model::Id, AutoWrite> autoWrite_;
    void beginAutomationPass(double beat);
    Status finishAutomationPass(double stopBeat);
    TransportRange loop_, punch_;  // kept across engine restarts (not in the session)
    MetronomeSettings metronome_;
    std::vector<std::filesystem::path> recordingFiles_;  // while recording
    std::set<model::Id> reportedFailures_;
    std::filesystem::path projectPath_;
    std::unique_ptr<lua::Runtime> scripts_;
    std::deque<cmd::LogRecord> records_;
    std::vector<std::pair<std::string, json::Value>> events_;
    std::uint64_t fixedClock_ = 0;

    // The engine's start recipe, kept to restart it after a device change.
    std::string engineKind_;  // "jack"/"dummy" when the app owns the backend
    std::uint32_t engineBlock_ = 0;
    bool engineAnticipative_ = true;
    Backend* restartBackend_ = nullptr;  // a caller-supplied backend (not owned)
    BackendConfig restartConfig_{};
    EngineOptions restartOptions_{};
    bool restarting_ = false;
    std::string restartReason_;
    double restartSeconds_ = 0.0;       // the transport position to restore
    std::uint64_t restartUntil_ = 0;    // give up (engine.error) at this clock
    std::uint64_t restartRetryAt_ = 0;  // the next attempt
    std::uint64_t restartBackoff_ = 0;  // ms, doubling to 4 s

    // Autosave: the head version last written and when.
    bool autosaveMarked_ = false;
    std::uint64_t autosaveMs_ = 0;
    std::uint64_t autosaveVersion_ = 0;
};

// Registers the engine's command catalogue (session-model §7): project.*,
// track.*, item.*, midi.*, fx.*, render.mix, history.*, session.describe, …
void registerEngineCommands(cmd::Registry& registry);

// Compact text description of the session for LLMs (session-model §4).
std::string describeSession(const model::Session& s);

}  // namespace supple::engine
