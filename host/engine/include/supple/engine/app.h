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

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "supple/cmd/bus.h"
#include "supple/engine/offline.h"
#include "supple/engine/realtime.h"
#include "supple/model/versions.h"

namespace supple::engine {

struct AppOptions {
    std::vector<std::filesystem::path> pluginPath;  // empty = host::defaultSearchPath()
    host::HostIdentity identity;
    std::filesystem::path workdir;             // renders are written to <workdir>/renders
    std::string jackLibrary = "libjack.so.0";  // tests use a stand-in
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
};

struct TransportInfo {
    bool playing = false;
    double seconds = 0, beat = 0;
    std::int64_t sample = 0;
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

    // --- plugins ----------------------------------------------------------------
    Result<std::span<const host::PluginInfo>> plugins(bool rescan = false);
    // By id (exact, then case-insensitive) or display name; hint lists the closest ids.
    Result<host::PluginInfo> findPlugin(std::string_view idOrName);
    InstancePool& instances() { return *pool_; }
    MediaCache& media() { return media_; }
    // The plugin's semantic schema (null for plugins without the extension).
    Result<json::Value> fxSchema(const model::FxInstance& fx);
    // fx's patch after applying `patch` (a merge patch, or a complete patch
    // when `replace`), validated by the plugin; the complete canonical patch.
    Result<json::Value> mergedPatch(const model::FxInstance& fx, const json::Value& patch,
                                    bool replace);
    Result<std::string> describeFx(const model::FxInstance& fx, bool changedOnly);

    // --- project files ------------------------------------------------------------
    Status save(const std::filesystem::path& path);  // atomic; becomes the project path
    Status open(const std::filesystem::path& path);  // replaces the history
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
    std::filesystem::path rendersDir() const { return options_.workdir / "renders"; }

    // --- real-time engine (optional) ------------------------------------------------
    Status startEngine(Backend& backend, const BackendConfig& config);
    // On a backend the app owns: "jack" (the audio device, at the graph's
    // rate) or "dummy" (no device, paced like one, at the project's rate).
    Status startEngine(std::string_view backend, std::uint32_t block);
    void stopEngine();
    Engine* engine() { return engine_.get(); }
    EngineStatus engineStatus() const;
    // Transport requests. With a device, each waits (at most a second) until
    // the audio thread has acted on it, so the answer is the new state.
    Result<TransportInfo> transport(std::string_view action, std::optional<double> seconds = {});

    // --- cmd::Host ------------------------------------------------------------------
    cmd::Context makeContext(const cmd::Origin& origin) override;
    Result<std::uint64_t> beginTransaction(const cmd::Origin& origin,
                                           std::string_view label) override;
    Status commitTransaction(std::uint64_t id) override;
    void rollbackTransaction(std::uint64_t id) override;
    void log(const cmd::LogRecord& record) override;
    void emit(std::string_view topic, json::Value payload) override;
    bool actionTransactional(std::string_view command) const override;
    std::uint64_t clockMs() const override;
    void invoked(std::string_view command, int version, const json::Value& args,
                 cmd::Kind kind) override;
    // Example fixtures: empty, saved, song (tr_1 "Bass"), song_with_item
    // (+ it_1: a MIDI bar), song_with_fx (+ fx_1 sine, fx_2 gain), song_undone,
    // engine_running (the dummy backend), and the bus's job_running. Built by
    // running commands.
    void setupFixture(cmd::Dispatcher& dispatcher, std::string_view fixture) override;
    // Relative project paths resolve against the work directory.
    std::filesystem::path resolve(const std::filesystem::path& path) const;

    // Tests and tools.
    const std::vector<cmd::LogRecord>& records() const { return records_; }
    std::vector<std::pair<std::string, json::Value>> takeEvents();
    void setFixedClock(std::uint64_t ms) { fixedClock_ = ms; }

  private:
    explicit EngineApp(AppOptions options);
    Result<host::Instance*> scratch(const model::FxInstance& fx);
    void headChanged();  // reload the real-time engine

    AppOptions options_;
    std::unique_ptr<model::VersionStore> versions_;
    std::unique_ptr<model::EditTxn> txn_;
    model::TxnRecord record_;
    std::uint64_t txnId_ = 0;
    std::optional<std::vector<host::PluginInfo>> catalogue_;
    std::unique_ptr<InstancePool> pool_;
    MediaCache media_;
    std::map<std::string, std::unique_ptr<host::Instance>> scratch_;
    std::unique_ptr<Engine> engine_;
    Backend* backend_ = nullptr;
    std::unique_ptr<Backend> ownedBackend_;
    double playStart_ = 0.0;  // transport.stop returns here
    std::filesystem::path projectPath_;
    std::vector<cmd::LogRecord> records_;
    std::vector<std::pair<std::string, json::Value>> events_;
    std::uint64_t fixedClock_ = 0;
};

// Registers the engine's command catalogue (session-model §7): project.*,
// track.*, item.*, midi.*, fx.*, render.mix, history.*, session.describe, …
void registerEngineCommands(cmd::Registry& registry);

// Compact text description of the session for LLMs (session-model §4).
std::string describeSession(const model::Session& s);

}  // namespace supple::engine
