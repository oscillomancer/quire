#include "supple/engine/app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

#include "supple/base/closest.h"
#include "supple/base/files.h"
#include "supple/clap/semantic_params.h"
#include "supple/engine/jack.h"
#include "supple/model/schemas.h"
#include "supple/product.h"

namespace supple::engine {

namespace fs = std::filesystem;
using model::Id;

namespace {
bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}
std::string originText(const cmd::Origin& o) {
    static constexpr const char* kTypes[] = {"ui", "lua", "mcp", "rpc", "test", "internal"};
    std::string s = kTypes[static_cast<int>(o.type)];
    if (!o.client.empty()) s += ":" + o.client;
    if (!o.agent.empty()) s += "/" + o.agent;
    return s;
}
}  // namespace

EngineApp::EngineApp(AppOptions options) : options_(std::move(options)) {}

EngineApp::~EngineApp() {
    stopEngine();
    scratch_.clear();
}

Result<std::unique_ptr<EngineApp>> EngineApp::create(AppOptions options, model::Project project) {
    if (options.workdir.empty())
        return Error("engine.workdir", "no work directory given", {}, "pass a writable directory");
    std::error_code ec;
    fs::create_directories(options.workdir / "renders", ec);
    if (ec) return Error("engine.workdir", "cannot create " + options.workdir.string());
    auto app = std::unique_ptr<EngineApp>(new EngineApp(std::move(options)));
    EngineApp* self = app.get();
    app->pool_ = std::make_unique<InstancePool>(
        [self](const model::PluginRef& ref) { return self->findPlugin(ref.id); },
        app->options_.identity);
    app->newProject(std::move(project));
    return app;
}

const model::Session& EngineApp::head() const { return *versions_->head(); }

model::Session EngineApp::view() const { return txn_ ? txn_->preview() : head(); }

model::EditTxn& EngineApp::txn() {
    assert(txn_ && "edit command outside a transaction");
    return *txn_;
}

// --- plugins -------------------------------------------------------------------------

Result<std::span<const host::PluginInfo>> EngineApp::plugins(bool rescan) {
    if (!catalogue_ || rescan) {
        const auto path =
            options_.pluginPath.empty() ? host::defaultSearchPath() : options_.pluginPath;
        auto list = host::scan(path);
        if (!list) return std::move(list).error();
        std::sort(list->begin(), list->end(), [](const auto& a, const auto& b) {
            return a.id != b.id ? a.id < b.id : a.path < b.path;
        });
        catalogue_ = std::move(*list);
    }
    return std::span<const host::PluginInfo>(*catalogue_);
}

Result<host::PluginInfo> EngineApp::findPlugin(std::string_view idOrName) {
    auto all = plugins();
    if (!all) return std::move(all).error();
    for (const auto& p : *all)
        if (p.id == idOrName) return p;
    for (const auto& p : *all)
        if (iequals(p.id, idOrName) || iequals(p.name, idOrName)) return p;
    std::vector<std::string_view> ids;
    for (const auto& p : *all) ids.push_back(p.id);
    const std::string best = closestMatch(idOrName, ids);
    return Error(
        "plugin.unknown", "no plugin \"" + std::string(idOrName) + "\"", {},
        (best.empty() ? std::string() : "did you mean \"" + best + "\"? ") + "see plugin.list");
}

Result<host::Instance*> EngineApp::scratch(const model::FxInstance& fx) {
    auto& slot = scratch_[fx.plugin.id];
    if (!slot) {
        auto info = findPlugin(fx.plugin.id);
        if (!info) {
            scratch_.erase(fx.plugin.id);
            return std::move(info).error();
        }
        auto made = host::Instance::create(*info, options_.identity);
        if (!made) {
            scratch_.erase(fx.plugin.id);
            return std::move(made).error();
        }
        slot = std::move(*made);
    }
    return slot.get();
}

Result<json::Value> EngineApp::fxSchema(const model::FxInstance& fx) {
    auto in = scratch(fx);
    if (!in) return std::move(in).error();
    if (!(*in)->info().semantic) return json::Value();
    return (*in)->semanticSchema();
}

Result<json::Value> EngineApp::mergedPatch(const model::FxInstance& fx, const json::Value& patch,
                                           bool replace) {
    auto inr = scratch(fx);
    if (!inr) return std::move(inr).error();
    host::Instance& in = **inr;
    if (!in.info().semantic)
        return Error("fx.not_semantic", fx.plugin.id + " has no semantic patch", {},
                     "use fx.param.set with CLAP ids or names");
    // Start from the model's patch (or the plugin's defaults).
    if (fx.patch) {
        SUPPLE_TRY(in.setPatch(json::writeCompact(*fx.patch), CLAP_SUPPLE_SET_REPLACE));
    } else {
        auto schema = in.semanticSchema();
        if (!schema) return std::move(schema).error();
        json::Value defaults = json::Value::object();
        defaults["schema"] =
            schema->find("patch_type") ? *schema->find("patch_type") : json::Value("");
        SUPPLE_TRY(in.setPatch(json::writeCompact(defaults), CLAP_SUPPLE_SET_REPLACE));
    }
    SUPPLE_TRY(in.setPatch(json::writeCompact(patch),
                           replace ? CLAP_SUPPLE_SET_REPLACE : CLAP_SUPPLE_SET_MERGE));
    auto text = in.getPatch(false);
    if (!text) return std::move(text).error();
    return json::parse(*text);
}

Result<std::string> EngineApp::describeFx(const model::FxInstance& fx, bool changedOnly) {
    auto inr = scratch(fx);
    if (!inr) return std::move(inr).error();
    host::Instance& in = **inr;
    if (!in.info().semantic) return in.info().name;
    if (fx.patch) SUPPLE_TRY(in.setPatch(json::writeCompact(*fx.patch), CLAP_SUPPLE_SET_REPLACE));
    return in.describe(changedOnly);
}

// --- project files ----------------------------------------------------------------------

void EngineApp::newProject(model::Project project) {
    versions_ = std::make_unique<model::VersionStore>(model::Session::create(std::move(project)));
    projectPath_.clear();
    headChanged();
}

fs::path EngineApp::resolve(const fs::path& path) const {
    return path.is_relative() ? options_.workdir / path : path;
}

Status EngineApp::save(const fs::path& where) {
    const fs::path path = resolve(where);
    SUPPLE_TRY(writeFileAtomic(path, model::serializeProject(head())));
    projectPath_ = path;
    return okStatus();
}

Status EngineApp::open(const fs::path& where) {
    const fs::path path = resolve(where);
    auto text = readTextFile(path, std::size_t(1) << 30);
    if (!text) return std::move(text).error();
    auto s = model::parseProject(*text);
    if (!s) return std::move(s).error();
    versions_ = std::make_unique<model::VersionStore>(std::move(*s));
    projectPath_ = path;
    headChanged();
    return okStatus();
}

// --- branches ----------------------------------------------------------------------------

Result<model::Replayed> EngineApp::replay(const cmd::Registry& registry, const model::Session& base,
                                          const std::vector<model::Invocation>& invocations,
                                          std::uint64_t version) {
    std::unique_ptr<model::EditTxn> saved = std::move(txn_);
    model::TxnRecord savedRecord = std::move(record_);
    txn_ = std::make_unique<model::EditTxn>(base, version);
    auto restore = [&] {
        txn_ = std::move(saved);
        record_ = std::move(savedRecord);
    };
    for (std::size_t i = 0; i < invocations.size(); ++i) {
        const model::Invocation& inv = invocations[i];
        const cmd::CommandInfo* info = registry.find(inv.command);
        if (!info) {
            restore();
            return Error("history.replay", "unknown command " + inv.command + " in history");
        }
        cmd::Context ctx = makeContext({cmd::Origin::Type::Internal, "replay", ""});
        ctx.provide<const cmd::Registry>(registry);
        auto r = info->invoke(inv.args, ctx);
        if (!r) {
            Error e = std::move(r).error();
            restore();
            e.message = inv.command + ": " + e.message;
            return e;
        }
    }
    model::Replayed out;
    out.changes = txn_->changes();
    out.created = txn_->created();
    auto s = txn_->commit();
    restore();
    if (!s) return std::move(s).error();
    out.session = std::move(*s);
    return out;
}

Status EngineApp::switchBranch(std::string_view name) {
    SUPPLE_TRY(versions_->switchBranch(name));
    headChanged();
    return okStatus();
}

model::MergeOutcome EngineApp::mergeBranch(const cmd::Registry& registry, std::string_view from,
                                           std::string_view into) {
    auto outcome = versions_->merge(
        from, into,
        [&](const model::Session& base, const std::vector<model::Invocation>& inv,
            std::uint64_t version) { return replay(registry, base, inv, version); },
        [&](std::string_view command) -> const json::Value* {
            const cmd::CommandInfo* info = registry.find(command);
            return info ? &info->argsSchema : nullptr;
        },
        clockMs());
    if (outcome.conflicts.empty() && !outcome.upToDate && versions_->branch() == into)
        headChanged();
    return outcome;
}

// --- rendering ----------------------------------------------------------------------------

Result<OfflineResult> EngineApp::renderMix(const OfflineSettings& settings) {
    OfflineSettings s = settings;
    if (s.projectDir.empty() && !projectPath_.empty()) s.projectDir = projectPath_.parent_path();
    if (!s.media) s.media = &media_;
    // Offline renders use their own plugin instances (engine.md §3): playback
    // is never disturbed and state is deterministic.
    InstancePool offlinePool([this](const model::PluginRef& ref) { return findPlugin(ref.id); },
                             options_.identity);
    return renderOffline(view(), offlinePool, s);  // inside a batch: with the earlier steps
}

// --- real-time engine -----------------------------------------------------------------------

Status EngineApp::startEngine(Backend& backend, const BackendConfig& config) {
    return startEngine(backend, config, EngineOptions{});
}

Status EngineApp::startEngine(Backend& backend, const BackendConfig& config,
                              const EngineOptions& options) {
    if (engine_) return Error("engine.running", "the engine is already running");
    engine_ = std::make_unique<Engine>(*pool_, media_, options);
    auto st = engine_->start(backend, config);
    if (!st) {
        engine_.reset();
        return st;
    }
    backend_ = &backend;
    playStart_ = 0.0;
    return engine_->load(head(), projectPath_.empty() ? fs::path() : projectPath_.parent_path());
}

Status EngineApp::startEngine(std::string_view backend, std::uint32_t block, bool anticipative) {
    if (engine_)
        return Error("engine.running", "the engine is already running", {},
                     "engine.stop first to change the backend or block size");
    BackendConfig config;
    config.sampleRate = view().project->sampleRate;
    config.blockSize = block;
    if (backend == "jack") {
        JackOptions o;
        o.clientName = std::string(product::kSlug);
        o.library = options_.jackLibrary;
        ownedBackend_ = std::make_unique<JackBackend>(o);
    } else if (backend == "dummy") {
        ownedBackend_ = std::make_unique<DummyBackend>(
            DummyBackend::Options{.mode = DummyBackend::Mode::Thread, .paced = true});
    } else {
        return Error("engine.backend", "unknown backend " + std::string(backend), {},
                     "jack or dummy");
    }
    auto st = startEngine(*ownedBackend_, config, EngineOptions{.anticipative = anticipative});
    if (!st) stopEngine();  // e.g. the project's rate is not the device's
    return st;
}

void EngineApp::stopEngine() {
    if (engine_) {
        engine_->stop();
        engine_->service();
        engine_.reset();
    }
    backend_ = nullptr;
    ownedBackend_.reset();
}

EngineStatus EngineApp::engineStatus() const {
    EngineStatus st;
    if (!engine_ || !backend_) return st;
    const BackendStatus b = backend_->status();
    st.running = true;
    st.backend = backend_->name();
    st.sampleRate = engine_->config().sampleRate;
    st.block = engine_->config().blockSize;
    st.period = b.period;
    st.planLatency = engine_->outputLatency();
    st.deviceLatency = b.outputLatency;
    st.xruns = static_cast<std::int64_t>(b.xruns);
    st.cycles = static_cast<std::int64_t>(engine_->cycles());
    st.processErrors = static_cast<std::int64_t>(engine_->processErrors());
    st.restartNeeded = b.restartNeeded;
    st.anticipative = engine_->options().anticipative;
    st.anticipationMisses = static_cast<std::int64_t>(engine_->anticipationMisses());
    return st;
}

Result<TransportInfo> EngineApp::transport(std::string_view action, std::optional<double> seconds) {
    if (!engine_)
        return Error("engine.stopped", "the real-time engine is not running", {},
                     "engine.start first");
    if (action == "play") {
        playStart_ = engine_->transport().seconds;
        engine_->play();
    } else if (action == "pause") {
        engine_->pause();
    } else if (action == "stop") {
        engine_->pause();
        engine_->seek(playStart_);
    } else if (action == "seek") {
        engine_->seek(seconds.value_or(0.0));
        if (!engine_->transport().playing) playStart_ = seconds.value_or(0.0);
    }
    if (action != "get") engine_->awaitTransport();
    engine_->service();
    const TransportState t = engine_->transport();
    return TransportInfo{t.playing, t.seconds, view().timeMap->beatAt(t.seconds), t.sample};
}

void EngineApp::headChanged() {
    if (!engine_) return;
    auto st = engine_->load(head(), projectPath_.empty() ? fs::path() : projectPath_.parent_path());
    if (!st) emit("engine.error", json::Value(st.error().toString()));
    engine_->service();
}

// --- cmd::Host ------------------------------------------------------------------------------

cmd::Context EngineApp::makeContext(const cmd::Origin& origin) {
    cmd::Context ctx(origin);
    ctx.provide<EngineApp>(*this);
    return ctx;
}

Result<std::uint64_t> EngineApp::beginTransaction(const cmd::Origin& origin,
                                                  std::string_view label) {
    if (txn_) return Error("history.nested", "a transaction is already open");
    txn_ = std::make_unique<model::EditTxn>(head(), versions_->nextVersion());
    record_ = model::TxnRecord{std::string(label), originText(origin), {}, {}};
    return ++txnId_;
}

Status EngineApp::commitTransaction(std::uint64_t id) {
    if (!txn_ || id != txnId_) return Error("history.transaction", "no such open transaction");
    std::unique_ptr<model::EditTxn> t = std::move(txn_);
    auto changes = t->changes();
    if (changes.empty() && !t->projectChanged() && t->created().empty())
        return okStatus();  // nothing to record
    auto created = t->created();
    auto s = t->commit();
    if (!s) return std::move(s).error();
    auto v = versions_->commit(std::move(*s), std::move(record_), std::move(changes),
                               std::move(created), clockMs());
    if (!v) return std::move(v).error();
    json::Value ev = json::Value::object();
    ev["version"] = *v;
    ev["branch"] = versions_->branch();
    emit("session.changed", std::move(ev));
    headChanged();
    return okStatus();
}

void EngineApp::rollbackTransaction(std::uint64_t id) {
    if (id == txnId_) txn_.reset();
}

void EngineApp::log(const cmd::LogRecord& record) { records_.push_back(record); }

void EngineApp::emit(std::string_view topic, json::Value payload) {
    events_.emplace_back(std::string(topic), std::move(payload));
}

std::vector<std::pair<std::string, json::Value>> EngineApp::takeEvents() {
    return std::exchange(events_, {});
}

bool EngineApp::actionTransactional(std::string_view command) const {
    return command == "render.mix";  // writes a file, changes no state
}

std::uint64_t EngineApp::clockMs() const {
    if (fixedClock_) return fixedClock_;
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::system_clock::now().time_since_epoch())
                                          .count());
}

void EngineApp::invoked(std::string_view command, int, const json::Value& args, cmd::Kind kind) {
    if (txn_ && kind == cmd::Kind::Edit)
        record_.invocations.push_back({std::string(command), args});
}

void EngineApp::setupFixture(cmd::Dispatcher& d, std::string_view fixture) {
    if (fixture == "empty") return;
    if (fixture == "engine_running") {  // the dummy backend: no device
        auto st = startEngine("dummy", 256);
        assert(st && "fixture engine failed");
        (void)st;
        return;
    }
    if (fixture == "job_running") {
        (void)d.jobs().create();
        return;
    }
    auto run = [&](const char* command, std::string_view args) {
        auto r = d.execute(
            {command, json::parse(args).value(), {cmd::Origin::Type::Test, "fixture", ""}});
        assert(r && "fixture command failed");
        (void)r;
    };
    if (fixture == "saved") {
        run("project.set", R"({"name": "Saved"})");
        (void)save("example-project.json");
        return;
    }
    run("track.add", R"({"name": "Bass"})");
    if (fixture == "song") return;
    if (fixture == "branch_ahead") {  // an agent branch one edit ahead of main, main current
        run("branch.create", R"({"name": "agent/claude/darker"})");
        run("track.set", R"({"track": "tr_1", "volume_db": -3})");
        run("branch.switch", R"({"name": "main"})");
        return;
    }
    if (fixture == "song_undone") {
        run("track.set", R"({"track": "tr_1", "volume_db": -3})");
        (void)versions_->undo();
        return;
    }
    run("item.add",
        R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: C2:q r:q C2:q r:q"})");
    if (fixture == "song_with_item") return;
    run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.gain"})");
}

}  // namespace supple::engine
