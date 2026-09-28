#include "supple/engine/app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

#include "supple/audio/audio_file.h"
#include "supple/audio/wav_stream.h"
#include "supple/base/closest.h"
#include "supple/base/files.h"
#include "supple/clap/semantic_params.h"
#include "supple/engine/jack.h"
#include "supple/model/schemas.h"
#include "supple/product.h"

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

Result<std::vector<host::ParamInfo>> EngineApp::fxParams(const model::FxInstance& fx) {
    auto in = scratch(fx);
    if (!in) return std::move(in).error();
    return (*in)->params();
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

std::string EngineApp::rememberRender(const fs::path& path) {
    const std::string id = "rd_" + std::to_string(renders_.size() + 1);
    renders_[id] = path;
    return id;
}

Result<fs::path> EngineApp::renderPath(std::string_view reference) const {
    if (reference.starts_with("rd_")) {
        auto it = renders_.find(std::string(reference));
        if (it == renders_.end())
            return Error("engine.unknown_render", "no such render " + std::string(reference), {},
                         "render.mix returns a render id; use it or a WAV path");
        return it->second;
    }
    return resolve(fs::path(reference));
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
    if (loop_.enabled || loop_.end > loop_.start)
        engine_->setLoop(loop_.start, loop_.end, loop_.enabled);
    return engine_->load(head(), projectPath_.empty() ? fs::path() : projectPath_.parent_path());
}

Status EngineApp::startEngine(std::string_view backend, std::uint32_t block, bool anticipative) {
    if (engine_)
        return Error("engine.running", "the engine is already running", {},
                     "engine.stop first to change the backend or block size");
    BackendConfig config;
    config.sampleRate = view().project->sampleRate;
    config.blockSize = block;
    config.inputs = 2;         // the device's first stereo pair, for recording
    config.midiInputs = true;  // its MIDI inputs, for MIDI tracks (ADR-0016)
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
        pool_->prune(head());  // nothing processes now: replaced instances can go
    }
    backend_ = nullptr;
    ownedBackend_.reset();
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
    return st;
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
        engine_->play();
    } else if (action == "play") {
        playStart_ = engine_->transport().seconds;
        engine_->play();
    } else if (action == "pause" || action == "stop") {
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
    info.seconds = t.seconds;
    info.beat = view().timeMap->beatAt(t.seconds);
    info.sample = t.sample;
    info.loop = loop_;
    info.punch = punch_;
    info.recorded = std::move(recorded);
    return info;
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

// midi_overdub (ADR-0016): the recording merges into the MIDI item under its
// first heard position, as a new take (the old one stays for undo and take
// switching). False when there is no such item (a new item is made then).
bool overdubMidi(model::EditTxn& t, const model::Session& s, const RecordedTake& r,
                 const std::vector<Run>& runs, double rate, std::vector<RecordedInfo>& out) {
    if (runs.empty()) return false;
    const double atBeat = s.timeMap->beatAt(static_cast<double>(runs.front().t0) / rate);
    for (const auto& e : s.items) {
        const model::Item& it = *e.value;
        if (it.track != r.track || !it.activeTake) continue;
        const model::Take* old = s.takes.find(*it.activeTake);
        if (!old || !old->source || old->source->kind != model::Kind::Midi) continue;
        if (atBeat < it.position || atBeat >= it.position + it.length) continue;
        midi::Sequence seq = s.midi.find(*old->source)->sequence;
        const double ppq = seq.ppq;
        auto tick = [&](std::int64_t sample) {
            return static_cast<std::int64_t>(
                std::llround((s.timeMap->beatAt(static_cast<double>(sample) / rate) - it.position +
                              old->startOffset) *
                             ppq));
        };
        const auto from =
            static_cast<std::int64_t>(std::llround(s.timeMap->secondsAt(it.position) * rate));
        const auto to = static_cast<std::int64_t>(
            std::llround(s.timeMap->secondsAt(it.position + it.length) * rate));
        for (const Run& run : runs)  // every pass layers onto the same take
            appendRecorded(seq, r, run.index, from, to, run.t0 + (run.to - run.from), tick);
        seq.canonicalize();
        model::MidiSource src;
        src.sequence = std::move(seq);
        const Id mi = t.create(std::move(src));
        model::Take take = *old;
        take.source = mi;
        take.name = (old->name.empty() ? std::string("MIDI") : old->name) + " overdub";
        const Id tk = t.create(std::move(take));
        t.modify<model::Item>(e.id, [&](model::Item& x) {
            x.takes.push_back(tk);
            x.activeTake = tk;
        });
        RecordedInfo info;
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
void commitMidiTake(model::EditTxn& t, const model::Session& s, const RecordedTake& r,
                    const std::vector<Run>& runs, const std::vector<ItemPlan>& items, double rate,
                    std::vector<RecordedInfo>& out) {
    const model::Track* tr = s.tracks.find(r.track);
    if (tr && tr->record.mode == model::RecordMode::MidiOverdub &&
        overdubMidi(t, s, r, runs, rate, out))
        return;
    const int ppq = s.project->ppq;
    for (const ItemPlan& ip : items) {
        model::Item item;
        item.track = r.track;
        const double start = static_cast<double>(ip.start) / rate;
        const double end = static_cast<double>(ip.end) / rate;
        item.position = s.timeMap->beatAt(start);
        item.length = s.timeMap->beatAt(end) - item.position;
        auto tick = [&](std::int64_t sample) {
            return static_cast<std::int64_t>(std::llround(
                (s.timeMap->beatAt(static_cast<double>(sample) / rate) - item.position) * ppq));
        };
        RecordedInfo info;
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
}

}  // namespace

Result<std::vector<RecordedInfo>> EngineApp::commitTakes(const std::vector<RecordedTake>& takes,
                                                         std::string_view label) {
    std::vector<RecordedInfo> out;
    auto txnId = beginTransaction({cmd::Origin::Type::Internal, "engine", ""}, label);
    if (!txnId) return std::move(txnId).error();
    const model::Session s = view();
    const fs::path media =
        projectPath_.empty() ? fs::path() : projectPath_.parent_path() / s.project->mediaDir;
    for (const RecordedTake& r : takes) {
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
            if (!r.midi.empty()) commitMidiTake(txn(), s, r, runs, items, rate, out);
            continue;
        }
        model::MediaSource md;
        std::error_code ec;
        const fs::path rel = media.empty() ? fs::path() : fs::relative(r.file, media, ec);
        md.path = !ec && !rel.empty() && *rel.begin() != ".." ? rel.string() : r.file.string();
        md.format = "wav";
        md.sampleRate = rate;  // the device's (resampled to the plan's rate on playback)
        md.channels = 0;
        if (auto head = audio::inspectWavStream(r.file))
            md.channels = static_cast<int>(head->channels);
        md.frames = r.frames;
        model::EditTxn& t = txn();
        const Id source = t.create(std::move(md));
        for (const ItemPlan& ip : items) {
            model::Item item;
            item.track = r.track;
            const double start = static_cast<double>(ip.start) / rate;
            const double end = static_cast<double>(ip.end) / rate;
            item.position = s.timeMap->beatAt(start);
            item.length = s.timeMap->beatAt(end) - item.position;
            RecordedInfo info;
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
    return projectPath_.empty() ? options_.workdir / "media"
                                : projectPath_.parent_path() / head().project->mediaDir;
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

std::vector<RecoverableInfo> EngineApp::recoverable() const {
    std::vector<RecoverableInfo> out;
    const fs::path dir = recordDir();
    std::error_code ec;
    if (!fs::is_directory(fs::symlink_status(dir, ec))) return out;
    // Every file a version uses, resolved the way plans resolve media.
    std::set<fs::path> used;
    const fs::path projectDir = projectPath_.empty() ? fs::path() : projectPath_.parent_path();
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
        if (!side || !(side->sampleRate > 0)) continue;
        auto info = audio::inspectWavStream(f);
        if (!info || info->fileFrames == 0) continue;
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
                                                     std::optional<Id> track) {
    const std::vector<RecoverableInfo> all = recoverable();
    std::vector<const RecoverableInfo*> chosen;
    if (files.empty()) {
        for (const RecoverableInfo& r : all) chosen.push_back(&r);
        if (chosen.empty()) return Error("recover.nothing", "there are no recordings to recover");
    }
    for (std::size_t i = 0; i < files.size(); ++i) {
        const fs::path want = fs::path(files[i]).lexically_normal();
        auto it = std::find_if(all.begin(), all.end(),
                               [&](const RecoverableInfo& r) { return fs::path(r.file) == want; });
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
        if (!side) return std::move(side).error();
        t.track = track.value_or(side->track);
        if (!s.tracks.find(t.track))
            return Error("recover.no_track",
                         r->file + " was recorded on " + t.track.str() + ", which is gone",
                         "/track", "give a track to put it on");
        t.startSample = side->startSample;
        t.sampleRate = side->sampleRate;
        t.segments = side->segments;  // loop passes, when the recording ended normally
        t.loop = {side->loopStart, side->loopEnd};
        takes.push_back(std::move(t));
    }
    for (RecordedTake& t : takes) {
        if (!inRecordDir(t.file))  // replaced since it was listed
            return Error("recover.unknown_file",
                         t.file.string() + " is not a recording to recover");
        auto frames = audio::repairWavStream(t.file);
        if (!frames) return std::move(frames).error();
        t.frames = static_cast<std::int64_t>(*frames);
    }
    return commitTakes(takes, "recover");
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
    if (fixture == "song_with_env")  // + en_1: fx_2 gain, two points
        run("env.add",
            R"({"fx": "fx_2", "param": "gain", "points": [{"beat": 0, "value": -12}, {"beat": 4, "value": 0}]})");
    if (fixture == "song_with_render") run("render.mix", R"({"name":"example"})");
}

}  // namespace supple::engine
