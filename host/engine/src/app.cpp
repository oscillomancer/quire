#include "supple/engine/app.h"
#include "supple/model/numeric.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <numbers>
#include <string_view>
#include <thread>

#include "supple/audio/audio_file.h"
#include "supple/audio/wav_stream.h"
#include "supple/base/closest.h"
#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/clap/semantic_params.h"
#include "supple/engine/jack.h"
#include "supple/model/env_shape.h"
#include "supple/model/schemas.h"
#include "supple/product.h"

#include "meters.h"
#include "recording.h"

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
    static constexpr const char* kTypes[] = {"ui",   "lua",      "mcp",  "rpc",
                                             "test", "internal", "agent"};
    std::string s = kTypes[static_cast<int>(o.type)];
    if (!o.client.empty()) s += ":" + o.client;
    if (!o.agent.empty()) s += "/" + o.agent;
    return s;
}
}  // namespace

EngineApp::EngineApp(AppOptions options) : options_(std::move(options)) {
    // The scripting runtime exists from the start (its commands are attached
    // later, when the dispatcher does); a script that only computes runs either
    // way, and the registry's examples exercise exactly that.
    scripts_ = std::make_unique<lua::Runtime>();
}

EngineApp::~EngineApp() {
    stopJobs();
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
    SUPPLE_TRY(app->newProject(std::move(project)));
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
        host::orderCatalogue(*list);  // the first location on the path wins an id
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

Result<std::vector<host::ParamInfo>> EngineApp::fxParams(const model::FxInstance& fx) {
    auto in = scratch(fx);
    if (!in) return std::move(in).error();
    return (*in)->params();
}

Result<std::uint32_t> EngineApp::fxSidechainChannels(const model::FxInstance& fx) {
    auto in = scratch(fx);
    if (!in) return std::move(in).error();
    return (*in)->sidechainChannels();
}

namespace {
Status restoreFx(host::Instance& instance, const model::FxInstance& fx, const model::Session& s) {
    if (fx.patch)
        SUPPLE_TRY(instance.setPatch(json::writeCompact(*fx.patch), CLAP_SUPPLE_SET_REPLACE));
    else if (fx.state) {
        const auto blob = s.blobs.find(*fx.state);
        if (blob == s.blobs.end())
            return Error("fx.state", "the state blob is missing", "/fx/state");
        SUPPLE_TRY(instance.loadState(blob->second->data));
    }
    for (const auto& [id, value] : fx.params) SUPPLE_TRY(instance.setParam(id, value));
    return {};
}
}  // namespace

Result<json::Value> EngineApp::setPlainParams(model::Id id, const json::Value& values) {
    if (!values.isObject())
        return Error("fx.params", "values must be an object", "/values",
                     "map parameter names or ids to numbers or plugin text");
    const auto s = view();
    const auto* fx = s.fx.find(id);
    if (!fx) return Error("fx.unknown", "the plugin instance is missing", "/fx");
    auto info = findPlugin(fx->plugin.id);
    if (!info) return info.error();
    auto made = host::Instance::create(*info, options_.identity);
    if (!made) return made.error();
    auto& instance = **made;
    SUPPLE_TRY(restoreFx(instance, *fx, s));
    const auto params = instance.params();
    std::map<std::uint32_t, double> pending;
    json::Value changed = json::Value::object();
    for (const auto& entry : values.asObject()) {
        std::uint32_t numeric = 0;
        const auto parsed =
            std::from_chars(entry.key.data(), entry.key.data() + entry.key.size(), numeric);
        const bool byId =
            parsed.ec == std::errc{} && parsed.ptr == entry.key.data() + entry.key.size();
        const host::ParamInfo* found = nullptr;
        for (const auto& param : params)
            if ((byId && param.id == numeric) || (!byId && iequals(param.name, entry.key))) {
                if (found)
                    return Error("fx.param_ambiguous", "parameter name is ambiguous",
                                 "/values/" + entry.key,
                                 "use the numeric id from fx.describe full");
                found = &param;
            }
        const auto path = "/values/" + entry.key;
        if (!found)
            return Error("fx.param_unknown", "unknown parameter " + entry.key, path,
                         "use a parameter name or id from fx.describe full");
        if (found->flags & CLAP_PARAM_IS_READONLY)
            return Error("fx.param_readonly", "parameter is read-only", path,
                         "choose a writable parameter");
        double value = 0;
        if (entry.value.isNumber())
            value = entry.value.asDouble();
        else if (entry.value.isString()) {
            auto parsedText = instance.paramFromText(found->id, entry.value.asString());
            if (!parsedText) return std::move(parsedText).error().at(path);
            value = *parsedText;
        } else
            return Error("fx.param_value", "value must be a number or plugin text", path);
        if (!std::isfinite(value) || value < found->min || value > found->max)
            return Error("fx.param_range", "parameter value is outside its declared range", path,
                         "use " + std::to_string(found->min) + ".." + std::to_string(found->max));
        if (found->flags & CLAP_PARAM_IS_STEPPED) value = std::round(value);
        if (pending.contains(found->id))
            return Error("fx.param_duplicate", "parameter appears twice", path,
                         "use one name or id per parameter");
        pending[found->id] = value;
        changed[entry.key] = value;
    }
    for (const auto& [param, value] : pending) SUPPLE_TRY(instance.setParam(param, value));
    // VST3 controllers enqueue processor changes: consume them on this private
    // instance before saving the component state. No device is opened.
    SUPPLE_TRY(instance.activate(s.project->sampleRate, 1, 1));
    if (!instance.startProcessing()) return Error("fx.process", "plugin refused the state probe");
    float left = 0, right = 0;
    const float* inputs[]{&left, &right};
    float* outputs[]{&left, &right};
    const bool processed = instance.process({.inputs = inputs,
                                             .inputChannels = 2,
                                             .outputs = outputs,
                                             .outputChannels = 2,
                                             .frames = 1});
    instance.stopProcessing();
    instance.deactivate();
    if (!processed) return Error("fx.process", "plugin failed the state probe");
    auto state = instance.saveState();
    if (!state) return state.error();
    const auto hash = "sha256:" + sha256Hex(*state);
    txn().putBlob(hash, std::make_shared<const model::Blob>(model::Blob{std::move(*state)}));
    auto next = *fx;
    next.state = hash;
    for (const auto& [param, value] : pending) next.params[param] = value;
    txn().put<model::FxInstance>(id, std::move(next));
    return changed;
}

Result<std::uint32_t> EngineApp::fxLatencySamples(model::Id id, const model::FxInstance& fx) {
    // A running instance already knows the plan's rate and block. Otherwise
    // activate a throwaway instance at the project rate: a plugin reports its
    // latency in samples, so a rate is needed even when nothing is playing.
    if (host::Instance* live = instanceOf(id); live && live->active()) return live->latency();
    auto info = findPlugin(fx.plugin.id);
    if (!info) return std::move(info).error();
    auto made = host::Instance::create(*info, options_.identity);
    if (!made) return std::move(made).error();
    host::Instance& probe = **made;
    SUPPLE_TRY(restoreFx(probe, fx, view()));
    constexpr std::uint32_t kProbeBlock = 512;  // OfflineSettings' default block
    SUPPLE_TRY(probe.activate(head().project->sampleRate, 1, kProbeBlock));
    if (probe.paramsNormalized() && !fx.params.empty()) {
        if (!probe.startProcessing())
            return Error("fx.process", "plugin refused the latency probe");
        float l = 0, r = 0;
        const float* inputs[]{&l, &r};
        float* outputs[]{&l, &r};
        const bool processed = probe.process({.inputs = inputs,
                                              .inputChannels = 2,
                                              .outputs = outputs,
                                              .outputChannels = 2,
                                              .frames = 1});
        probe.stopProcessing();
        if (!processed) return Error("fx.process", "plugin failed the latency probe");
    }
    const std::uint32_t latency = probe.latency();
    probe.deactivate();
    return latency;
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
    auto patch = mergedPatch(fx, json::Value::object(), false);
    if (!patch) return std::move(patch).error();
    return in.describe(changedOnly);
}

Result<model::Id> copyTake(const model::Session& source, model::EditTxn& txn, model::Id original,
                           model::Take copy, double shift) {
    std::map<model::Id, model::Id> ids;
    copy.chain.clear();
    const auto* old = source.takes.find(original);
    if (!old) return Error("item.take", "the copied take is missing", "/take");
    for (model::Id id : old->chain) {
        const auto* fx = source.fx.find(id);
        if (!fx) return Error("item.fx", "the copied take FX is missing", "/take");
        const auto fresh = txn.create(*fx);
        ids[id] = fresh;
        copy.chain.push_back(fresh);
    }
    const auto fresh = txn.create(std::move(copy));
    ids[original] = fresh;
    for (const auto& e : source.envelopes) {
        const auto to = ids.find(e.value->target.object);
        if (to == ids.end()) continue;
        model::Envelope env = *e.value;
        env.target.object = to->second;
        const bool relative = to->first == original;
        if (relative && shift != 0 && !env.points.empty()) {
            const auto& points = e.value->points;
            auto boundary = points.front();
            for (std::size_t i = 0; i + 1 < points.size(); ++i)
                if (points[i].pos <= shift && shift < points[i + 1].pos) {
                    boundary = points[i];
                    const double t = (shift - points[i].pos) / (points[i + 1].pos - points[i].pos);
                    boundary.value += (points[i + 1].value - points[i].value) *
                                      model::shapeCurve(boundary.shape, boundary.tension, t);
                }
            if (shift >= points.back().pos) boundary = points.back();
            boundary.pos = 0;
            env.points = {boundary};
            for (const auto& point : points)
                if (point.pos > shift) {
                    auto right = point;
                    right.pos -= shift;
                    env.points.push_back(right);
                }
            auto left = points;
            boundary.pos = shift;
            std::erase_if(left, [&](const auto& point) { return point.pos >= shift; });
            left.push_back(boundary);
            txn.modify<model::Envelope>(e.id, [&](auto& lane) { lane.points = left; });
        }
        const auto lane = txn.create(std::move(env));
        for (const auto& ai : source.autoItems)
            if (ai.value->envelope == e.id) {
                auto item = *ai.value;
                item.envelope = lane;
                if (relative) item.position -= shift;
                txn.create(std::move(item));
            }
    }
    return fresh;
}

// --- project files ----------------------------------------------------------------------

Status EngineApp::replaceProject(model::Session session, fs::path path) {
    Backend* running = engine_ ? backend_ : nullptr;
    const auto config = engine_ ? engine_->config() : BackendConfig{};
    const auto settings = engine_ ? engine_->options() : EngineOptions{};
    if (engine_) {
        auto stopped = transport("stop");
        if (!stopped) return stopped.error();
    }
    projectGeneration_.fetch_add(1, std::memory_order_acq_rel);
    if (commands_) {
        const auto jobs = commands_->jobs().list();
        for (const auto& job : jobs.find("jobs")->asArray())
            if (*job.find("status") == "running")
                SUPPLE_TRY(commands_->jobs().cancel(job.find("job")->asString()));
    }
    stopJobs();
    if (engine_) {
        engine_->stop();
        engine_.reset();
        backend_ = nullptr;
    }
    if (versions_ && projectPath_.empty()) clearScratch();
    loadedHead_.reset();
    media_ = MediaCache{};
    scratch_.clear();
    reportedFailures_.clear();
    recordingFiles_.clear();
    autoWrite_.clear();
    renderQueue_.clear();
    {
        std::lock_guard lock(rendersMutex_);
        renders_.clear();
    }
    pool_ = std::make_unique<InstancePool>(
        [this](const model::PluginRef& ref) { return findPlugin(ref.id); }, options_.identity);
    if (scripts_) {
        clearScripts_ = scripts_->active();
        if (!clearScripts_) scripts_->clear();
    }
    versions_ = std::make_unique<model::VersionStore>(std::move(session));
    projectPath_ = std::move(path);
    autosaveMarked_ = !projectPath_.empty();
    autosaveMs_ = clockMs();
    autosaveVersion_ = head().version;
    if (running) SUPPLE_TRY(startEngine(*running, config, settings));
    headChanged();
    return {};
}

Status EngineApp::newProject(model::Project project) {
    return replaceProject(model::Session::create(std::move(project)), {});
}

fs::path EngineApp::resolve(const fs::path& path) const {
    return path.is_relative() ? options_.workdir / path : path;
}

Status EngineApp::save(const fs::path& where) {
    const fs::path path = resolve(where);
    SUPPLE_TRY(model::checkInvariants(head()));
    const bool fromScratch = projectPath_.empty() && !options_.scratchDir.empty();
    if (fromScratch && engine_ && engine_->recording())
        return Error("project.recording", "a recording is running", {},
                     "stop it before the project's first save (its files move then)");
    auto publish = [&] { return writeFileAtomic(path, model::serializeProject(head())); };
    if (fromScratch)
        SUPPLE_TRY(moveScratchMedia(path.parent_path() / head().project->mediaDir, publish));
    else
        SUPPLE_TRY(publish());
    projectPath_ = path;
    autosaveMarked_ = true;  // the file itself is the newest copy
    autosaveMs_ = clockMs();
    autosaveVersion_ = head().version;
    return okStatus();
}

Status EngineApp::open(const fs::path& where, std::vector<schema::Warning>* warnings) {
    const fs::path path = resolve(where);
    auto text = readTextFile(path, std::size_t(1) << 30);
    if (!text) return std::move(text).error();
    auto s = model::parseProject(*text, warnings);
    if (!s) return std::move(s).error();
    return replaceProject(std::move(*s), path);
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
    auto validation = model::validateStripRules(txn_->preview());
    if (!validation) {
        restore();
        return std::move(validation).error();
    }
    auto s = txn_->commit();
    restore();
    if (!s) return std::move(s).error();
    out.session = std::move(*s);
    return out;
}

Status EngineApp::undo() {
    SUPPLE_TRY(versions_->undo());
    headChanged();
    json::Value ev = json::Value::object();
    ev["version"] = head().version;
    ev["branch"] = versions_->branch();
    emit("session.changed", std::move(ev));
    return {};
}

Status EngineApp::redo() {
    SUPPLE_TRY(versions_->redo());
    headChanged();
    json::Value ev = json::Value::object();
    ev["version"] = head().version;
    ev["branch"] = versions_->branch();
    emit("session.changed", std::move(ev));
    return {};
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
    if (!s.media) s.media = &media_;
    return renderSession(view(), s);
}

Result<OfflineResult> EngineApp::renderSession(const model::Session& session,
                                               const OfflineSettings& settings) {
    OfflineSettings s = settings;
    if (s.projectDir.empty()) s.projectDir = projectDir();
    MediaCache localMedia;
    if (!s.media) s.media = &localMedia;
    // Offline renders use their own plugin instances (engine.md §3): playback
    // is never disturbed and state is deterministic.
    InstancePool offlinePool([this](const model::PluginRef& ref) { return findPlugin(ref.id); },
                             options_.identity);
    return renderOffline(session, offlinePool, s);
}

std::string EngineApp::rememberRender(const fs::path& path) {
    const std::string id = reserveRenderId();
    publishRender(id, path);
    return id;
}

std::string EngineApp::reserveRenderId() {
    std::lock_guard lock(rendersMutex_);
    return "rd_" + std::to_string(nextRender_++);
}

void EngineApp::publishRender(std::string id, const fs::path& path) {
    std::lock_guard lock(rendersMutex_);
    renders_[std::move(id)] = path;
}

Result<fs::path> EngineApp::renderPath(std::string_view reference) const {
    if (reference.starts_with("rd_")) {
        std::lock_guard lock(rendersMutex_);
        auto it = renders_.find(std::string(reference));
        if (it == renders_.end())
            return Error("engine.unknown_render", "no such render " + std::string(reference), {},
                         "render.mix returns a render id; use it or a WAV path");
        return it->second;
    }
    return resolve(fs::path(reference));
}

void EngineApp::startJob(std::function<void(std::stop_token)> work) {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->finished->load(std::memory_order_acquire))
            it = jobs_.erase(it);
        else
            ++it;
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    jobs_.push_back({std::jthread([hook = options_.jobStartedHook, work = std::move(work),
                                   finished](std::stop_token stop) {
                         if (hook) hook();
                         work(stop);
                         finished->store(true, std::memory_order_release);
                     }),
                     finished});
}

void EngineApp::stopJobs() {
    for (auto& job : jobs_) job.thread.request_stop();
    jobs_.clear();  // jthread joins before the command bus destroys JobManager
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
    model::Session atRate = head();
    auto project = *atRate.project;
    project.sampleRate = engine_->config().sampleRate;
    atRate.project = std::make_shared<const model::Project>(std::move(project));
    auto numeric = model::validateNumeric(atRate);
    if (numeric)
        for (const auto& range : {loop_, punch_})
            if (range.end > range.start) {
                numeric = model::checkedSeconds(atRate, range.end, "/end_seconds");
                if (!numeric) break;
            }
    if (!numeric) {
        engine_->stop();
        engine_.reset();
        return numeric.error();
    }
    backend_ = &backend;
    restartBackend_ = &backend;  // a caller-supplied backend: reuse it
    restartConfig_ = engine_->config();
    restartOptions_ = options;
    restarting_ = false;
    playStart_ = 0.0;
    if (loop_.enabled || loop_.end > loop_.start)
        engine_->setLoop(loop_.start, loop_.end, loop_.enabled);
    engine_->setMetronome(metronome_.enabled, metronome_.volumeDb);
    auto loaded = engine_->load(head(), projectDir());
    if (loaded) loadedHead_ = versions_->head();
    return loaded;
}

Status EngineApp::startEngine(std::string_view backend, std::uint32_t block, bool anticipative) {
    if (engine_)
        return Error("engine.running", "the engine is already running", {},
                     "engine.stop first to change the backend or block size");
    BackendConfig config;
    config.sampleRate = view().project->sampleRate;
    config.blockSize = block;
    config.inputs = 2;         // at least the device's first stereo pair, for recording
    config.midiInputs = true;  // its MIDI inputs, for MIDI tracks (ADR-0016)
    if (backend == "jack") {
        JackOptions o;
        o.clientName = std::string(product::kSlug);
        o.library = options_.jackLibrary;
        // One input per physical capture port (at least the first pair, at
        // most 32), so any device input can feed a track.
        if (auto names = JackBackend::captureNames(o); names && !names->empty())
            config.inputs =
                static_cast<std::uint32_t>(std::clamp<std::size_t>(names->size(), 2, 32));
        ownedBackend_ = std::make_unique<JackBackend>(o);
    } else if (backend == "dummy") {
        ownedBackend_ = std::make_unique<DummyBackend>(
            DummyBackend::Options{.mode = DummyBackend::Mode::Thread, .paced = true});
    } else {
        return Error("engine.backend", "unknown backend " + std::string(backend), {},
                     "jack or dummy");
    }
    auto st = startEngine(*ownedBackend_, config, EngineOptions{.anticipative = anticipative});
    if (!st) {
        haltEngine();  // e.g. the project's rate is not the device's
        return st;
    }
    // The app owns this backend: a restart recreates it (the instance may be
    // dead with the device).
    engineKind_ = std::string(backend);
    engineBlock_ = block;
    engineAnticipative_ = anticipative;
    restartBackend_ = nullptr;
    return st;
}

void EngineApp::haltEngine() {
    if (engine_) {
        engine_->stop();
        engine_->service();
        engine_.reset();
        pool_->prune(head());  // nothing processes now: replaced instances can go
    }
    backend_ = nullptr;
    ownedBackend_.reset();
}

void EngineApp::stopEngine() {
    restarting_ = false;  // an explicit stop cancels a pending restart
    haltEngine();
}

// --- the periodic service tick ------------------------------------------------------------

namespace {
constexpr std::uint64_t kAutosaveIntervalMs = 60'000;
constexpr int kAutosaveKeep = 5;
constexpr std::uint64_t kRestartGiveUpMs = 30'000;
}  // namespace

void EngineApp::tick() {
    if (clearScripts_ && scripts_ && !scripts_->active()) {
        scripts_->clear();
        clearScripts_ = false;
    }
    if (engine_) {
        engine_->service();
        // Failures reach subscribers without a client asking.
        checkFailures();
        if (backend_) {
            const BackendStatus b = backend_->status();
            if (b.restartNeeded)
                beginRestart(b.restartReason.empty() ? std::string("the device changed")
                                                     : std::string(b.restartReason),
                             engine_->transport().seconds);
        }
    } else if (restarting_) {
        retryRestart();
    }
    maybeAutosave();
    // What the tick emitted (fx.failed, engine.restarting, …) reaches scripts
    // now, not after some later command.
    if (scripts_ && !clearScripts_) scripts_->deliver();
}

void EngineApp::maybeAutosave() {
    if (projectPath_.empty()) return;
    const std::uint64_t now = clockMs();
    if (!autosaveMarked_) {
        autosaveMarked_ = true;
        autosaveMs_ = now;
        autosaveVersion_ = head().version;
        return;
    }
    // Autosave when the head changed and an interval passed since the last one.
    if (head().version == autosaveVersion_ || now < autosaveMs_ + kAutosaveIntervalMs) return;
    autosave();
}

void EngineApp::beginRestart(std::string reason, double seconds) {
    restartReason_ = std::move(reason);
    restartSeconds_ = seconds;
    restartBackoff_ = 0;
    restartUntil_ = clockMs() + kRestartGiveUpMs;
    restartRetryAt_ = clockMs();
    json::Value ev = json::Value::object();
    ev["reason"] = restartReason_;
    emit("engine.restarting", std::move(ev));
    // Playback stays stopped; the position and the loop are restored.
    haltEngine();
    restarting_ = true;
}

void EngineApp::retryRestart() {
    const std::uint64_t now = clockMs();
    if (now < restartRetryAt_) return;
    Status st = okStatus();
    if (!engineKind_.empty())
        st = startEngine(engineKind_, engineBlock_, engineAnticipative_);
    else if (restartBackend_)
        st = startEngine(*restartBackend_, restartConfig_, restartOptions_);
    else
        st = Error("engine.restart", "there is no backend to restart");
    if (st) {
        restarting_ = false;
        if (engine_) {
            engine_->seek(restartSeconds_);  // the transport position is restored
            engine_->awaitTransport();       // ... before the tick returns
        }
        return;
    }
    if (now >= restartUntil_) {
        restarting_ = false;
        json::Value ev = json::Value::object();
        ev["reason"] = restartReason_;
        ev["error"] = st.error().toString();
        emit("engine.error", std::move(ev));
        return;
    }
    restartBackoff_ =
        restartBackoff_ == 0 ? 250 : std::min<std::uint64_t>(restartBackoff_ * 2, 4000);
    restartRetryAt_ = now + restartBackoff_;
}

fs::path EngineApp::autosaveDir() const {
    if (projectPath_.empty()) return {};
    return projectPath_.parent_path() / (projectPath_.filename().string() + "-data");
}

fs::path EngineApp::newerAutosave(const fs::path& project) const {
    if (project.empty()) return {};
    std::error_code ec;
    const fs::path dir = project.parent_path() / (project.filename().string() + "-data");
    if (!fs::is_directory(dir, ec)) return {};
    const auto written = fs::last_write_time(project, ec);
    if (ec) return {};
    fs::path best;
    fs::file_time_type bestTime{};
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const fs::path& candidate = it->path();
        const std::string name = candidate.filename().string();
        if (!std::string_view(name).starts_with("autosave-") || candidate.extension() != ".json")
            continue;
        if (!fs::is_regular_file(candidate, ec)) continue;
        const auto time = fs::last_write_time(candidate, ec);
        if (ec) break;
        if (time > written && (best.empty() || time > bestTime)) {
            best = candidate;
            bestTime = time;
        }
    }
    return best;
}

void EngineApp::autosave() {
    const fs::path dir = autosaveDir();
    if (dir.empty()) return;
    // Rotate: the next free index, then the oldest files over the keep count go.
    int next = 1;
    std::vector<std::pair<int, fs::path>> existing;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (!std::string_view(name).starts_with("autosave-") || it->path().extension() != ".json")
            continue;
        const int n = std::atoi(name.c_str() + 9);
        if (n < 1) continue;
        next = std::max(next, n + 1);
        existing.push_back({n, it->path()});
    }
    const fs::path file = dir / ("autosave-" + std::to_string(next) + ".json");
    if (auto st = writeFileAtomic(file, model::serializeProject(head())); !st) {
        emit("engine.error", json::Value(st.error().toString()));
        autosaveMs_ = clockMs();  // try again after an interval, not every tick
        return;
    }
    existing.push_back({next, file});
    std::sort(existing.begin(), existing.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    while (static_cast<int>(existing.size()) > kAutosaveKeep) {
        std::error_code ignored;
        fs::remove(existing.front().second, ignored);
        existing.erase(existing.begin());
    }
    autosaveVersion_ = head().version;
    autosaveMs_ = clockMs();
    autosaveMarked_ = true;
}

void EngineApp::checkFailures() {
    for (const auto& [id, why] : pool_->failed()) {
        if (!reportedFailures_.insert(id).second) continue;
        json::Value payload = json::Value::object();
        payload["fx"] = id.str();
        payload["reason"] = why;
        emit("fx.failed", std::move(payload));
    }
}

void EngineApp::restartFx(model::Id fx) {
    pool_->restart(fx);
    reportedFailures_.erase(fx);
    headChanged();  // a running engine gets the new instance in its next plan
}

EngineStatus EngineApp::engineStatus() {
    checkFailures();
    EngineStatus st;
    for (const auto& [id, why] : pool_->failed()) st.failedFx.push_back(id.str());
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
    if (const std::shared_ptr<MeterBank> bank = engine_->meterBank()) {
        st.meterDrops.renderer = static_cast<std::int64_t>(bank->droppedRenderer());
        st.meterDrops.callback = static_cast<std::int64_t>(bank->droppedCallback());
    }
    return st;
}

std::vector<MeterReading> EngineApp::meters() {
    if (!engine_) return {};
    const std::shared_ptr<MeterBank> bank = engine_->meterBank();
    if (!bank) return {};
    const Engine::MeterClock clock = engine_->meterClock();
    return bank->read(clock.epoch, clock.heardStream);
}

Result<TransportInfo> EngineApp::transport(std::string_view action, std::optional<double> seconds) {
    if (!engine_)
        return Error("engine.stopped", "the real-time engine is not running", {},
                     "engine.start first");
    std::optional<std::vector<RecordedInfo>> recorded;
    if (action == "record") {
        const model::Session s = view();
        const fs::path dir = recordDir();
        std::vector<RecordTarget> targets;
        for (const auto& e : s.tracks) {
            const model::Track& tr = *e.value;
            if (!tr.record.armed || !tr.record.input) continue;
            const model::Route* in = s.routes.find(*tr.record.input);
            if (in && in->from.midiDevice) {  // MIDI (ADR-0016): events, no file
                RecordTarget t;
                t.track = e.id;
                t.midi = RecordTarget::Midi{
                    midiPortMask(engine_->midiPorts(), *in->from.midiDevice),
                    in->midi ? in->midi->fromChannel : 0, in->midi ? in->midi->toChannel : 0};
                targets.push_back(std::move(t));
                continue;
            }
            if (!in || !in->from.hwChannels) continue;
            RecordTarget t;
            t.track = e.id;
            for (int ch : *in->from.hwChannels) t.inputs.push_back(ch - 1);
            std::string base;
            for (char c : tr.name.empty() ? e.id.str() : tr.name)
                base += std::isalnum(static_cast<unsigned char>(c)) ? c : '-';
            for (int n = 1;; ++n) {  // (the writer also creates it exclusively)
                t.file = dir / (base + "-" + std::to_string(n) + ".wav");
                std::error_code ec;
                const bool taken =
                    std::any_of(targets.begin(), targets.end(),
                                [&](const RecordTarget& o) { return o.file == t.file; });
                if (!taken && !fs::exists(t.file, ec) &&
                    !fs::exists(detail::sidecarPath(t.file), ec))
                    break;
            }
            targets.push_back(std::move(t));
        }
        std::vector<fs::path> files;
        for (const RecordTarget& t : targets)
            if (!t.midi) files.push_back(t.file);
        SUPPLE_TRY(engine_->startRecording(std::move(targets)));
        recordingFiles_ = std::move(files);
        playStart_ = engine_->transport().seconds;
        startPlaying(metronome_.countInBars);
    } else if (action == "play") {
        playStart_ = engine_->transport().seconds;
        beginAutomationPass(view().timeMap->beatAt(playStart_));
        startPlaying(metronome_.countInPlayback ? metronome_.countInBars : 0);
    } else if (action == "pause" || action == "stop") {
        const double stopBeat = view().timeMap->beatAt(engine_->transport().seconds);
        const bool wasPlaying = engine_->transport().playing;
        engine_->pause();
        if (action == "stop") engine_->seek(playStart_);
        if (engine_->recording()) {
            auto takes = engine_->stopRecording();
            if (!takes) return std::move(takes).error();  // record.busy: still recording
            recordingFiles_.clear();
            auto committed = commitTakes(*takes);
            if (!committed) return std::move(committed).error();
            recorded = std::move(*committed);
        }
        if (wasPlaying) SUPPLE_TRY(finishAutomationPass(stopBeat));
    } else if (action == "seek") {
        engine_->seek(seconds.value_or(0.0));
        if (!engine_->transport().playing) playStart_ = seconds.value_or(0.0);
    }
    if (action != "get") engine_->awaitTransport();
    engine_->service();
    checkFailures();
    const TransportState t = engine_->transport();
    TransportInfo info;
    info.playing = t.playing;
    info.recording = engine_->recording();
    info.countingIn = t.countingIn;
    info.seconds = t.seconds;
    info.beat = view().timeMap->beatAt(t.seconds);
    info.sample = t.sample;
    info.loop = loop_;
    info.punch = punch_;
    info.recorded = std::move(recorded);
    return info;
}

namespace {

bool writingMode(model::EnvMode m) {
    return m == model::EnvMode::Touch || m == model::EnvMode::Latch ||
           m == model::EnvMode::LatchPreview || m == model::EnvMode::Write;
}

// The envelope's value at a beat with the engine's segment shapes.
double envelopeAt(const std::vector<model::EnvPoint>& pts, double beat, double fallback) {
    if (pts.empty()) return fallback;
    if (beat <= pts.front().pos) return pts.front().value;
    if (beat >= pts.back().pos) return pts.back().value;
    const auto next = std::upper_bound(
        pts.begin(), pts.end(), beat, [](double b, const model::EnvPoint& p) { return b < p.pos; });
    const model::EnvPoint& b = *next;
    const model::EnvPoint& a = *(next - 1);
    if (b.pos <= a.pos) return b.value;
    return a.value + (b.value - a.value) *
                         model::shapeCurve(a.shape, a.tension, (beat - a.pos) / (b.pos - a.pos));
}

void insertPoint(std::vector<model::EnvPoint>& pts, double beat, double value) {
    model::EnvPoint p;
    p.pos = beat;
    p.value = value;
    const auto at = std::upper_bound(pts.begin(), pts.end(), beat,
                                     [](double b, const model::EnvPoint& q) { return b < q.pos; });
    pts.insert(at, p);
}

// A strip's static value for an envelope's parameter (a write pass that never
// moved the control flattens to it).
double staticValue(const model::Session& s, const model::EnvTarget& t) {
    if (const model::Track* tr = s.tracks.find(t.object))
        return t.param == "volume" ? tr->volumeDb
               : t.param == "pan"  ? tr->pan
                                   : (tr->mute ? 1.0 : 0.0);
    if (const model::Route* r = s.routes.find(t.object))
        return t.param == "volume" ? r->volumeDb
               : t.param == "pan"  ? r->pan.percent.value_or(0.0)
                                   : (r->mute ? 1.0 : 0.0);
    return 0.0;
}

}  // namespace

void EngineApp::beginAutomationPass(double beat) {
    autoWrite_.clear();
    const model::Session s = view();
    for (const auto& e : s.envelopes)
        if (e.value->active && e.value->mode == model::EnvMode::Write &&
            e.value->target.object.kind != model::Kind::Fx) {
            AutoWrite w;
            w.startBeat = w.lastBeat = beat;
            w.lastValue = staticValue(s, e.value->target);
            autoWrite_[e.id] = w;
        }
}

bool EngineApp::writesAutomation(model::Id object, const std::string& param) const {
    if (!engine_ || !txn_ || !engine_->transport().playing) return false;
    const model::Session s = view();
    for (const auto& e : s.envelopes)
        if (e.value->target.object == object && e.value->target.param == param && e.value->active &&
            writingMode(e.value->mode))
            return true;
    return false;
}

void EngineApp::recordAutomation(model::Id object, const std::string& param, double value) {
    if (!engine_ || !txn_ || !engine_->transport().playing) return;
    const model::Session s = view();
    std::optional<model::Id> envId;
    for (const auto& e : s.envelopes)
        if (e.value->target.object == object && e.value->target.param == param && e.value->active &&
            writingMode(e.value->mode))
            envId = e.id;
    if (!envId) return;
    const double beat = s.timeMap->beatAt(engine_->transport().seconds);
    auto it = autoWrite_.find(*envId);
    const bool first = it == autoWrite_.end() || !it->second.touched;
    // A loop pass wraps: the erase window restarts at the write itself.
    const double from = !first && it->second.lastBeat < beat ? it->second.lastBeat : beat;
    const double startBeat = it == autoWrite_.end() ? beat : it->second.startBeat;
    txn().modify<model::Envelope>(*envId, [&](model::Envelope& env) {
        const double before = envelopeAt(env.points, beat, staticValue(s, env.target));
        std::erase_if(env.points, [&](const model::EnvPoint& p) {
            return first ? std::abs(p.pos - beat) < 1e-9 : p.pos > from && p.pos <= beat + 1e-9;
        });
        // Touching in: the envelope holds its value up to here, then jumps.
        if (first && beat > 1e-3) insertPoint(env.points, beat - 1e-3, before);
        insertPoint(env.points, beat, value);
    });
    AutoWrite w;
    w.startBeat = startBeat;
    w.lastBeat = beat;
    w.lastValue = value;
    w.touched = true;
    autoWrite_[*envId] = w;
}

Status EngineApp::finishAutomationPass(double stopBeat) {
    std::map<model::Id, AutoWrite> pass;
    pass.swap(autoWrite_);
    const model::Session s = head();
    std::vector<std::pair<model::Id, AutoWrite>> work;
    for (const auto& [id, w] : pass) {
        const model::Envelope* env = s.envelopes.find(id);
        if (!env || stopBeat <= w.lastBeat) continue;
        const bool holds = env->mode == model::EnvMode::Latch ||
                           env->mode == model::EnvMode::LatchPreview ||
                           env->mode == model::EnvMode::Write;
        if (holds) work.emplace_back(id, w);
    }
    if (work.empty()) return {};
    auto txnId =
        beginTransaction({cmd::Origin::Type::Internal, "engine", ""}, "Automation written");
    if (!txnId) return std::move(txnId).error();
    for (const auto& [id, w] : work)
        txn().modify<model::Envelope>(id, [&](model::Envelope& env) {
            const double from = w.touched ? w.lastBeat : w.startBeat - 1e-9;
            std::erase_if(env.points, [&](const model::EnvPoint& p) {
                return p.pos > from && p.pos <= stopBeat + 1e-9;
            });
            if (!w.touched) insertPoint(env.points, w.startBeat, w.lastValue);
            insertPoint(env.points, stopBeat, w.lastValue);
        });
    return commitTransaction(*txnId);
}

void EngineApp::startPlaying(int countInBars) {
    if (countInBars <= 0) {
        engine_->play();
        return;
    }
    // Whole bars of the meter at the start position, at its tempo.
    const time::TimeMap& tm = *view().timeMap;
    const double beat = tm.beatAt(engine_->transport().seconds);
    const time::MeterChange m = tm.meterAt(tm.barBeat(beat).bar);
    const double beatSeconds = 4.0 / static_cast<double>(m.denominator) * 60.0 / tm.bpmAt(beat);
    const auto beatFrames =
        static_cast<std::int64_t>(std::llround(beatSeconds * engine_->config().sampleRate));
    engine_->playCountIn(beatFrames * m.numerator * countInBars, beatFrames, m.numerator);
}

void EngineApp::setMetronome(const MetronomeSettings& m) {
    metronome_ = m;
    if (engine_) engine_->setMetronome(m.enabled, m.volumeDb);
}

void EngineApp::setLoop(const TransportRange& loop) {
    loop_ = loop;
    if (engine_) engine_->setLoop(loop.start, loop.end, loop.enabled);
}

namespace {

// A recording's runs on the timeline, and the items and takes they become
// (ADR-0015, ADR-0016).
struct Run {
    std::int64_t from, to, t0;  // file frames [from, to) heard from timeline t0
    std::size_t index;          // into the segments (a MIDI event's run)
};
struct TakePlan {
    std::int64_t fileFrame;  // at the item's start (audio)
    bool whole;              // the run covers the whole item
    std::size_t run;         // into the runs
};
struct ItemPlan {
    std::int64_t start, end;
    std::vector<TakePlan> takes;
};

// Adds run `run`'s recorded MIDI heard in [from, to) to `seq`: notes paired
// from note-on/off (a retrigger ends the previous one; still held at the
// run's end: ended there, or at `to`), other channel messages as controls.
template <class TickOf>
void appendRecorded(midi::Sequence& seq, const RecordedTake& r, std::size_t run, std::int64_t from,
                    std::int64_t to, std::int64_t runEnd, TickOf tick) {
    std::map<std::pair<int, int>, std::pair<std::int64_t, int>> open;
    auto close = [&](std::pair<int, int> key, std::int64_t at, int offVelocity) {
        auto it = open.find(key);
        if (it == open.end()) return;
        const auto [on, velocity] = it->second;
        open.erase(it);
        const auto first = std::max(on, from);
        const auto last = std::min(at, to);
        if (last <= first) return;
        midi::Note n;
        n.start = tick(first);
        n.length = std::max<std::int64_t>(1, tick(last) - n.start);
        n.channel = static_cast<std::uint8_t>(key.first);
        n.pitch = static_cast<std::uint8_t>(key.second);
        n.velocity = static_cast<std::uint8_t>(velocity);
        n.offVelocity = static_cast<std::uint8_t>(offVelocity);
        seq.notes.push_back(n);
    };
    for (const RecordedMidiEvent& ev : r.midi) {
        if (ev.run != run) continue;
        const int status = ev.bytes[0] & 0xf0, ch = ev.bytes[0] & 0x0f;
        const int b1 = ev.bytes[1], b2 = ev.bytes[2];
        if (status == 0x90 && b2 > 0) {
            close({ch, b1}, ev.timeline, 0);
            open[{ch, b1}] = {ev.timeline, b2};
        } else if (status == 0x80 || status == 0x90) {
            close({ch, b1}, ev.timeline, status == 0x80 ? b2 : 0);
        } else if (ev.timeline >= from && ev.timeline < to) {
            midi::Control c;
            c.tick = tick(ev.timeline);
            c.channel = static_cast<std::uint8_t>(ch);
            switch (status) {
                case 0xb0:
                    c.kind = midi::ControlKind::CC;
                    c.number = static_cast<std::uint8_t>(b1);
                    c.value = b2;
                    break;
                case 0xc0:
                    c.kind = midi::ControlKind::ProgramChange;
                    c.value = b1;
                    break;
                case 0xd0:
                    c.kind = midi::ControlKind::ChannelPressure;
                    c.value = b1;
                    break;
                case 0xa0:
                    c.kind = midi::ControlKind::PolyPressure;
                    c.number = static_cast<std::uint8_t>(b1);
                    c.value = b2;
                    break;
                default:  // 0xe0
                    c.kind = midi::ControlKind::PitchBend;
                    c.value = ((b2 << 7) | b1) - 8192;
                    break;
            }
            seq.controls.push_back(c);
        }
    }
    while (!open.empty()) close(open.begin()->first, runEnd, 0);  // still held
}

Status recordingBounds(const model::Session& s, const detail::RecordingInfo& side,
                       std::uint64_t frames, double fileRate = 0) {
    auto invalid = [](std::string path) {
        return Error("record.sidecar", "recording positions exceed the finite timeline range",
                     std::move(path), "restore valid recording positions or skip this file");
    };
    if (!std::isfinite(side.sampleRate) || side.sampleRate < 8000 || side.sampleRate > 384000 ||
        (fileRate > 0 && side.sampleRate != fileRate) || frames > INT64_MAX)
        return invalid("/sample_rate");
    auto run = [&](std::int64_t start, std::uint64_t count, const std::string& path) -> Status {
        const long double end = static_cast<long double>(start) + static_cast<long double>(count);
        if (std::abs(end) > model::kSampleLimit ||
            !model::checkedSamples(
                static_cast<long double>(start) / static_cast<long double>(side.sampleRate),
                side.sampleRate, path))
            return invalid(path);
        // A negative start is valid latency preroll; only its positive end enters the session.
        if (end > 0 && !model::checkedSeconds(
                           s, static_cast<double>(end / static_cast<long double>(side.sampleRate)),
                           path, side.sampleRate))
            return invalid(path);
        return {};
    };
    if (side.segments.empty()) return run(side.startSample, frames, "/start_sample");
    if (!model::checkedSeconds(s, static_cast<double>(side.loopStart) / side.sampleRate, "/loop/0",
                               side.sampleRate) ||
        !model::checkedSeconds(s, static_cast<double>(side.loopEnd) / side.sampleRate, "/loop/1",
                               side.sampleRate))
        return invalid("/loop");
    for (std::size_t i = 0; i < side.segments.size(); ++i) {
        const auto& seg = side.segments[i];
        const auto from = static_cast<std::uint64_t>(seg.fileFrame);
        const auto to =
            i + 1 < side.segments.size()
                ? std::min(frames, static_cast<std::uint64_t>(side.segments[i + 1].fileFrame))
                : frames;
        if (from < to) SUPPLE_TRY(run(seg.timeline, to - from, "/segments/" + std::to_string(i)));
    }
    return {};
}

std::optional<std::vector<RecordingWarning>> recordingWarnings(const RecordedTake& r) {
    if (r.warnings.empty()) return {};
    std::vector<RecordingWarning> out;
    for (const auto& e : r.warnings)
        out.push_back({e.code, e.message, e.path, e.hint, r.track.str(), r.file.string()});
    return out;
}

// midi_overdub (ADR-0016): the recording merges into the MIDI item under its
// first heard position, as a new take (the old one stays for undo and take
// switching). False when there is no such item (a new item is made then).
Result<bool> overdubMidi(model::EditTxn& t, const model::Session& s, const RecordedTake& r,
                         const std::vector<Run>& runs, double rate,
                         std::vector<RecordedInfo>& out) {
    if (runs.empty()) return false;
    const double atBeat = s.timeMap->beatAt(static_cast<double>(runs.front().t0) / rate);
    for (const auto& e : s.items) {
        const model::Item& it = *e.value;
        if (it.track != r.track || !it.activeTake) continue;
        const model::Take* old = s.takes.find(*it.activeTake);
        if (!old || !old->source || old->source->kind != model::Kind::Midi) continue;
        const model::ItemSpan span = model::itemSpan(s, it);
        const double itemEndBeat = s.timeMap->beatAt(span.endSec);
        if (atBeat < span.startBeat || atBeat >= itemEndBeat) continue;
        midi::Sequence seq = s.midi.find(*old->source)->sequence;
        const double ppq = seq.ppq;
        auto tick = [&](std::int64_t sample) {
            const double beat = s.timeMap->beatAt(static_cast<double>(sample) / rate);
            return std::llround(((beat - span.startBeat) * old->playrate + old->startOffset) * ppq);
        };
        const auto from = static_cast<std::int64_t>(std::llround(span.startSec * rate));
        const auto to = static_cast<std::int64_t>(std::llround(span.endSec * rate));
        for (const Run& run : runs)  // every pass layers onto the same take
            appendRecorded(seq, r, run.index, from, to, run.t0 + (run.to - run.from), tick);
        seq.canonicalize();
        model::MidiSource src;
        src.sequence = std::move(seq);
        const Id mi = t.create(std::move(src));
        model::Take take = *old;
        take.source = mi;
        take.name = (old->name.empty() ? std::string("MIDI") : old->name) + " overdub";
        auto copied = copyTake(s, t, *it.activeTake, std::move(take));
        if (!copied) return copied.error();
        const Id tk = *copied;
        t.modify<model::Item>(e.id, [&](model::Item& x) {
            x.takes.push_back(tk);
            x.activeTake = tk;
        });
        RecordedInfo info;
        info.warnings = recordingWarnings(r);
        info.track = r.track.str();
        info.item = e.id.str();
        info.take = tk.str();
        info.takes = {tk.str()};
        info.seconds = static_cast<double>(r.frames) / rate;
        info.lostFrames = static_cast<std::int64_t>(r.overflowFrames);
        out.push_back(std::move(info));
        return true;
    }
    return false;
}

// MIDI (ADR-0016): a source per take in the project's PPQ from the item start.

Status commitMidiTake(model::EditTxn& t, const model::Session& s, const RecordedTake& r,
                      const std::vector<Run>& runs, const std::vector<ItemPlan>& items, double rate,
                      std::vector<RecordedInfo>& out) {
    const model::Track* tr = s.tracks.find(r.track);
    if (tr && tr->record.mode == model::RecordMode::MidiOverdub) {
        auto overdub = overdubMidi(t, s, r, runs, rate, out);
        if (!overdub) return overdub.error();
        if (*overdub) return {};
    }
    const int ppq = s.project->ppq;
    for (const ItemPlan& ip : items) {
        model::Item item;
        item.track = r.track;
        const double start = static_cast<double>(ip.start) / rate;
        const double end = static_cast<double>(ip.end) / rate;
        model::placeItemSeconds(s, item, start, end);  // in the track's timebase
        const double startBeat = s.timeMap->beatAt(start);
        auto tick = [&](std::int64_t sample) {
            return static_cast<std::int64_t>(std::llround(
                (s.timeMap->beatAt(static_cast<double>(sample) / rate) - startBeat) * ppq));
        };
        RecordedInfo info;
        info.warnings = recordingWarnings(r);
        std::optional<Id> active;
        for (std::size_t k = 0; k < ip.takes.size(); ++k) {
            const Run& run = runs[ip.takes[k].run];
            midi::Sequence seq;
            seq.ppq = ppq;
            seq.length = tick(ip.end);
            appendRecorded(seq, r, run.index, ip.start, ip.end, run.t0 + (run.to - run.from), tick);
            seq.canonicalize();
            model::MidiSource src;
            src.sequence = std::move(seq);
            const Id mi = t.create(std::move(src));
            model::Take take;
            take.source = mi;
            take.name = (tr && !tr->name.empty() ? tr->name : r.track.str()) + " MIDI";
            if (ip.takes.size() > 1) take.name += " pass " + std::to_string(k + 1);
            const Id tk = t.create(std::move(take));
            item.takes.push_back(tk);
            info.takes.push_back(tk.str());
            if (ip.takes[k].whole) active = tk;  // the newest whole pass...
        }
        if (!active) active = item.takes.back();  // ...or the last one
        item.activeTake = active;
        const Id it = t.create(std::move(item));
        info.track = r.track.str();
        info.item = it.str();
        info.take = active->str();
        info.seconds = end - start;
        info.lostFrames = static_cast<std::int64_t>(r.overflowFrames);  // events, for MIDI
        out.push_back(std::move(info));
    }
    return {};
}

}  // namespace

Result<std::vector<RecordedInfo>> EngineApp::commitTakes(const std::vector<RecordedTake>& takes,
                                                         std::string_view label) {
    std::vector<RecordedInfo> out;
    const model::Session initial = view();
    for (const auto& r : takes) {
        if (r.frames <= 0) continue;
        const double rate = r.sampleRate == 0 ? initial.project->sampleRate : r.sampleRate;
        detail::RecordingInfo side{r.track,    r.startSample, rate,
                                   r.segments, r.loop.start,  r.loop.end};
        SUPPLE_TRY(recordingBounds(initial, side, static_cast<std::uint64_t>(r.frames)));
    }
    auto txnId = beginTransaction({cmd::Origin::Type::Internal, "engine", ""}, label);
    if (!txnId) return std::move(txnId).error();
    const model::Session s = view();
    // Relative to the media folder when there is one (a saved project's or the
    // scratch folder), so a first save can move the files and keep the paths.
    const fs::path media = projectDir().empty() ? fs::path() : projectDir() / s.project->mediaDir;
    for (const RecordedTake& r : takes) {
        for (const Error& e : r.warnings) {
            json::Value ev = json::Value::object();
            ev["code"] = e.code;
            ev["message"] = e.message;
            ev["path"] = e.path;
            ev["hint"] = e.hint;
            ev["track"] = r.track.str();
            ev["file"] = r.file.string();
            emit("record.warning", std::move(ev));
        }
        if (!r.error.empty()) {
            json::Value ev = json::Value::object();
            ev["track"] = r.track.str();
            ev["file"] = r.file.string();
            ev["error"] = r.error;
            emit("record.error", std::move(ev));
        }
        const double rate = r.sampleRate > 0 ? r.sampleRate : s.project->sampleRate;
        if (r.frames <= 0 || !s.tracks.find(r.track)) continue;
        // The runs on the timeline (ADR-0015): one per loop pass, or one.
        std::vector<RecordedSegment> segs = r.segments;
        if (segs.empty()) segs.push_back({0, r.startSample});
        std::vector<Run> runs;
        for (std::size_t k = 0; k < segs.size(); ++k) {
            const std::int64_t to =
                std::min(r.frames, k + 1 < segs.size() ? segs[k + 1].fileFrame : r.frames);
            if (to > segs[k].fileFrame)
                runs.push_back({segs[k].fileFrame, to, segs[k].timeline, k});
        }
        const bool passes = runs.size() > 1 && r.loop.end > r.loop.start;
        // Items: the loop range for passes (one take per pass), else one per
        // run; within the punch range and never before the timeline's start
        // (material outside stays in the file, skipped by the take).
        const auto punchFrom = static_cast<std::int64_t>(std::llround(punch_.start * rate));
        const auto punchTo = static_cast<std::int64_t>(std::llround(punch_.end * rate));
        auto clip = [&](std::int64_t& from, std::int64_t& to) {
            if (punch_.enabled) {
                from = std::max(from, punchFrom);
                to = std::min(to, punchTo);
            }
            from = std::max<std::int64_t>(from, 0);
            return to > from;
        };
        std::vector<ItemPlan> items;
        if (passes) {
            ItemPlan ip{r.loop.start, r.loop.end, {}};
            if (clip(ip.start, ip.end)) {
                for (std::size_t k = 0; k < runs.size(); ++k) {
                    const Run& run = runs[k];
                    const std::int64_t t1 = run.t0 + (run.to - run.from);
                    if (t1 <= ip.start || run.t0 >= ip.end) continue;
                    ip.takes.push_back(
                        {run.from + (ip.start - run.t0), run.t0 <= ip.start && t1 >= ip.end, k});
                }
                if (!ip.takes.empty()) items.push_back(std::move(ip));
            }
        } else {
            for (std::size_t k = 0; k < runs.size(); ++k) {
                const Run& run = runs[k];
                ItemPlan ip{run.t0, run.t0 + (run.to - run.from), {}};
                if (!clip(ip.start, ip.end)) continue;
                ip.takes.push_back({run.from + (ip.start - run.t0), true, k});
                items.push_back(std::move(ip));
            }
        }
        if (items.empty()) continue;
        if (r.isMidi) {
            if (!r.midi.empty()) SUPPLE_TRY(commitMidiTake(txn(), s, r, runs, items, rate, out));
            continue;
        }
        // The recorder hashed the take's file when it closed it (realtime.h
        // RecordedTake::hash), so a re-recorded or recovered file with the same
        // bytes reuses the source the session already has.
        model::EditTxn& t = txn();
        Id source;
        if (const auto known = model::mediaSourceWithHash(s, r.hash)) {
            source = *known;
        } else {
            model::MediaSource md;
            std::error_code ec;
            const fs::path rel = media.empty() ? fs::path() : fs::relative(r.file, media, ec);
            md.path = !ec && !rel.empty() && *rel.begin() != ".." ? rel.string() : r.file.string();
            md.hash = r.hash;
            md.format = "wav";
            md.sampleRate = rate;  // the device's (resampled to the plan's rate on playback)
            md.channels = 0;
            if (auto head = audio::inspectWavStream(r.file))
                md.channels = static_cast<int>(head->channels);
            md.frames = r.frames;
            source = t.create(std::move(md));
        }
        for (const ItemPlan& ip : items) {
            model::Item item;
            item.track = r.track;
            const double start = static_cast<double>(ip.start) / rate;
            const double end = static_cast<double>(ip.end) / rate;
            model::placeItemSeconds(s, item, start, end);  // in the track's timebase
            RecordedInfo info;
            info.warnings = recordingWarnings(r);
            std::optional<Id> active;
            for (std::size_t k = 0; k < ip.takes.size(); ++k) {
                model::Take take;
                take.source = source;
                take.startOffset =
                    static_cast<double>(ip.takes[k].fileFrame) / rate;  // < 0: silence first
                take.name = r.file.stem().string();
                if (ip.takes.size() > 1) take.name += " pass " + std::to_string(k + 1);
                const Id tk = t.create(std::move(take));
                item.takes.push_back(tk);
                info.takes.push_back(tk.str());
                if (ip.takes[k].whole) active = tk;  // the newest whole pass...
            }
            if (!active) active = item.takes.back();  // ...or the last one
            item.activeTake = active;
            const Id it = t.create(std::move(item));
            info.track = r.track.str();
            info.item = it.str();
            info.take = active->str();
            info.file = r.file.string();
            info.seconds = end - start;
            info.lostFrames = static_cast<std::int64_t>(r.overflowFrames);
            out.push_back(std::move(info));
        }
    }
    SUPPLE_TRY(commitTransaction(*txnId));
    return out;
}

// --- recordings (crash recovery) ------------------------------------------------------------

fs::path EngineApp::mediaDir() const {
    return projectDir().empty() ? options_.workdir / "media"
                                : projectDir() / head().project->mediaDir;
}

fs::path EngineApp::projectDir() const {
    return projectPath_.empty() ? options_.scratchDir : projectPath_.parent_path();
}

namespace {
bool within(const fs::path& inner, const fs::path& outer) {
    std::error_code ec;
    const fs::path a = fs::weakly_canonical(inner, ec), b = fs::weakly_canonical(outer, ec);
    const auto rel = a.lexically_relative(b);
    return !rel.empty() && *rel.begin() != "..";
}
}  // namespace

void EngineApp::clearScratch() {
    const fs::path& scratch = options_.scratchDir;
    // Only an unsaved project's: once saved, its recordings moved beside the
    // file (or, if that failed, are still here and must stay).
    if (!projectPath_.empty() || scratch.empty() || scratch == scratch.root_path()) return;
    if (within(options_.workdir, scratch)) return;
    std::error_code ec;
    fs::remove_all(scratch, ec);
}

// First save: stage and verify copies while the scratch originals remain.
// Failed relocation/publication removes only files this operation created.
Status EngineApp::moveScratchMedia(const fs::path& dest, const std::function<Status()>& publish) {
    const fs::path from = options_.scratchDir / head().project->mediaDir;
    std::error_code ec;
    if (!fs::exists(from, ec))
        return ec ? Status{Error("project.media_move", ec.message())} : publish();
    if (fs::equivalent(from, dest, ec)) return publish();
    ec.clear();
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(from, ec); !ec && it != fs::end(it);
         it.increment(ec)) {
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_symlink(status))
            return Error("project.media_move", "scratch media contains a symlink",
                         it->path().string());
        if (fs::is_regular_file(status)) files.push_back(it->path().lexically_relative(from));
    }
    if (ec) return Error("project.media_move", ec.message(), from.string());
    std::sort(files.begin(), files.end());
    for (const auto& rel : files) {
        const auto status = fs::symlink_status(dest / rel, ec);
        if (ec && ec != std::errc::no_such_file_or_directory)
            return Error("project.media_move", ec.message());
        if (fs::exists(status))
            return Error("project.media_exists", (dest / rel).string() + " already exists", {},
                         "save into another folder; scratch media is unchanged");
        ec.clear();
    }
    fs::create_directories(dest, ec);
    if (ec) return Error("project.media_move", ec.message(), dest.string());
    static std::atomic<std::uint64_t> nextStage{0};
    fs::path stage;
    for (;;) {
        stage = dest / (".save-stage-" + std::to_string(clockMs()) + "-" +
                        std::to_string(nextStage.fetch_add(1)));
        if (fs::create_directory(stage, ec)) break;
        if (ec) return Error("project.media_move", ec.message(), stage.string());
    }
    std::vector<fs::path> placed;
    bool published = false;
    struct Cleanup {
        fs::path stage;
        std::vector<fs::path>& placed;
        bool& published;
        ~Cleanup() {
            std::error_code ignored;
            if (!published)
                for (const auto& path : placed) fs::remove(path, ignored);
            fs::remove_all(stage, ignored);
        }
    } cleanup{stage, placed, published};
    for (const auto& rel : files) {
        const auto staged = stage / rel;
        fs::create_directories(staged.parent_path(), ec);
        if (!ec) fs::copy_file(from / rel, staged, fs::copy_options::none, ec);
        if (ec) return Error("project.media_move", ec.message(), (from / rel).string());
        auto original = sha256File(from / rel), copied = sha256File(staged);
        if (!original || !copied || *original != *copied)
            return Error("project.media_move", "staged media verification failed", rel.string());
    }
    for (const auto& rel : files) {
        const auto target = dest / rel;
        fs::create_directories(target.parent_path(), ec);
        if (!ec) fs::create_hard_link(stage / rel, target, ec);
        if (ec) return Error("project.media_move", ec.message(), target.string());
        placed.push_back(target);
    }
    SUPPLE_TRY(publish());
    published = true;
    std::vector<fs::path> emptyDirs;
    for (const auto& rel : files) {
        ec.clear();
        fs::remove(from / rel, ec);
        for (auto parent = (from / rel).parent_path(); parent != from && within(parent, from);
             parent = parent.parent_path())
            emptyDirs.push_back(parent);
    }
    std::sort(emptyDirs.begin(), emptyDirs.end(),
              [](const auto& a, const auto& b) { return a.native().size() > b.native().size(); });
    for (const auto& dir : emptyDirs) {
        ec.clear();
        fs::remove(dir, ec);
    }
    ec.clear();
    fs::remove(from, ec);
    ec.clear();
    fs::remove(options_.scratchDir, ec);  // only when empty
    return {};
}

namespace {

fs::path comparable(const fs::path& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(p, ec);
    return ec ? p.lexically_normal() : c;
}

}  // namespace

// Only regular files directly in a real rec/ directory: recovery rewrites
// headers, so nothing reached through a link is touched.
bool EngineApp::inRecordDir(const fs::path& file) const {
    std::error_code ec;
    const fs::path dir = recordDir();
    return fs::is_directory(fs::symlink_status(dir, ec)) &&
           fs::is_regular_file(fs::symlink_status(file, ec)) &&
           fs::equivalent(file.parent_path(), dir, ec) && !ec;
}

std::vector<RecoverableInfo> EngineApp::recoverable(std::vector<RecordingWarning>* warnings) const {
    std::vector<RecoverableInfo> out;
    const fs::path dir = recordDir();
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(dir, ec))) return out;
    // Every file a version uses, resolved the way plans resolve media.
    std::set<fs::path> used;
    const fs::path projectDir = this->projectDir();
    versions_->forEachVersion([&](const model::Version& v) {
        for (const auto& e : v.session->media) {
            const fs::path p = e.value->path;
            if (p.is_absolute()) {
                used.insert(comparable(p));
            } else {
                used.insert(comparable(projectDir / v.session->project->mediaDir / p));
                used.insert(comparable(projectDir / p));
            }
        }
    });
    for (const fs::path& f : recordingFiles_) used.insert(comparable(f));
    std::vector<fs::path> files;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator();
         it.increment(ec))
        if (it->path().extension() == ".wav") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    const fs::path media = mediaDir();
    for (const fs::path& f : files) {
        if (!inRecordDir(f) || used.count(comparable(f))) continue;
        auto side = detail::readSidecar(f);  // none: not a recording, or capture never began
        if (!side) {
            if (warnings && fs::exists(detail::sidecarPath(f), ec)) {
                const auto& e = side.error();
                warnings->push_back({"record.sidecar", e.message, e.path.empty() ? "/" : e.path,
                                     e.hint.empty()
                                         ? "restore a valid recording sidecar or skip this file"
                                         : e.hint,
                                     "", f.lexically_relative(media).string()});
            }
            continue;
        }
        auto info = audio::inspectWavStream(f);
        if (!info) {
            const auto& e = info.error();
            if (warnings)
                warnings->push_back(
                    {e.code, e.message, e.path.empty() ? "/file" : e.path,
                     e.hint.empty() ? "restore the recording audio or skip this file" : e.hint,
                     side->track.str(), f.lexically_relative(media).string()});
            continue;
        }
        if (info->fileFrames == 0) continue;
        if (auto valid = recordingBounds(head(), *side, info->fileFrames, info->sampleRate);
            !valid) {
            const auto& e = valid.error();
            if (warnings)
                warnings->push_back({e.code, e.message, e.path, e.hint, side->track.str(),
                                     f.lexically_relative(media).string()});
            continue;
        }
        RecoverableInfo r;
        r.file = f.lexically_relative(media).string();
        r.track = side->track.str();
        r.startSeconds = static_cast<double>(side->startSample) / side->sampleRate;
        r.seconds = static_cast<double>(info->fileFrames) / info->sampleRate;
        r.channels = info->channels;
        out.push_back(std::move(r));
    }
    return out;
}

Result<std::vector<RecordedInfo>> EngineApp::recover(const std::vector<std::string>& files,
                                                     std::optional<Id> track,
                                                     std::vector<RecordingWarning>* warnings) {
    std::vector<RecordingWarning> skipped;
    const std::vector<RecoverableInfo> all = recoverable(&skipped);
    if (warnings) warnings->insert(warnings->end(), skipped.begin(), skipped.end());
    auto warn = [&](const Error& e, const fs::path& file, std::string trackName = {}) {
        skipped.push_back({e.code, e.message, e.path, e.hint, std::move(trackName), file.string()});
        if (warnings) warnings->push_back(skipped.back());
    };
    std::vector<const RecoverableInfo*> chosen;
    if (files.empty()) {
        for (const RecoverableInfo& r : all) chosen.push_back(&r);
        if (chosen.empty() && skipped.empty())
            return Error("recover.nothing", "there are no recordings to recover");
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        const fs::path want = fs::path(files[i]).lexically_normal();
        auto it = std::find_if(all.begin(), all.end(),
                               [&](const RecoverableInfo& r) { return fs::path(r.file) == want; });
        if (it == all.end() &&
            std::any_of(skipped.begin(), skipped.end(),
                        [&](const RecordingWarning& w) { return fs::path(w.file) == want; }))
            continue;
        if (it == all.end())
            return Error("recover.unknown_file", files[i] + " is not a recording to recover",
                         "/files/" + std::to_string(i), "project.recoverable lists them");
        if (std::find(chosen.begin(), chosen.end(), &*it) != chosen.end())
            return Error("recover.duplicate_file", files[i] + " is listed twice",
                         "/files/" + std::to_string(i));
        chosen.push_back(&*it);
    }
    const model::Session& s = head();
    std::vector<RecordedTake> takes;
    for (const RecoverableInfo* r : chosen) {
        RecordedTake t;
        t.file = mediaDir() / r->file;
        auto side = detail::readSidecar(t.file);
        if (!side) {
            Error e = side.error();
            e.code = "record.sidecar";
            warn(e, r->file, r->track);
            continue;
        }
        auto info = audio::inspectWavStream(t.file);
        if (!info) {
            warn(info.error(), r->file, r->track);
            continue;
        }
        if (auto valid = recordingBounds(s, *side, info->fileFrames, info->sampleRate); !valid) {
            warn(valid.error(), r->file, r->track);
            continue;
        }
        t.track = track.value_or(side->track);
        if (!s.tracks.find(t.track))
            return Error("recover.no_track",
                         r->file + " was recorded on " + t.track.str() + ", which is gone",
                         "/track", "give a track to put it on");
        if (s.tracks.find(t.track)->type != model::StripType::Channel)
            return Error("track.strip_type", "recover recordings onto a channel", "/track",
                         "choose a child channel instead of a buss/master");
        t.startSample = side->startSample;
        t.sampleRate = side->sampleRate;
        t.segments = side->segments;  // loop passes, when the recording ended normally
        t.loop = {side->loopStart, side->loopEnd};
        takes.push_back(std::move(t));
    }
    for (auto it = takes.begin(); it != takes.end();) {
        RecordedTake& t = *it;
        if (!inRecordDir(t.file))  // replaced since it was listed
            return Error("recover.unknown_file",
                         t.file.string() + " is not a recording to recover");
        auto frames = audio::repairWavStream(t.file);
        if (!frames) {
            warn(frames.error(), t.file.lexically_relative(mediaDir()), t.track.str());
            it = takes.erase(it);
            continue;
        }
        t.frames = static_cast<std::int64_t>(*frames);
        ++it;
    }
    return commitTakes(takes, "recover");
}

void EngineApp::headChanged() {
    if (!engine_) return;
    // Returning to a version before the first patch/state edit means defaults,
    // rather than retaining the previous version's live plugin state.
    if (loadedHead_)
        for (const auto& e : head().fx)
            if (!e.value->patch && !e.value->state)
                if (const auto* before = loadedHead_->fx.find(e.id);
                    before && (before->patch || before->state))
                    pool_->restart(e.id);
    auto st = engine_->load(head(), projectDir());
    if (st) loadedHead_ = versions_->head();
    if (!st) emit("engine.error", json::Value(st.error().toString()));
    engine_->service();
}

// --- cmd::Host ------------------------------------------------------------------------------

void EngineApp::attachCommands(cmd::Dispatcher& dispatcher) {
    if (!scripts_) scripts_ = std::make_unique<lua::Runtime>();
    scripts_->setCommands(dispatcher.registry(), dispatcher);
}

cmd::Context EngineApp::makeContext(const cmd::Origin& origin) {
    cmd::Context ctx(origin);
    ctx.provide<EngineApp>(*this);
    if (scripts_) ctx.provide<lua::Runtime>(*scripts_);
    return ctx;
}

Result<std::uint64_t> EngineApp::beginTransaction(const cmd::Origin& origin,
                                                  std::string_view label) {
    if (txn_) return Error("history.nested", "a transaction is already open");
    txn_ = std::make_unique<model::EditTxn>(head(), versions_->nextVersion());
    record_ = model::TxnRecord{std::string(label), originText(origin), {}, {}};
    // Scoped by origin, so two clients' gestures never merge into one step.
    if (!origin.gesture.empty()) record_.coalesceKey = record_.origin + "|" + origin.gesture;
    return ++txnId_;
}

Status EngineApp::commitTransaction(std::uint64_t id) {
    if (!txn_ || id != txnId_) return Error("history.transaction", "no such open transaction");
    std::unique_ptr<model::EditTxn> t = std::move(txn_);
    auto changes = t->changes();
    if (changes.empty() && !t->projectChanged() && t->created().empty())
        return okStatus();  // nothing to record
    auto created = t->created();
    SUPPLE_TRY(model::validateStripRules(t->preview()));
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

std::function<void()> EngineApp::invocationSavepoint() {
    if (!txn_) return {};
    auto scriptEvents = scripts_ ? scripts_->eventSavepoint() : std::function<void()>{};
    return [this, saved = *txn_, record = record_, writing = autoWrite_, events = events_,
            scriptEvents = std::move(scriptEvents)]() mutable {
        *txn_ = std::move(saved);
        record_ = std::move(record);
        autoWrite_ = std::move(writing);
        events_ = std::move(events);
        if (scriptEvents) scriptEvents();
    };
}

void EngineApp::log(const cmd::LogRecord& record) {
    if (record.command == "log.list") return;  // reading the log is not news
    records_.push_back(record);
    while (records_.size() > kMaxRecords) records_.pop_front();
}

void EngineApp::emit(std::string_view topic, json::Value payload) {
    // Scripts see the event once the emitting command has finished
    // (commandFinished), like the clients on the control socket.
    if (scripts_) scripts_->event(topic, payload);
    events_.emplace_back(std::string(topic), std::move(payload));
}

void EngineApp::commandFinished() {
    if (clearScripts_ && scripts_ && !scripts_->active()) {
        scripts_->clear();
        clearScripts_ = false;
    }
    if (scripts_ && !clearScripts_) scripts_->deliver();
}

std::vector<std::pair<std::string, json::Value>> EngineApp::takeEvents() {
    return std::exchange(events_, {});
}

bool EngineApp::actionTransactional(std::string_view command) const {
    return command == "render.mix" || command == "render.stems" || command == "render.regions" ||
           command == "branch.render-ab";
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
    if (fixture == "engine_armed") {  // + tr_1 armed on inputs 1-2
        run("track.add", R"({"name": "Vocal"})");
        run("track.set", R"({"track": "tr_1", "armed": true, "record_input": [1, 2]})");
        auto st = startEngine("dummy", 256);
        assert(st && "fixture engine failed");
        (void)st;
        return;
    }
    if (fixture == "recording_interrupted") {  // tr_1 "Vocal" + rec/Vocal-1.wav, never committed
        run("track.add", R"({"name": "Vocal"})");
        // One second of stereo from 1 s on; the recording stopped (crashed)
        // half a second after its last header rewrite.
        const fs::path file = recordDir() / "Vocal-1.wav";
        std::error_code ec;
        fs::create_directories(file.parent_path(), ec);
        const double rate = head().project->sampleRate;
        const auto half = static_cast<std::size_t>(rate / 2);
        std::vector<float> frames(2 * half, 0.25f);
        audio::WavStreamWriter w;
        auto st = w.open(file, rate, 2);
        st = st ? w.append(frames.data(), half) : st;
        st = st ? w.updateHeader() : st;
        st = st ? w.append(frames.data(), half) : st;
        st = st ? w.flush() : st;
        w.abandon();
        st = st ? detail::writeSidecar(
                      file,
                      {Id{model::Kind::Track, 1}, static_cast<std::int64_t>(rate), rate, {}, 0, 0})
                : st;
        assert(st && "fixture recording failed");
        (void)st;
        return;
    }
    if (fixture == "song_with_audio") {  // tr_1 "Audio" + it_1/tk_1: four beats of a 0.5 tone
        run("track.add", R"({"name": "Audio"})");
        const fs::path file = mediaDir() / "tone.wav";
        audio::AudioData tone;
        tone.sampleRate = head().project->sampleRate;
        tone.channels.assign(2, std::vector<float>(static_cast<std::size_t>(2 * tone.sampleRate)));
        for (std::size_t i = 0; i < tone.frames(); ++i)
            tone.channels[0][i] = tone.channels[1][i] =
                static_cast<float>(0.5 * std::sin(2 * std::numbers::pi * 440.0 *
                                                  static_cast<double>(i) / tone.sampleRate));
        const Status st = audio::writeWav(file, tone);
        assert(st && "fixture audio failed");
        (void)st;
        run("item.add",
            R"({"track": "tr_1", "position": 0, "audio_file": ")" + file.string() + R"("})");
        return;
    }
    if (fixture == "meter_change") {  // 3/4 from bar 9
        run("timesig.set", R"({"bar": 9, "meter": "3/4"})");
        return;
    }
    if (fixture == "saved") {
        run("project.set", R"({"name": "Saved"})");
        (void)save("example-project.json");
        return;
    }
    run("track.add", R"({"name": "Bass"})");
    if (fixture == "song") return;
    if (fixture == "song_with_chain" || fixture == "song_with_saved_chain" ||
        fixture == "song_with_saved_template") {
        run("fx.add",
            R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","patch":{"params":{"gain":-6}}})");
        run("fx.add",
            R"({"track":"tr_1","plugin":"com.suppleaudio.test.gain","patch":{"params":{"gain":-12}}})");
        if (fixture == "song_with_saved_chain")
            run("fx.chain.save", R"({"track":"tr_1","name":"Bass"})");
        if (fixture == "song_with_saved_template")
            run("track.template.save", R"({"track":"tr_1","name":"Bass"})");
        return;
    }
    if (fixture == "song_with_keymix" || fixture == "song_with_keymix_4ch") {
        // + tr_2 "Keyed" with fx_1, a key mix (ADR-0018); 4ch: tr_2 has channels 3/4 (ADR-0020)
        run("track.add", R"({"name": "Keyed"})");
        run("fx.add", R"({"track": "tr_2", "plugin": "com.suppleaudio.test.keymix"})");
        if (fixture == "song_with_keymix_4ch")
            run("track.set", R"({"track": "tr_2", "channels": 4})");
        return;
    }
    if (fixture == "song_with_route") {
        run("route.add", R"({"from":"tr_1","to":"tr_0"})");
        return;
    }
    if (fixture == "song_with_marker") {
        run("marker.add", R"({"pos":4,"name":"Verse"})");
        return;
    }
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
    if (fixture == "song_with_region") {
        run("region.add", R"({"start":0,"end":4,"name":"Verse"})");
        return;
    }
    run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})");
    run("fx.add", R"({"track": "tr_1", "plugin": "com.suppleaudio.test.gain"})");
    if (fixture == "song_with_env" || fixture == "song_with_autoitem")  // + en_1: fx_2 gain
        run("env.add",
            R"({"fx": "fx_2", "param": "gain", "points": [{"beat": 0, "value": -12}, {"beat": 4, "value": 0}]})");
    if (fixture == "song_with_autoitem")  // + ai_1 on en_1: beats 4-8, a two-beat ramp, looped
        run("autoitem.add",
            R"({"env": "en_1", "position": 4, "length": 4, "source_length": 2, "points": [{"beat": 0, "value": -6}, {"beat": 2, "value": 0}]})");
    if (fixture == "song_with_render") run("render.mix", R"({"name":"example"})");
    if (fixture == "render_queued")  // + q1: the mix of the first beat
        run("render.queue.add",
            R"({"command": "render.mix", "args": {"end_beat": 1, "name": "queued"}})");
}

}  // namespace supple::engine
