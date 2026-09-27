// The engine's command catalogue (session-model §7) — the core set. Every
// command is a thin, typed wrapper: validate references with helpful errors,
// edit through the open EditTxn (edits) or read the view (queries), return
// IDs and the few fields an agent needs next.
#include <algorithm>
#include <cmath>
#include <set>

#include "supple/audio/audio_file.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"
#include "supple/notation/notation.h"
#include "supple/params/params.h"

namespace supple::engine {

using cmd::Command;
using cmd::Context;
using cmd::Kind;
using cmd::NoArgs;
using model::Id;
namespace fs = std::filesystem;

namespace {

json::Value obj(std::string_view text) { return json::parse(text).value(); }
EngineApp& A(Context& ctx) { return ctx.service<EngineApp>(); }

auto idArg(const char* kindPrefix, const char* what) {
    return schema::string()
        .pattern(std::string("^") + kindPrefix + "_[0-9]+$", std::string(what) + " id")
        .doc(std::string(what) + " id, e.g. \"" + kindPrefix + "_1\".");
}

// Existence check against the current view, with the candidates in the hint.
template <class T>
Result<Id> existing(const model::Session& s, const std::string& text, const char* path) {
    auto id = Id::parse(text);
    const auto& map = model::Traits<T>::map(s);
    if (id && id->kind == model::Traits<T>::kind && map.contains(*id)) return *id;
    std::string some;
    int n = 0;
    for (const auto& e : map) {
        if (n++ == 8) {
            some += ", …";
            break;
        }
        some += (some.empty() ? "" : ", ") + e.id.str();
    }
    return Error(std::string(model::prefix(model::Traits<T>::kind)) + ".unknown", "no " + text,
                 path, some.empty() ? "there are none yet" : "existing: " + some);
}

double beatsOf(const model::Session& s, std::optional<double> beats, std::optional<int> bar) {
    if (bar) return s.timeMap->beatOfBar(*bar);
    return beats.value_or(0.0);
}

struct Empty {};
schema::Schema<Empty> emptySchema() { return schema::object<Empty>("empty").build(); }

// --- project / session -----------------------------------------------------------------

struct ProjectInfo {
    std::string name, path, branch;
    double sampleRate = 0, tempo = 0;
    std::string meter;
    std::int64_t version = 0;
};
struct ProjectSetArgs {
    std::optional<std::string> name, title, author, notes;
    std::optional<double> panLawDb;
};
struct PathArgs {
    std::optional<std::string> path;
};
struct PathResult {
    std::string path;
};
struct TextResult {
    std::string text;
};

// --- engine / transport -------------------------------------------------------------------

enum class BackendArg { Jack, Dummy };
struct EngineStartArgs {
    std::optional<BackendArg> backend;
    std::optional<std::int64_t> block;
    std::optional<bool> anticipative;
};
struct SeekArgs {
    std::optional<double> seconds, beat;
    std::optional<std::int64_t> bar;
};

schema::Schema<EngineStatus> engineStatusSchema() {
    return schema::object<EngineStatus>("engine_status")
        .field("running", &EngineStatus::running, schema::boolean())
        .field("backend", &EngineStatus::backend, schema::string())
        .field("sample_rate", &EngineStatus::sampleRate, schema::number())
        .field("block", &EngineStatus::block, schema::integer())
        .field("period", &EngineStatus::period, schema::integer())
        .field("plan_latency", &EngineStatus::planLatency, schema::integer())
        .field("device_latency", &EngineStatus::deviceLatency, schema::integer())
        .field("xruns", &EngineStatus::xruns, schema::integer())
        .field("cycles", &EngineStatus::cycles, schema::integer())
        .field("process_errors", &EngineStatus::processErrors, schema::integer())
        .field("restart_needed", &EngineStatus::restartNeeded, schema::boolean())
        .field("anticipative", &EngineStatus::anticipative, schema::boolean())
        .field("anticipation_misses", &EngineStatus::anticipationMisses, schema::integer())
        .field("failed_fx", &EngineStatus::failedFx, schema::arrayOf(schema::string()))
        .build();
}

schema::Schema<TransportInfo> transportSchema() {
    auto recorded = schema::object<RecordedInfo>("recorded_take")
                        .field("track", &RecordedInfo::track, schema::string())
                        .field("item", &RecordedInfo::item, schema::string())
                        .field("take", &RecordedInfo::take, schema::string())
                        .field("file", &RecordedInfo::file, schema::string())
                        .field("seconds", &RecordedInfo::seconds, schema::number())
                        .field("lost_frames", &RecordedInfo::lostFrames, schema::integer())
                        .build();
    return schema::object<TransportInfo>("transport")
        .field("playing", &TransportInfo::playing, schema::boolean())
        .field("recording", &TransportInfo::recording, schema::boolean())
        .field("seconds", &TransportInfo::seconds, schema::number())
        .field("beat", &TransportInfo::beat, schema::number())
        .field("sample", &TransportInfo::sample, schema::integer())
        .field("recorded", &TransportInfo::recorded, schema::arrayOf(recorded))
        .build();
}

// --- automation envelopes ---------------------------------------------------------------

struct EnvPointArg {
    std::optional<double> beat, seconds;
    std::optional<std::int64_t> bar;
    json::Value value;
    std::optional<model::PointShape> shape;
    std::optional<double> tension;
};
struct EnvAddArgs {
    std::optional<std::string> track, fx, route;
    std::string param;
    std::optional<std::vector<EnvPointArg>> points;
};
struct EnvAddResult {
    std::string env;
    std::int64_t points = 0;
};
struct EnvPointsSetArgs {
    std::string env;
    std::vector<EnvPointArg> points;
    std::optional<double> fromBeat, toBeat;
};
struct EnvCount {
    std::int64_t points = 0;
};
struct EnvRef {
    std::string env;
};
struct EnvSetArgs {
    std::string env;
    std::optional<bool> active;
    std::optional<model::EnvMode> mode;
};
struct EnvListArgs {
    std::optional<std::string> object;
};
struct EnvEntry {
    std::string env, target, param, mode;
    std::int64_t points = 0;
    bool active = true;
};
struct EnvList {
    std::vector<EnvEntry> envelopes;
};

schema::Schema<model::PointShape> pointShapeSchema() {
    return schema::enumeration<model::PointShape>({{model::PointShape::Linear, "linear"},
                                                   {model::PointShape::Square, "square"},
                                                   {model::PointShape::Slow, "slow"},
                                                   {model::PointShape::FastStart, "fast_start"},
                                                   {model::PointShape::FastEnd, "fast_end"},
                                                   {model::PointShape::Bezier, "bezier"}});
}
schema::Schema<model::EnvMode> envModeSchema() {
    return schema::enumeration<model::EnvMode>({{model::EnvMode::Trim, "trim"},
                                                {model::EnvMode::Read, "read"},
                                                {model::EnvMode::Touch, "touch"},
                                                {model::EnvMode::Latch, "latch"},
                                                {model::EnvMode::LatchPreview, "latch_preview"},
                                                {model::EnvMode::Write, "write"}});
}
schema::Schema<EnvPointArg> envPointSchema() {
    return schema::object<EnvPointArg>("env_point_arg")
        .field("beat", &EnvPointArg::beat, schema::number().min(0))
        .field("bar", &EnvPointArg::bar, schema::integer().min(1))
        .field("seconds", &EnvPointArg::seconds, schema::number().min(0))
        .field("value", &EnvPointArg::value, schema::any())
        .field("shape", &EnvPointArg::shape, pointShapeSchema())
        .field("tension", &EnvPointArg::tension, schema::number().range(-1, 1))
        .build();
}

// A mixer parameter's unit, or none when `param` is not one.
std::optional<units::Unit> mixerUnit(model::Kind kind, const std::string& param) {
    if (param == "volume" || (kind == model::Kind::Track && param == "trim"))
        return units::Unit::dB;
    if (param == "pan" || (kind == model::Kind::Track && param == "width"))
        return units::Unit::percent;
    if (param == "mute") return units::Unit::none;
    return std::nullopt;
}

// How an envelope's values are read: a mixer unit, a plugin's semantic
// parameter (physical units, text with units accepted), or a plain CLAP value.
struct EnvDomain {
    std::optional<units::Unit> mixer;
    std::unique_ptr<params::DescribedParam> semantic;
    std::optional<std::pair<double, double>> clapRange;
};

Result<EnvDomain> envDomain(EngineApp& app, const model::Session& s, const model::EnvTarget& t,
                            const char* path) {
    EnvDomain d;
    if (t.object.kind != model::Kind::Fx) {
        d.mixer = mixerUnit(t.object.kind, t.param);
        if (!d.mixer)
            return Error("env.param",
                         "no automatable parameter \"" + t.param + "\" on " + t.object.str(), path,
                         t.object.kind == model::Kind::Track ? "volume, trim, pan, width, mute"
                                                             : "volume, pan, mute");
        return d;
    }
    const model::FxInstance& fx = *s.fx.find(t.object);
    auto schema = app.fxSchema(fx);
    if (!schema) return std::move(schema).error();
    if (const json::Value* ps = schema->isObject() ? schema->find("params") : nullptr) {
        if (const json::Value* entry = ps->find(t.param)) {
            auto spec = params::fromDescription(t.param, *entry);
            if (!spec) return std::move(spec).error();
            if ((*spec)->spec.flags & params::kReadonly)
                return Error("env.param", t.param + " is read-only", path);
            d.semantic = std::move(*spec);
            return d;
        }
    }
    auto list = app.fxParams(fx);
    if (!list) return std::move(list).error();
    for (const host::ParamInfo& p : *list)
        if (std::to_string(p.id) == t.param || p.name == t.param) {
            d.clapRange = std::pair{p.min, p.max};
            return d;
        }
    std::string keys;
    if (const json::Value* ps = schema->isObject() ? schema->find("params") : nullptr)
        for (const auto& m : ps->asObject()) keys += (keys.empty() ? "" : ", ") + m.key;
    return Error("env.param", fx.plugin.id + " has no parameter \"" + t.param + "\"", path,
                 keys.empty() ? "use a CLAP id or parameter name (fx.describe)" : "keys: " + keys);
}

Result<double> envValue(const EnvDomain& d, const json::Value& v, const std::string& path) {
    auto located = [&](Result<double> r) -> Result<double> {
        if (r) return r;
        Error e = std::move(r).error();
        if (e.path.empty()) e.path = path;
        return e;
    };
    if (d.semantic) return located(params::fromJson(d.semantic->spec, v, true));
    if (d.mixer) {
        if (v.isBool()) return v.asBool() ? 1.0 : 0.0;
        if (v.isNumber()) return v.asDouble();
        if (v.isString() && *d.mixer != units::Unit::none)
            return located(units::parseQuantity(v.asString(), *d.mixer));
        return Error("env.value", "a number (or text with a unit)", path);
    }
    if (!v.isNumber()) return Error("env.value", "a CLAP value (number)", path);
    return std::clamp(v.asDouble(), d.clapRange->first, d.clapRange->second);
}

Result<std::vector<model::EnvPoint>> envPoints(const model::Session& s, const EnvDomain& d,
                                               const std::vector<EnvPointArg>& in,
                                               const std::string& path) {
    std::vector<model::EnvPoint> out;
    for (std::size_t i = 0; i < in.size(); ++i) {
        const EnvPointArg& a = in[i];
        const std::string at = path + "/" + std::to_string(i);
        const int given = a.beat.has_value() + a.bar.has_value() + a.seconds.has_value();
        if (given != 1) return Error("env.position", "give one of beat, bar or seconds", at);
        model::EnvPoint pt;
        pt.pos = a.seconds
                     ? s.timeMap->beatAt(*a.seconds)
                     : beatsOf(s, a.beat,
                               a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt);
        auto v = envValue(d, a.value, at + "/value");
        if (!v) return std::move(v).error();
        pt.value = *v;
        pt.shape = a.shape.value_or(model::PointShape::Linear);
        pt.tension = a.tension.value_or(0.0);
        out.push_back(pt);
    }
    std::stable_sort(
        out.begin(), out.end(),
        [](const model::EnvPoint& x, const model::EnvPoint& y) { return x.pos < y.pos; });
    return out;
}

// --- tempo ------------------------------------------------------------------------------

struct TempoPointArg {
    double beat = 0, bpm = 120;
    bool ramp = false;
};
struct TempoSetArgs {
    std::vector<TempoPointArg> points;
};
struct TimesigSetArgs {
    int bar = 1;
    std::string meter;
};
struct CountResult {
    std::int64_t count = 0;
};

// --- tracks -----------------------------------------------------------------------------

struct TrackAddArgs {
    std::optional<std::string> name, parent;
    std::optional<std::int64_t> index;
    std::optional<std::int64_t> channels;
};
struct TrackRef {
    std::string track;
};
struct TrackSetArgs {
    std::string track;
    std::optional<std::string> name, color, notes;
    std::optional<double> volumeDb, pan, width;
    std::optional<bool> mute, phaseInvert;
    std::optional<std::string> solo;
    std::optional<bool> armed;
    std::optional<std::vector<std::int64_t>> recordInput;
    std::optional<model::Monitor> monitor;
};
struct TrackMoveArgs {
    std::string track;
    std::optional<std::string> parent;
    std::int64_t index = 0;
};
struct TrackEntry {
    std::string id, name, parent;
    std::int64_t depth = 0;
    double volumeDb = 0;
    bool mute = false;
    std::string solo;
    std::vector<std::string> fx, items;
};
struct TrackList {
    std::vector<TrackEntry> tracks;
};

// --- items / MIDI ------------------------------------------------------------------------

struct ItemAddArgs {
    std::string track;
    std::optional<double> position;  // beats
    std::optional<std::int64_t> bar;
    std::optional<double> length;  // beats
    std::optional<std::string> notation, audioFile;
    std::optional<std::string> name;
};
struct ItemAddResult {
    std::string item, take;
    std::optional<std::string> source;
    double position = 0, length = 0;
    std::optional<std::vector<std::string>> warnings;
};
struct ItemRef {
    std::string item;
};
struct ItemMoveArgs {
    std::string item;
    std::optional<double> position;
    std::optional<std::int64_t> bar;
    std::optional<std::string> track;
};
struct MidiWriteArgs {
    std::string item;
    std::string notation;
};
struct MidiGetArgs {
    std::string item;
};

// --- fx ------------------------------------------------------------------------------------

struct FxAddArgs {
    std::string track, plugin;
    std::optional<std::int64_t> index;
    std::optional<json::Value> patch;
};
struct FxAddResult {
    std::string fx, name;
    bool semantic = false;
    std::vector<std::string> chain;
};
struct FxRef {
    std::string fx;
};
struct FxSetArgs {
    std::string fx;
    std::optional<bool> bypass, offline;
    std::optional<double> wet;
    std::optional<std::string> label;
    std::optional<model::Isolation> isolation;
};
struct FxPatchArgs {
    std::string fx;
    json::Value patch;
};
struct FxParamSetArgs {
    std::string fx;
    json::Value values;
};
struct MergeResult {
    json::Value changed;
    std::string description;
};
struct FxDescribeArgs {
    std::string fx;
    bool full = false;
};
struct FxDescription {
    std::string plugin, name, description;
    std::optional<json::Value> params, patch;
};

// --- render / history ------------------------------------------------------------------------

struct RenderMixArgs {
    std::optional<double> startBeat, endBeat, tailS;
    std::optional<std::string> name;
};
struct RenderMixResult {
    std::string path;
    double seconds = 0, peakDbfs = 0;
    std::int64_t outputLatency = 0;
    std::optional<std::vector<std::string>> warnings;
};
struct HistoryResult {
    std::int64_t version = 0;
    std::string label;
};
struct HistoryEntry {
    std::int64_t version = 0;
    std::string label, origin;
};
struct HistoryList {
    std::vector<HistoryEntry> versions;
    bool canUndo = false, canRedo = false;
};

// Compiles notation into a MIDI source; the length in beats is the item's.
Result<std::pair<model::MidiSource, double>> compileNotation(const model::Session& s,
                                                             const std::string& text,
                                                             double atBeat) {
    notation::CompileContext cc;
    cc.ppq = s.project->ppq;
    auto compiled = notation::compile(text, cc);
    if (!compiled) {
        Error e = std::move(compiled).error();
        if (e.path.empty() || e.path.front() != '/') e.path = "/notation";
        return e;
    }
    (void)atBeat;
    model::MidiSource src;
    src.sequence = std::move(compiled->sequence);
    const double beats = static_cast<double>(src.sequence.length) / src.sequence.ppq;
    return std::pair{std::move(src), beats};
}

}  // namespace

void registerEngineCommands(cmd::Registry& registry) {
    // ---------------------------------------------------------------- project.get
    {
        Command<NoArgs, ProjectInfo> c;
        c.name = "project.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Show the project: name, rate, tempo, meter, file, version and branch.";
        c.tags = {"project"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = schema::object<ProjectInfo>("project_info")
                       .field("name", &ProjectInfo::name, schema::string())
                       .field("path", &ProjectInfo::path, schema::string())
                       .field("branch", &ProjectInfo::branch, schema::string())
                       .field("sample_rate", &ProjectInfo::sampleRate, schema::number())
                       .field("tempo", &ProjectInfo::tempo, schema::number())
                       .field("meter", &ProjectInfo::meter, schema::string())
                       .field("version", &ProjectInfo::version, schema::integer())
                       .build();
        c.examples = {
            {"A new project", obj("{}"),
             obj(R"({"name": "", "path": "", "branch": "main", "sample_rate": 48000.0, "tempo": 120.0, "meter": "4/4", "version": 0})"),
             "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<ProjectInfo> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            const auto m = s.timeMap->meterAt(1);
            return ProjectInfo{s.project->name,
                               app.projectPath().string(),
                               app.versions().branch(),
                               s.project->sampleRate,
                               s.timeMap->bpmAt(0),
                               std::to_string(m.numerator) + "/" + std::to_string(m.denominator),
                               static_cast<std::int64_t>(s.version)};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- project.set
    {
        Command<ProjectSetArgs, Empty> c;
        c.name = "project.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Change project metadata (name, title, author, notes, pan law).";
        c.tags = {"project"};
        c.args = schema::object<ProjectSetArgs>("project_set_args")
                     .field("name", &ProjectSetArgs::name, schema::string())
                     .field("title", &ProjectSetArgs::title, schema::string())
                     .field("author", &ProjectSetArgs::author, schema::string())
                     .field("notes", &ProjectSetArgs::notes, schema::string())
                     .field("pan_law_db", &ProjectSetArgs::panLawDb,
                            schema::number().range(-6, 0).unit(units::Unit::dB))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Name it", obj(R"({"name": "Demo"})"), obj("{}"), "empty"}};
        c.run = [](const ProjectSetArgs& a, Context& ctx) -> Result<Empty> {
            model::EditTxn& t = A(ctx).txn();
            model::Project p = t.project();
            if (a.name) p.name = *a.name;
            if (a.title) p.title = *a.title;
            if (a.author) p.author = *a.author;
            if (a.notes) p.notes = *a.notes;
            if (a.panLawDb) p.panLawDb = *a.panLawDb;
            t.setProject(std::move(p));
            return Empty{};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- project.save / project.open
    {
        Command<PathArgs, PathResult> c;
        c.name = "project.save";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save the project (canonical JSON) to its file or a new path.";
        c.description = "path: where to save (becomes the project's file); omit to save in place.";
        c.tags = {"project"};
        c.args = schema::object<PathArgs>("path_args")
                     .field("path", &PathArgs::path, schema::string())
                     .build();
        c.result = schema::object<PathResult>("path")
                       .field("path", &PathResult::path, schema::string())
                       .build();
        c.examples = {{"Save next to the work directory",
                       obj(R"({"path": "example-project.json"})"), obj(R"({"path": "<any>"})"),
                       "empty"}};
        c.run = [](const PathArgs& a, Context& ctx) -> Result<PathResult> {
            EngineApp& app = A(ctx);
            fs::path p = a.path ? fs::path(*a.path) : app.projectPath();
            if (p.empty())
                return Error("project.no_path", "the project has no file yet", "/path",
                             "give a path");
            SUPPLE_TRY(app.save(p));
            return PathResult{app.projectPath().string()};
        };
        registry.add(std::move(c));
    }
    {
        Command<PathArgs, ProjectInfo> c;
        c.name = "project.open";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Open a project file (replaces the current project and its history).";
        c.tags = {"project"};
        c.args = schema::object<PathArgs>("path_args")
                     .field("path", &PathArgs::path, schema::string())
                     .build();
        c.result = schema::object<ProjectInfo>("project_info")
                       .field("name", &ProjectInfo::name, schema::string())
                       .field("path", &ProjectInfo::path, schema::string())
                       .field("branch", &ProjectInfo::branch, schema::string())
                       .field("sample_rate", &ProjectInfo::sampleRate, schema::number())
                       .field("tempo", &ProjectInfo::tempo, schema::number())
                       .field("meter", &ProjectInfo::meter, schema::string())
                       .field("version", &ProjectInfo::version, schema::integer())
                       .build();
        c.errors = {{"file.open", "the file cannot be read", "check the path"},
                    {"schema.invalid", "the file has problems", "details list each with its path"}};
        c.examples = {
            {"Open", obj(R"({"path": "example-project.json"})"),
             obj(R"({"name": "<any>", "path": "<any>", "branch": "main", "sample_rate": "<any>", "tempo": "<any>", "meter": "<any>", "version": "<any>"})"),
             "saved"}};
        c.run = [](const PathArgs& a, Context& ctx) -> Result<ProjectInfo> {
            EngineApp& app = A(ctx);
            if (!a.path) return Error("project.no_path", "give the project file to open", "/path");
            SUPPLE_TRY(app.open(*a.path));
            const model::Session& s = app.head();
            const auto m = s.timeMap->meterAt(1);
            return ProjectInfo{s.project->name,
                               app.projectPath().string(),
                               app.versions().branch(),
                               s.project->sampleRate,
                               s.timeMap->bpmAt(0),
                               std::to_string(m.numerator) + "/" + std::to_string(m.denominator),
                               static_cast<std::int64_t>(s.version)};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- session.describe
    {
        Command<NoArgs, TextResult> c;
        c.name = "session.describe";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Describe the whole session in compact text (tracks, items, fx, routes).";
        c.description =
            "The quickest way to see where things are; use the IDs it shows with other commands.";
        c.tags = {"session"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = schema::object<TextResult>("text")
                       .field("text", &TextResult::text, schema::string())
                       .build();
        c.examples = {{"Describe", obj("{}"), obj(R"({"text": "<any>"})"), "song"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<TextResult> {
            return TextResult{describeSession(A(ctx).view())};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- tempo.set / timesig.set
    {
        Command<TempoSetArgs, CountResult> c;
        c.name = "tempo.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Replace the tempo map: points {beat, bpm, ramp?}; the first must be at beat 0.";
        c.description =
            "ramp: true changes the tempo linearly (in beats) to the next point; otherwise it "
            "steps.";
        c.tags = {"tempo"};
        auto point = schema::object<TempoPointArg>("tempo_point")
                         .field("beat", &TempoPointArg::beat, schema::number().min(0))
                         .field("bpm", &TempoPointArg::bpm,
                                schema::number().range(1, 1000).unit(units::Unit::bpm))
                         .field("ramp", &TempoPointArg::ramp, schema::boolean())
                         .defaultValue(false)
                         .build();
        c.args = schema::object<TempoSetArgs>("tempo_set_args")
                     .field("points", &TempoSetArgs::points, schema::arrayOf(point))
                     .build();
        c.result = schema::object<CountResult>("count")
                       .field("count", &CountResult::count, schema::integer())
                       .build();
        c.examples = {
            {"Slow down after 8 bars",
             obj(R"({"points": [{"beat": 0, "bpm": 120}, {"beat": 32, "bpm": 100, "ramp": false}]})"),
             obj(R"({"count": 2})"), "empty"}};
        c.run = [](const TempoSetArgs& a, Context& ctx) -> Result<CountResult> {
            model::EditTxn& t = A(ctx).txn();
            if (a.points.empty() || a.points.front().beat != 0.0)
                return Error("tempo.first", "the first tempo point must be at beat 0",
                             "/points/0/beat");
            for (std::size_t i = 1; i < a.points.size(); ++i)
                if (!(a.points[i].beat > a.points[i - 1].beat))
                    return Error("tempo.order", "tempo points must increase in beat",
                                 "/points/" + std::to_string(i) + "/beat");
            std::vector<Id> old;
            for (const auto& e : model::Traits<model::TempoPointObj>::map(t.base()))
                old.push_back(e.id);
            for (Id id : old) t.remove<model::TempoPointObj>(id);
            for (const auto& p : a.points) t.create(model::TempoPointObj{p.beat, p.bpm, p.ramp});
            return CountResult{static_cast<std::int64_t>(a.points.size())};
        };
        registry.add(std::move(c));
    }
    {
        Command<TimesigSetArgs, Empty> c;
        c.name = "timesig.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set the meter from a bar on, e.g. {\"bar\": 9, \"meter\": \"3/4\"}.";
        c.tags = {"tempo"};
        c.args = schema::object<TimesigSetArgs>("timesig_set_args")
                     .field("bar", &TimesigSetArgs::bar, schema::integerOf<int>().min(1))
                     .field("meter", &TimesigSetArgs::meter,
                            schema::string().pattern("^[0-9]{1,2}/[0-9]{1,2}$", "N/D"))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Waltz from bar 9", obj(R"({"bar": 9, "meter": "3/4"})"), obj("{}"), "empty"}};
        c.run = [](const TimesigSetArgs& a, Context& ctx) -> Result<Empty> {
            model::EditTxn& t = A(ctx).txn();
            const auto slash = a.meter.find('/');
            const int n = std::stoi(a.meter.substr(0, slash)),
                      d = std::stoi(a.meter.substr(slash + 1));
            if (n < 1 || n > 64 || d < 1 || d > 64 || (d & (d - 1)))
                return Error("timesig.invalid", "invalid meter " + a.meter, "/meter",
                             "N 1..64, D a power of 2");
            std::optional<Id> at;
            for (const auto& e : model::Traits<model::MeterObj>::map(t.base()))
                if (e.value->bar == a.bar) at = e.id;
            if (at)
                t.put(*at, model::MeterObj{a.bar, n, d});
            else
                t.create(model::MeterObj{a.bar, n, d});
            return Empty{};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- track.add
    {
        Command<TrackAddArgs, TrackRef> c;
        c.name = "track.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add a track (under the master, or inside a folder track) and return its id.";
        c.description =
            "parent: a track id to nest under (it becomes a folder); index: position among its "
            "siblings.";
        c.tags = {"track"};
        c.args = schema::object<TrackAddArgs>("track_add_args")
                     .field("name", &TrackAddArgs::name, schema::string())
                     .field("parent", &TrackAddArgs::parent, idArg("tr", "Parent track"))
                     .field("index", &TrackAddArgs::index, schema::integer().min(0))
                     .field("channels", &TrackAddArgs::channels, schema::integer().range(1, 64))
                     .build();
        c.result = schema::object<TrackRef>("track_ref")
                       .field("track", &TrackRef::track, schema::string())
                       .build();
        c.examples = {
            {"Add a bass track", obj(R"({"name": "Bass"})"), obj(R"({"track": "tr_1"})"), "empty"}};
        c.run = [](const TrackAddArgs& a, Context& ctx) -> Result<TrackRef> {
            model::EditTxn& t = A(ctx).txn();
            Id parent = model::kMaster;
            if (a.parent) {
                auto p = Id::parse(*a.parent);
                if (!p || !t.exists<model::Track>(*p))
                    return Error("tr.unknown", "no track " + *a.parent, "/parent",
                                 "see track.list");
                parent = *p;
            }
            model::Track tr;
            tr.name = a.name.value_or("");
            tr.parent = parent;
            if (a.channels) tr.channels = static_cast<int>(*a.channels);
            const Id id = t.create(tr);
            t.modify<model::Track>(parent, [&](model::Track& p) {
                const auto at = a.index ? std::min<std::size_t>(static_cast<std::size_t>(*a.index),
                                                                p.children.size())
                                        : p.children.size();
                p.children.insert(p.children.begin() + static_cast<std::ptrdiff_t>(at), id);
            });
            return TrackRef{id.str()};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- track.remove
    {
        Command<TrackRef, Empty> c;
        c.name = "track.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove a track with its items, fx, routes and envelopes (not a folder).";
        c.tags = {"track"};
        c.args = schema::object<TrackRef>("track_ref")
                     .field("track", &TrackRef::track, idArg("tr", "Track"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", obj(R"({"track": "tr_1"})"), obj("{}"), "song"}};
        c.run = [](const TrackRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            model::EditTxn& t = app.txn();
            const model::Session s = app.view();
            auto idr = existing<model::Track>(s, a.track, "/track");
            if (!idr) return std::move(idr).error();
            const Id id = *idr;
            if (id == model::kMaster)
                return Error("tr.master", "the master track cannot be removed", "/track");
            const model::Track& tr = *s.tracks.find(id);
            if (!tr.children.empty())
                return Error(
                    "tr.has_children",
                    a.track + " is a folder with " + std::to_string(tr.children.size()) + " tracks",
                    "/track", "move or remove its children first");
            for (const auto& e : s.items)
                if (e.value->track == id) {
                    for (Id tk : e.value->takes) {
                        const model::Take* take = s.takes.find(tk);
                        if (take)
                            for (Id f : take->chain) t.remove<model::FxInstance>(f);
                        t.remove<model::Take>(tk);
                    }
                    t.remove<model::Item>(e.id);
                }
            for (Id f : tr.chain) t.remove<model::FxInstance>(f);
            for (Id f : tr.inputChain) t.remove<model::FxInstance>(f);
            std::set<Id> gone(tr.chain.begin(), tr.chain.end());
            gone.insert(id);
            for (const auto& e : s.routes)
                if ((e.value->from.track && *e.value->from.track == id) ||
                    (e.value->to.track && *e.value->to.track == id)) {
                    gone.insert(e.id);
                    t.remove<model::Route>(e.id);
                }
            for (const auto& e : s.envelopes)
                if (gone.count(e.value->target.object)) t.remove<model::Envelope>(e.id);
            t.modify<model::Track>(*tr.parent, [&](model::Track& p) {
                p.children.erase(std::remove(p.children.begin(), p.children.end(), id),
                                 p.children.end());
            });
            t.remove<model::Track>(id);
            return Empty{};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- track.set
    {
        Command<TrackSetArgs, Empty> c;
        c.name = "track.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Set track fields: name, volume_db, pan, width, mute, solo, color, notes, arming.";
        c.description =
            "Only the given fields change. solo: off | on | in_place | defeat. armed + "
            "record_input (device input channels, 1-based) prepare a track for "
            "transport.record; monitor: off | on | auto (input monitoring is not heard yet).";
        c.tags = {"track", "mixer"};
        c.args = schema::object<TrackSetArgs>("track_set_args")
                     .field("track", &TrackSetArgs::track, idArg("tr", "Track"))
                     .field("name", &TrackSetArgs::name, schema::string())
                     .field("color", &TrackSetArgs::color, schema::string())
                     .field("notes", &TrackSetArgs::notes, schema::string())
                     .field("volume_db", &TrackSetArgs::volumeDb,
                            schema::number().range(model::kSilenceDb, 12).unit(units::Unit::dB))
                     .field("pan", &TrackSetArgs::pan,
                            schema::number().range(-100, 100).unit(units::Unit::percent))
                     .field("width", &TrackSetArgs::width,
                            schema::number().range(-100, 100).unit(units::Unit::percent))
                     .field("mute", &TrackSetArgs::mute, schema::boolean())
                     .field("phase_invert", &TrackSetArgs::phaseInvert, schema::boolean())
                     .field("solo", &TrackSetArgs::solo,
                            schema::string().pattern("^(off|on|in_place|defeat)$",
                                                     "off, on, in_place or defeat"))
                     .field("armed", &TrackSetArgs::armed, schema::boolean())
                     .field("record_input", &TrackSetArgs::recordInput,
                            schema::arrayOf(schema::integer().range(1, 64)))
                     .field("monitor", &TrackSetArgs::monitor,
                            schema::enumeration<model::Monitor>({{model::Monitor::Off, "off"},
                                                                 {model::Monitor::On, "on"},
                                                                 {model::Monitor::Auto, "auto"}}))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Turn the bass down 4 dB and a little left",
             obj(R"({"track": "tr_1", "volume_db": "-4 dB", "pan": -15})"), obj("{}"), "song"},
            {"Arm for recording from inputs 1 and 2",
             obj(R"({"track": "tr_1", "armed": true, "record_input": [1, 2]})"), obj("{}"),
             "song"}};
        c.run = [](const TrackSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Track>(s, a.track, "/track");
            if (!id) return std::move(id).error();
            if ((a.armed || a.recordInput || a.monitor) && *id == model::kMaster)
                return Error("track.master", "the master does not record", "/track");
            // The record input is a route from the device inputs (hw_input).
            std::optional<Id> input = s.tracks.find(*id)->record.input;
            if (a.recordInput) {
                if (a.recordInput->empty())
                    return Error("track.record_input", "give one or more input channels",
                                 "/record_input", "e.g. [1, 2] for a stereo pair");
                std::vector<int> channels(a.recordInput->begin(), a.recordInput->end());
                if (input && s.routes.find(*input)) {
                    app.txn().modify<model::Route>(
                        *input, [&](model::Route& r) { r.from.hwChannels = channels; });
                } else {
                    model::Route r;
                    r.from.hwChannels = channels;
                    r.to.track = *id;
                    input = app.txn().create(r);
                }
            }
            app.txn().modify<model::Track>(*id, [&](model::Track& t) {
                if (a.armed) t.record.armed = *a.armed;
                if (a.recordInput) t.record.input = input;
                if (a.monitor) t.record.monitor = *a.monitor;
                if (a.name) t.name = *a.name;
                if (a.color) t.color = *a.color;
                if (a.notes) t.notes = *a.notes;
                if (a.volumeDb) t.volumeDb = *a.volumeDb;
                if (a.pan) t.pan = *a.pan;
                if (a.width) t.width = *a.width;
                if (a.mute) t.mute = *a.mute;
                if (a.phaseInvert) t.phaseInvert = *a.phaseInvert;
                if (a.solo)
                    t.solo = *a.solo == "on"         ? model::Solo::On
                             : *a.solo == "in_place" ? model::Solo::InPlace
                             : *a.solo == "defeat"   ? model::Solo::Defeat
                                                     : model::Solo::Off;
            });
            return Empty{};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- track.list
    {
        Command<NoArgs, TrackList> c;
        c.name = "track.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List tracks in tree order with their level, mute/solo, fx and items.";
        c.tags = {"track"};
        c.args = schema::object<NoArgs>("no_args").build();
        auto entry = schema::object<TrackEntry>("track_entry")
                         .field("id", &TrackEntry::id, schema::string())
                         .field("name", &TrackEntry::name, schema::string())
                         .field("parent", &TrackEntry::parent, schema::string())
                         .field("depth", &TrackEntry::depth, schema::integer())
                         .field("volume_db", &TrackEntry::volumeDb, schema::number())
                         .field("mute", &TrackEntry::mute, schema::boolean())
                         .field("solo", &TrackEntry::solo, schema::string())
                         .field("fx", &TrackEntry::fx, schema::arrayOf(schema::string()))
                         .field("items", &TrackEntry::items, schema::arrayOf(schema::string()))
                         .build();
        c.result = schema::object<TrackList>("track_list")
                       .field("tracks", &TrackList::tracks, schema::arrayOf(entry))
                       .build();
        c.examples = {{"List", obj("{}"), obj(R"({"tracks": "<any>"})"), "song"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<TrackList> {
            const model::Session s = A(ctx).view();
            TrackList out;
            static constexpr const char* kSolo[] = {"off", "on", "in_place", "defeat"};
            std::function<void(Id, int)> walk = [&](Id id, int depth) {
                const model::Track& t = *s.tracks.find(id);
                TrackEntry e;
                e.id = id.str();
                e.name = t.name;
                e.parent = t.parent ? t.parent->str() : "";
                e.depth = depth;
                e.volumeDb = t.volumeDb;
                e.mute = t.mute;
                e.solo = kSolo[static_cast<int>(t.solo)];
                for (Id f : t.chain) e.fx.push_back(f.str());
                for (const auto& it : s.items)
                    if (it.value->track == id) e.items.push_back(it.id.str());
                out.tracks.push_back(std::move(e));
                for (Id c2 : t.children) walk(c2, depth + 1);
            };
            walk(model::kMaster, 0);
            return out;
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- item.add
    {
        Command<ItemAddArgs, ItemAddResult> c;
        c.name = "item.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add an item on a track: MIDI written in notation, or an audio file.";
        c.description =
            "position in beats (quarter notes from the start) or bar (1-based). notation: e.g. "
            "\"voice bass; 1: C2:q r:q C2:q r:q | F2:h G2:h\" — every bar must add up; the item "
            "is as long as the written bars unless length (beats) is given. audio_file: a path at "
            "the "
            "project's sample rate.";
        c.tags = {"item", "midi"};
        c.args =
            schema::object<ItemAddArgs>("item_add_args")
                .field("track", &ItemAddArgs::track, idArg("tr", "Track"))
                .field("position", &ItemAddArgs::position, schema::number().min(0).doc("Beats."))
                .field("bar", &ItemAddArgs::bar,
                       schema::integer().min(1).doc("Start at this bar (1-based)."))
                .field("length", &ItemAddArgs::length,
                       schema::number().exclusiveMin(0).doc("Beats."))
                .field("notation", &ItemAddArgs::notation, schema::string())
                .field("audio_file", &ItemAddArgs::audioFile, schema::string())
                .field("name", &ItemAddArgs::name, schema::string())
                .build();
        c.result =
            schema::object<ItemAddResult>("item_add")
                .field("item", &ItemAddResult::item, schema::string())
                .field("take", &ItemAddResult::take, schema::string())
                .field("source", &ItemAddResult::source, schema::string())
                .field("position", &ItemAddResult::position, schema::number())
                .field("length", &ItemAddResult::length, schema::number())
                .field("warnings", &ItemAddResult::warnings, schema::arrayOf(schema::string()))
                .build();
        c.errors = {
            {"notation.*", "the notation does not compile", "line:col and a hint in the error"},
            {"tr.unknown", "no such track", "see track.list"}};
        c.examples = {
            {"A bass line at bar 1",
             obj(R"({"track": "tr_1", "bar": 1, "notation": "voice bass; 1: C2:q r:q C2:q r:q"})"),
             obj(R"({"item": "it_1", "take": "tk_1", "source": "mi_1", "position": 0.0, "length": 4.0})"),
             "song"}};
        c.run = [](const ItemAddArgs& a, Context& ctx) -> Result<ItemAddResult> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto track = existing<model::Track>(s, a.track, "/track");
            if (!track) return std::move(track).error();
            if (*track == model::kMaster)
                return Error("item.master", "items cannot go on the master", "/track");
            if ((a.notation ? 1 : 0) + (a.audioFile ? 1 : 0) > 1)
                return Error("item.source", "give notation or audio_file, not both");
            const double pos = beatsOf(
                s, a.position, a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt);
            model::EditTxn& t = app.txn();
            model::Take take;
            take.name = a.name.value_or("");
            double length = a.length.value_or(4.0);
            ItemAddResult r;
            if (a.notation) {
                auto compiled = compileNotation(s, *a.notation, pos);
                if (!compiled) return std::move(compiled).error();
                if (!a.length) length = compiled->second;
                const Id mi = t.create(std::move(compiled->first));
                take.source = mi;
                r.source = mi.str();
            } else if (a.audioFile) {
                auto data = audio::readAudio(*a.audioFile);
                if (!data) return std::move(data).error().at("/audio_file");
                model::MediaSource md;
                md.path = fs::absolute(*a.audioFile).string();
                md.format = fs::path(*a.audioFile).extension().string().substr(1);
                md.sampleRate = data->sampleRate;
                md.channels = static_cast<int>(data->channelCount());
                md.frames = static_cast<std::int64_t>(data->frames());
                if (!a.length) {
                    const double secs = data->seconds();
                    length = s.timeMap->beatAt(s.timeMap->secondsAt(pos) + secs) - pos;
                }
                if (data->sampleRate != s.project->sampleRate)
                    r.warnings = std::vector<std::string>{
                        "the file is at " + std::to_string(static_cast<int>(data->sampleRate)) +
                        " Hz; it plays silent until resampling lands"};
                const Id md_ = t.create(std::move(md));
                take.source = md_;
                r.source = md_.str();
            }
            const Id tk = t.create(std::move(take));
            model::Item item;
            item.track = *track;
            item.position = pos;
            item.length = length;
            item.takes = {tk};
            item.activeTake = tk;
            const Id it = t.create(std::move(item));
            r.item = it.str();
            r.take = tk.str();
            r.position = pos;
            r.length = length;
            return r;
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- item.remove / item.move
    {
        Command<ItemRef, Empty> c;
        c.name = "item.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove an item with its takes (unpooled sources go too).";
        c.tags = {"item"};
        c.args = schema::object<ItemRef>("item_ref")
                     .field("item", &ItemRef::item, idArg("it", "Item"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", obj(R"({"item": "it_1"})"), obj("{}"), "song_with_item"}};
        c.run = [](const ItemRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            model::EditTxn& t = app.txn();
            for (Id tk : s.items.find(*id)->takes) {
                const model::Take* take = s.takes.find(tk);
                if (take) {
                    for (Id f : take->chain) t.remove<model::FxInstance>(f);
                    if (take->source && take->source->kind == model::Kind::Midi) {
                        const model::MidiSource* m = s.midi.find(*take->source);
                        if (m && !m->pooled) t.remove<model::MidiSource>(*take->source);
                    }
                }
                t.remove<model::Take>(tk);
            }
            t.remove<model::Item>(*id);
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<ItemMoveArgs, Empty> c;
        c.name = "item.move";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Move an item in time (position in beats or bar) and/or to another track.";
        c.tags = {"item"};
        c.args = schema::object<ItemMoveArgs>("item_move_args")
                     .field("item", &ItemMoveArgs::item, idArg("it", "Item"))
                     .field("position", &ItemMoveArgs::position, schema::number().min(0))
                     .field("bar", &ItemMoveArgs::bar, schema::integer().min(1))
                     .field("track", &ItemMoveArgs::track, idArg("tr", "Track"))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Two bars later", obj(R"({"item": "it_1", "bar": 3})"), obj("{}"), "song_with_item"}};
        c.run = [](const ItemMoveArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            std::optional<Id> track;
            if (a.track) {
                auto tr = existing<model::Track>(s, *a.track, "/track");
                if (!tr) return std::move(tr).error();
                if (*tr == model::kMaster)
                    return Error("item.master", "items cannot go on the master", "/track");
                track = *tr;
            }
            app.txn().modify<model::Item>(*id, [&](model::Item& it) {
                if (a.position || a.bar)
                    it.position = beatsOf(
                        s, a.position,
                        a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt);
                if (track) it.track = *track;
            });
            return Empty{};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- midi.write / midi.get
    {
        Command<MidiWriteArgs, Empty> c;
        c.name = "midi.write";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Replace an item's MIDI with notation (the item keeps its position and length).";
        c.tags = {"midi"};
        c.args = schema::object<MidiWriteArgs>("midi_write_args")
                     .field("item", &MidiWriteArgs::item, idArg("it", "Item"))
                     .field("notation", &MidiWriteArgs::notation, schema::string())
                     .build();
        c.result = emptySchema();
        c.examples = {{"Rewrite the bass line",
                       obj(R"({"item": "it_1", "notation": "voice bass; 1: C2:e C2:e r:q G1:h"})"),
                       obj("{}"), "song_with_item"}};
        c.run = [](const MidiWriteArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const model::Item& it = *s.items.find(*id);
            const model::Take* take = it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
            if (!take || !take->source || take->source->kind != model::Kind::Midi)
                return Error("midi.not_midi", a.item + " has no MIDI take", "/item",
                             "use item.add with notation");
            auto compiled = compileNotation(s, a.notation, it.position);
            if (!compiled) return std::move(compiled).error();
            const model::MidiSource* old = s.midi.find(*take->source);
            compiled->first.pooled = old && old->pooled;
            app.txn().put<model::MidiSource>(*take->source, std::move(compiled->first));
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<MidiGetArgs, TextResult> c;
        c.name = "midi.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read an item's MIDI as notation.";
        c.tags = {"midi"};
        c.args = schema::object<MidiGetArgs>("midi_get_args")
                     .field("item", &MidiGetArgs::item, idArg("it", "Item"))
                     .build();
        c.result = schema::object<TextResult>("text")
                       .field("text", &TextResult::text, schema::string())
                       .build();
        c.examples = {{"Read the bass line", obj(R"({"item": "it_1"})"),
                       obj(R"({"text": "<any>"})"), "song_with_item"}};
        c.run = [](const MidiGetArgs& a, Context& ctx) -> Result<TextResult> {
            const model::Session s = A(ctx).view();
            auto id = existing<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const model::Item& it = *s.items.find(*id);
            const model::Take* take = it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
            if (!take || !take->source || take->source->kind != model::Kind::Midi)
                return Error("midi.not_midi", a.item + " has no MIDI take", "/item");
            auto text = notation::decompile(s.midi.find(*take->source)->sequence);
            if (!text) return std::move(text).error();
            return TextResult{std::move(*text)};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- fx.add
    {
        Command<FxAddArgs, FxAddResult> c;
        c.name = "fx.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Insert a plugin into a track's FX chain and return its id.";
        c.description =
            "plugin: id or name from plugin.list; patch: an initial (partial) patch for semantic "
            "plugins.";
        c.tags = {"fx"};
        c.args = schema::object<FxAddArgs>("fx_add_args")
                     .field("track", &FxAddArgs::track, idArg("tr", "Track"))
                     .field("plugin", &FxAddArgs::plugin, schema::string().minLength(1))
                     .field("index", &FxAddArgs::index, schema::integer().min(0))
                     .field("patch", &FxAddArgs::patch, schema::any())
                     .build();
        c.result = schema::object<FxAddResult>("fx_add")
                       .field("fx", &FxAddResult::fx, schema::string())
                       .field("name", &FxAddResult::name, schema::string())
                       .field("semantic", &FxAddResult::semantic, schema::boolean())
                       .field("chain", &FxAddResult::chain, schema::arrayOf(schema::string()))
                       .build();
        c.errors = {
            {"plugin.unknown", "no such plugin", "closest ids in the hint; see plugin.list"}};
        c.examples = {
            {"An instrument on the bass track",
             obj(R"({"track": "tr_1", "plugin": "com.suppleaudio.test.sine"})"),
             obj(R"({"fx": "fx_1", "name": "Test Sine", "semantic": false, "chain": ["fx_1"]})"),
             "song"}};
        c.run = [](const FxAddArgs& a, Context& ctx) -> Result<FxAddResult> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto track = existing<model::Track>(s, a.track, "/track");
            if (!track) return std::move(track).error();
            auto info = app.findPlugin(a.plugin);
            if (!info) return std::move(info).error().at("/plugin");
            model::FxInstance fx;
            fx.plugin = {"clap", info->id, info->name, info->vendor, info->version};
            if (a.patch) {
                auto merged = app.mergedPatch(fx, *a.patch, false);
                if (!merged) {
                    Error e = std::move(merged).error();
                    e.path = "/patch" + e.path;
                    return e;
                }
                fx.patch = std::move(*merged);
            }
            model::EditTxn& t = app.txn();
            const Id id = t.create(std::move(fx));
            std::vector<Id> chain = s.tracks.find(*track)->chain;
            std::size_t at =
                a.index ? std::min<std::size_t>(static_cast<std::size_t>(*a.index), chain.size())
                        : chain.size();
            if (info->instrument && !a.index) at = 0;
            chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(at), id);
            t.modify<model::Track>(*track, [&](model::Track& tr) { tr.chain = chain; });
            FxAddResult r{id.str(), info->name, info->semantic, {}};
            for (Id f : chain) r.chain.push_back(f.str());
            return r;
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- fx.remove / fx.set
    {
        Command<FxRef, Empty> c;
        c.name = "fx.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove a plugin instance (and its envelopes).";
        c.tags = {"fx"};
        c.args = schema::object<FxRef>("fx_ref")
                     .field("fx", &FxRef::fx, idArg("fx", "Plugin instance"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", obj(R"({"fx": "fx_1"})"), obj("{}"), "song_with_fx"}};
        c.run = [](const FxRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            model::EditTxn& t = app.txn();
            auto drop = [&](std::vector<Id>& chain) {
                chain.erase(std::remove(chain.begin(), chain.end(), *id), chain.end());
            };
            for (const auto& e : s.tracks)
                if (std::count(e.value->chain.begin(), e.value->chain.end(), *id) ||
                    std::count(e.value->inputChain.begin(), e.value->inputChain.end(), *id))
                    t.modify<model::Track>(e.id, [&](model::Track& tr) {
                        drop(tr.chain);
                        drop(tr.inputChain);
                    });
            for (const auto& e : s.takes)
                if (std::count(e.value->chain.begin(), e.value->chain.end(), *id))
                    t.modify<model::Take>(e.id, [&](model::Take& tk) { drop(tk.chain); });
            for (const auto& e : s.envelopes)
                if (e.value->target.object == *id) t.remove<model::Envelope>(e.id);
            t.remove<model::FxInstance>(*id);
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<FxSetArgs, Empty> c;
        c.name = "fx.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set instance options: bypass, offline, wet (%), label, isolation.";
        c.description =
            "isolation: in_process, dedicated (the plugin runs in a process of its own: a "
            "crash silences only it), shared_bridge (one process for all such plugins), "
            "inherit (the project default: in_process).";
        c.tags = {"fx"};
        c.args = schema::object<FxSetArgs>("fx_set_args")
                     .field("fx", &FxSetArgs::fx, idArg("fx", "Plugin instance"))
                     .field("bypass", &FxSetArgs::bypass, schema::boolean())
                     .field("offline", &FxSetArgs::offline, schema::boolean())
                     .field("wet", &FxSetArgs::wet,
                            schema::number().range(0, 100).unit(units::Unit::percent))
                     .field("label", &FxSetArgs::label, schema::string())
                     .field("isolation", &FxSetArgs::isolation,
                            schema::enumeration<model::Isolation>(
                                {{model::Isolation::Inherit, "inherit"},
                                 {model::Isolation::InProcess, "in_process"},
                                 {model::Isolation::SharedBridge, "shared_bridge"},
                                 {model::Isolation::Dedicated, "dedicated"}}))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Half wet", obj(R"({"fx": "fx_1", "wet": "50%"})"), obj("{}"), "song_with_fx"}};
        c.run = [](const FxSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            auto id = existing<model::FxInstance>(app.view(), a.fx, "/fx");
            if (!id) return std::move(id).error();
            app.txn().modify<model::FxInstance>(*id, [&](model::FxInstance& f) {
                if (a.bypass) f.bypass = *a.bypass;
                if (a.offline) f.offline = *a.offline;
                if (a.wet) f.wet = *a.wet;
                if (a.label) f.label = *a.label;
                if (a.isolation) f.isolation = *a.isolation;
            });
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<FxRef, Empty> c;
        c.name = "fx.restart";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Re-create a plugin instance from its saved patch/state (after a crash).";
        c.description =
            "engine.status lists failed_fx: isolated plugins whose process crashed or hung. "
            "The new instance starts from the model's patch or state.";
        c.tags = {"fx"};
        c.args = schema::object<FxRef>("fx_ref")
                     .field("fx", &FxRef::fx, idArg("fx", "Plugin instance"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Restart", obj(R"({"fx": "fx_1"})"), obj("{}"), "song_with_fx"}};
        c.run = [](const FxRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            auto id = existing<model::FxInstance>(app.view(), a.fx, "/fx");
            if (!id) return std::move(id).error();
            app.restartFx(*id);
            return Empty{};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- fx.patch.merge /
    // fx.param.set
    auto mergeRun = [](EngineApp& app, const std::string& fxText, const json::Value& patch,
                       const char* patchPath) -> Result<MergeResult> {
        const model::Session s = app.view();
        auto id = existing<model::FxInstance>(s, fxText, "/fx");
        if (!id) return std::move(id).error();
        const model::FxInstance& fx = *s.fx.find(*id);
        auto merged = app.mergedPatch(fx, patch, false);
        if (!merged) {
            Error e = std::move(merged).error();
            auto fix = [&](Error& x) {
                if (std::string_view(patchPath) == "/values" && x.path.rfind("/params", 0) == 0)
                    x.path = "/values" + x.path.substr(7);
                else if (!x.path.empty() || x.details.empty())
                    x.path = std::string(patchPath) + x.path;
            };
            fix(e);
            for (auto& d : e.details) fix(d);
            return e;
        }
        MergeResult r;
        auto before = fx.patch ? *fx.patch : json::Value::object();
        auto changed = json::createMergePatch(before, *merged);
        r.changed = changed ? std::move(*changed) : json::Value::object();
        model::FxInstance next = fx;
        next.patch = std::move(*merged);
        auto d = app.describeFx(next, true);
        r.description = d ? *d : "";
        app.txn().put<model::FxInstance>(*id, std::move(next));
        return r;
    };
    auto mergeResultSchema = [] {
        return schema::object<MergeResult>("merge_result")
            .field("changed", &MergeResult::changed, schema::any())
            .field("description", &MergeResult::description, schema::string())
            .build();
    };
    {
        Command<FxPatchArgs, MergeResult> c;
        c.name = "fx.patch.merge";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Merge a partial patch into a plugin instance; returns the effective change.";
        c.description =
            "Semantic plugins only. Values in physical units: {\"params\": {\"gain\": \"-6 dB\"}}.";
        c.tags = {"fx", "patch"};
        c.args = schema::object<FxPatchArgs>("fx_patch_args")
                     .field("fx", &FxPatchArgs::fx, idArg("fx", "Plugin instance"))
                     .field("patch", &FxPatchArgs::patch, schema::any())
                     .build();
        c.result = mergeResultSchema();
        c.examples = {{"Turn the gain down",
                       obj(R"({"fx": "fx_2", "patch": {"params": {"gain": "-6 dB"}}})"),
                       obj(R"({"changed": "<any>", "description": "<any>"})"), "song_with_fx"}};
        c.run = [mergeRun](const FxPatchArgs& a, Context& ctx) {
            return mergeRun(A(ctx), a.fx, a.patch, "/patch");
        };
        registry.add(std::move(c));
    }
    {
        Command<FxParamSetArgs, MergeResult> c;
        c.name = "fx.param.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set plugin parameters by key in physical units: {\"cutoff\": \"800 Hz\"}.";
        c.tags = {"fx", "params"};
        c.args = schema::object<FxParamSetArgs>("fx_param_set_args")
                     .field("fx", &FxParamSetArgs::fx, idArg("fx", "Plugin instance"))
                     .field("values", &FxParamSetArgs::values, schema::any())
                     .build();
        c.result = mergeResultSchema();
        c.examples = {{"Set the gain", obj(R"({"fx": "fx_2", "values": {"gain": -3}})"),
                       obj(R"({"changed": "<any>", "description": "<any>"})"), "song_with_fx"}};
        c.run = [mergeRun](const FxParamSetArgs& a, Context& ctx) -> Result<MergeResult> {
            json::Value patch = json::Value::object();
            patch["params"] = a.values;
            return mergeRun(A(ctx), a.fx, patch, "/values");
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- fx.describe
    {
        Command<FxDescribeArgs, FxDescription> c;
        c.name = "fx.describe";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Describe a plugin instance in words; full adds its parameters and patch.";
        c.tags = {"fx"};
        c.args = schema::object<FxDescribeArgs>("fx_describe_args")
                     .field("fx", &FxDescribeArgs::fx, idArg("fx", "Plugin instance"))
                     .field("full", &FxDescribeArgs::full, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::object<FxDescription>("fx_description")
                       .field("plugin", &FxDescription::plugin, schema::string())
                       .field("name", &FxDescription::name, schema::string())
                       .field("description", &FxDescription::description, schema::string())
                       .field("params", &FxDescription::params, schema::any())
                       .field("patch", &FxDescription::patch, schema::any())
                       .build();
        c.examples = {
            {"Describe", obj(R"({"fx": "fx_2"})"),
             obj(R"({"plugin": "com.suppleaudio.test.gain", "name": "<any>", "description": "<any>"})"),
             "song_with_fx"}};
        c.run = [](const FxDescribeArgs& a, Context& ctx) -> Result<FxDescription> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            const model::FxInstance& fx = *s.fx.find(*id);
            FxDescription r;
            r.plugin = fx.plugin.id;
            r.name = fx.plugin.name;
            auto d = app.describeFx(fx, !a.full);
            if (!d) return std::move(d).error();
            r.description = *d;
            if (a.full) {
                auto schema = app.fxSchema(fx);
                if (schema && schema->isObject() && schema->find("params"))
                    r.params = *schema->find("params");
                if (fx.patch) r.patch = *fx.patch;
            }
            return r;
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- render.mix
    {
        Command<RenderMixArgs, RenderMixResult> c;
        c.name = "render.mix";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Render the mix offline to a float WAV (default: start to the last item's end).";
        c.description =
            "start_beat/end_beat in beats; tail_s seconds after the end (reverb tails).";
        c.tags = {"render"};
        c.args = schema::object<RenderMixArgs>("render_mix_args")
                     .field("start_beat", &RenderMixArgs::startBeat, schema::number().min(0))
                     .field("end_beat", &RenderMixArgs::endBeat, schema::number().exclusiveMin(0))
                     .field("tail_s", &RenderMixArgs::tailS, schema::number().range(0, 60))
                     .field("name", &RenderMixArgs::name,
                            schema::string().pattern("^[A-Za-z0-9 _.+-]{1,64}$",
                                                     "a file-name-safe name"))
                     .build();
        c.result =
            schema::object<RenderMixResult>("render_mix")
                .field("path", &RenderMixResult::path, schema::string())
                .field("seconds", &RenderMixResult::seconds, schema::number())
                .field("peak_dbfs", &RenderMixResult::peakDbfs, schema::number())
                .field("output_latency", &RenderMixResult::outputLatency, schema::integer())
                .field("warnings", &RenderMixResult::warnings, schema::arrayOf(schema::string()))
                .build();
        c.examples = {
            {"Render", obj(R"({"name": "mix-v1"})"),
             obj(R"({"path": "<any>", "seconds": "<any>", "peak_dbfs": "<any>", "output_latency": 0})"),
             "song_with_fx"}};
        c.run = [](const RenderMixArgs& a, Context& ctx) -> Result<RenderMixResult> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            double endBeat = a.endBeat.value_or(0.0);
            if (!a.endBeat)
                for (const auto& e : s.items)
                    endBeat = std::max(endBeat, e.value->position + e.value->length);
            if (endBeat <= a.startBeat.value_or(0.0))
                return Error("render.empty", "nothing to render", {}, "add items or give end_beat");
            OfflineSettings o;
            o.startSeconds = s.timeMap->secondsAt(a.startBeat.value_or(0.0));
            o.endSeconds = s.timeMap->secondsAt(endBeat);
            o.tailSeconds = a.tailS.value_or(0.0);
            auto r = app.renderMix(o);
            if (!r) return std::move(r).error();
            const std::string name = a.name.value_or("mix-v" + std::to_string(s.version));
            const fs::path path = app.rendersDir() / (name + ".wav");
            SUPPLE_TRY(audio::writeWav(path, r->audio));
            float peak = 0;
            for (const auto& ch : r->audio.channels)
                for (float v : ch) peak = std::max(peak, std::fabs(v));
            RenderMixResult out;
            out.path = path.string();
            out.seconds = std::round(r->audio.seconds() * 1e4) / 1e4;
            out.peakDbfs =
                peak > 0 ? std::round(20.0 * std::log10(static_cast<double>(peak)) * 100) / 100
                         : -1000.0;
            out.outputLatency = r->outputLatency;
            if (!r->warnings.empty()) out.warnings = r->warnings;
            return out;
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- history
    auto historyResult = [] {
        return schema::object<HistoryResult>("history_result")
            .field("version", &HistoryResult::version, schema::integer())
            .field("label", &HistoryResult::label, schema::string())
            .build();
    };
    for (bool undo : {true, false}) {
        Command<NoArgs, HistoryResult> c;
        c.name = undo ? "history.undo" : "history.redo";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            undo ? "Undo the last change on the current branch." : "Redo the last undone change.";
        c.tags = {"history"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = historyResult();
        c.examples = {{undo ? "Undo" : "Redo", obj("{}"),
                       obj(R"({"version": "<any>", "label": "<any>"})"),
                       undo ? "song" : "song_undone"}};
        c.run = [undo](const NoArgs&, Context& ctx) -> Result<HistoryResult> {
            EngineApp& app = A(ctx);
            SUPPLE_TRY(undo ? app.versions().undo() : app.versions().redo());
            const model::Version* v =
                app.versions().version(app.versions().headVersion(app.versions().branch()));
            app.emit("session.changed", json::Value(static_cast<std::int64_t>(v->id)));
            return HistoryResult{static_cast<std::int64_t>(v->id), v->txn.label};
        };
        registry.add(std::move(c));
    }
    {
        Command<NoArgs, HistoryList> c;
        c.name = "history.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List the current branch's recent versions (newest first).";
        c.tags = {"history"};
        c.args = schema::object<NoArgs>("no_args").build();
        auto entry = schema::object<HistoryEntry>("history_entry")
                         .field("version", &HistoryEntry::version, schema::integer())
                         .field("label", &HistoryEntry::label, schema::string())
                         .field("origin", &HistoryEntry::origin, schema::string())
                         .build();
        c.result = schema::object<HistoryList>("history_list")
                       .field("versions", &HistoryList::versions, schema::arrayOf(entry))
                       .field("can_undo", &HistoryList::canUndo, schema::boolean())
                       .field("can_redo", &HistoryList::canRedo, schema::boolean())
                       .build();
        c.examples = {{"List", obj("{}"),
                       obj(R"({"versions": "<any>", "can_undo": true, "can_redo": false})"),
                       "song"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<HistoryList> {
            EngineApp& app = A(ctx);
            HistoryList out;
            for (const model::Version* v : app.versions().history(20))
                out.versions.push_back(
                    {static_cast<std::int64_t>(v->id), v->txn.label, v->txn.origin});
            out.canUndo = app.versions().canUndo();
            out.canRedo = app.versions().canRedo();
            return out;
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- branches
    struct BranchCreateArgs {
        std::string name;
        std::optional<std::string> from;
        bool switchTo = true;
    };
    struct BranchName {
        std::string name;
    };
    struct BranchList {
        std::vector<std::string> branches;
        std::string current;
    };
    struct BranchDiffArgs {
        std::string a, b;
    };
    struct DiffEntry {
        std::string op, id;
        std::vector<std::string> fields;
    };
    struct BranchDiff {
        std::vector<DiffEntry> changes;
    };
    struct BranchMergeArgs {
        std::string from;
        std::optional<std::string> into;
    };
    struct BranchMergeResult {
        bool fastForward = false, upToDate = false;
        std::int64_t version = 0;
        json::Value remapped;
    };
    auto branchName = [] {
        return schema::string().pattern("^[A-Za-z0-9][A-Za-z0-9._/-]{0,99}$",
                                        "a branch name like agent/claude/darker");
    };
    {
        Command<BranchCreateArgs, BranchName> c;
        c.name = "branch.create";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Start a branch from a branch's head (default: the current one) and switch to it.";
        c.description =
            "Agents: work on agent/<you>/<topic> branches, then branch.diff and branch.merge into "
            "main. "
            "Merging replays your edits onto main even if main moved; conflicting edits are "
            "reported.";
        c.tags = {"branch"};
        c.args = schema::object<BranchCreateArgs>("branch_create_args")
                     .field("name", &BranchCreateArgs::name, branchName())
                     .field("from", &BranchCreateArgs::from, branchName())
                     .field("switch", &BranchCreateArgs::switchTo, schema::boolean())
                     .defaultValue(true)
                     .build();
        c.result = schema::object<BranchName>("branch_name")
                       .field("name", &BranchName::name, schema::string())
                       .build();
        c.errors = {{"branch.exists", "a branch with that name exists",
                     "pick another name or switch to it"}};
        c.examples = {{"An agent branch", obj(R"({"name": "agent/claude/darker"})"),
                       obj(R"({"name": "agent/claude/darker"})"), "song"}};
        c.run = [](const BranchCreateArgs& a, Context& ctx) -> Result<BranchName> {
            EngineApp& app = A(ctx);
            SUPPLE_TRY(
                app.versions().createBranch(a.name, a.from.value_or(app.versions().branch())));
            if (a.switchTo) SUPPLE_TRY(app.switchBranch(a.name));
            return BranchName{a.name};
        };
        registry.add(std::move(c));
    }
    {
        Command<BranchName, BranchName> c;
        c.name = "branch.switch";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Make another branch current (edits, undo and playback follow it).";
        c.tags = {"branch"};
        c.args = schema::object<BranchName>("branch_name")
                     .field("name", &BranchName::name, branchName())
                     .build();
        c.result = schema::object<BranchName>("branch_name")
                       .field("name", &BranchName::name, schema::string())
                       .build();
        c.examples = {
            {"Back to main", obj(R"({"name": "main"})"), obj(R"({"name": "main"})"), "song"}};
        c.run = [](const BranchName& a, Context& ctx) -> Result<BranchName> {
            SUPPLE_TRY(A(ctx).switchBranch(a.name));
            return BranchName{a.name};
        };
        registry.add(std::move(c));
    }
    {
        Command<NoArgs, BranchList> c;
        c.name = "branch.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List branches and the current one.";
        c.tags = {"branch"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = schema::object<BranchList>("branch_list")
                       .field("branches", &BranchList::branches, schema::arrayOf(schema::string()))
                       .field("current", &BranchList::current, schema::string())
                       .build();
        c.examples = {
            {"List", obj("{}"), obj(R"({"branches": ["main"], "current": "main"})"), "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<BranchList> {
            EngineApp& app = A(ctx);
            return BranchList{app.versions().branches(), app.versions().branch()};
        };
        registry.add(std::move(c));
    }
    {
        Command<BranchDiffArgs, BranchDiff> c;
        c.name = "branch.diff";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "What differs between two branch heads: objects added, removed, changed (fields).";
        c.tags = {"branch"};
        c.args = schema::object<BranchDiffArgs>("branch_diff_args")
                     .field("a", &BranchDiffArgs::a, branchName())
                     .field("b", &BranchDiffArgs::b, branchName())
                     .build();
        auto entry = schema::object<DiffEntry>("diff_entry")
                         .field("op", &DiffEntry::op, schema::string())
                         .field("id", &DiffEntry::id, schema::string())
                         .field("fields", &DiffEntry::fields, schema::arrayOf(schema::string()))
                         .build();
        c.result = schema::object<BranchDiff>("branch_diff")
                       .field("changes", &BranchDiff::changes, schema::arrayOf(entry))
                       .build();
        c.examples = {{"Main against itself", obj(R"({"a": "main", "b": "main"})"),
                       obj(R"({"changes": []})"), "song"}};
        c.run = [](const BranchDiffArgs& a, Context& ctx) -> Result<BranchDiff> {
            EngineApp& app = A(ctx);
            auto ha = app.versions().head(a.a), hb = app.versions().head(a.b);
            if (!ha) return Error("branch.unknown", "no branch " + a.a, "/a", "see branch.list");
            if (!hb) return Error("branch.unknown", "no branch " + a.b, "/b", "see branch.list");
            BranchDiff out;
            static constexpr const char* kOps[] = {"add", "remove", "update"};
            for (auto& d : model::diff(*ha, *hb))
                out.changes.push_back(
                    {kOps[static_cast<int>(d.op)], d.id.str(), std::move(d.fields)});
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<BranchMergeArgs, BranchMergeResult> c;
        c.name = "branch.merge";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Merge a branch into another (default: main) by replaying its edits.";
        c.description =
            "Fast-forwards when the target has not moved. Otherwise replays the branch's edits; "
            "IDs it "
            "created are renumbered where needed (see remapped). If both sides changed the same "
            "field "
            "of an object, nothing is merged and the error lists each conflict.";
        c.tags = {"branch"};
        c.args = schema::object<BranchMergeArgs>("branch_merge_args")
                     .field("from", &BranchMergeArgs::from, branchName())
                     .field("into", &BranchMergeArgs::into, branchName())
                     .build();
        c.result = schema::object<BranchMergeResult>("branch_merge")
                       .field("fast_forward", &BranchMergeResult::fastForward, schema::boolean())
                       .field("up_to_date", &BranchMergeResult::upToDate, schema::boolean())
                       .field("version", &BranchMergeResult::version, schema::integer())
                       .field("remapped", &BranchMergeResult::remapped, schema::any())
                       .build();
        c.errors = {{"branch.conflict", "both sides changed the same thing",
                     "details: one per conflict with the object"}};
        c.examples = {
            {"Merge an agent branch", obj(R"({"from": "agent/claude/darker"})"),
             obj(R"({"fast_forward": true, "up_to_date": false, "version": "<any>", "remapped": {}})"),
             "branch_ahead"}};
        c.run = [](const BranchMergeArgs& a, Context& ctx) -> Result<BranchMergeResult> {
            EngineApp& app = A(ctx);
            const std::string into = a.into.value_or("main");
            auto out = app.mergeBranch(ctx.service<const cmd::Registry>(), a.from, into);
            if (!out.conflicts.empty()) {
                Error e("branch.conflict", std::to_string(out.conflicts.size()) +
                                               " conflict(s) merging " + a.from + " into " + into +
                                               "; nothing was merged");
                e.hint = "change one side (or undo the edit on the branch) and merge again";
                for (const auto& cf : out.conflicts)
                    e.details.emplace_back(cf.kind == model::Conflict::Kind::ConcurrentEdit
                                               ? "merge.concurrent_edit"
                                               : "merge.invalid",
                                           cf.message, "/steps/" + std::to_string(cf.step),
                                           cf.object ? "object " + cf.object->str() : "");
                return e;
            }
            BranchMergeResult r;
            r.fastForward = out.fastForward;
            r.upToDate = out.upToDate;
            r.version = static_cast<std::int64_t>(out.head);
            r.remapped = json::Value::object();
            for (const auto& [from, to] : out.remap) r.remapped[from.str()] = to.str();
            return r;
        };
        registry.add(std::move(c));
    }
    {
        Command<BranchName, Empty> c;
        c.name = "branch.delete";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Delete a branch (not main, not the current one).";
        c.tags = {"branch"};
        c.args = schema::object<BranchName>("branch_name")
                     .field("name", &BranchName::name, branchName())
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Delete", obj(R"({"name": "agent/claude/darker"})"), obj("{}"), "branch_ahead"}};
        c.run = [](const BranchName& a, Context& ctx) -> Result<Empty> {
            SUPPLE_TRY(A(ctx).versions().deleteBranch(a.name));
            return Empty{};
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- env.*
    {
        Command<EnvAddArgs, EnvAddResult> c;
        c.name = "env.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Automate a parameter: a track's or route's mixer control, or a plugin's.";
        c.description =
            "Target one of track (param volume|trim|pan|width|mute), route (volume|pan|mute) or "
            "fx (a semantic key such as \"gain\" or \"filter1.cutoff\", or for other plugins a "
            "CLAP id or parameter name). points: [{beat|bar|seconds, value, shape?, tension?}]; "
            "values in the parameter's unit (dB, %, Hz… — text like \"800 Hz\" works for "
            "plugins); shapes linear, square, slow, fast_start, fast_end, bezier. Plugin values "
            "move along the parameter's curve (a log cutoff sweeps evenly in pitch).";
        c.tags = {"automation"};
        c.args = schema::object<EnvAddArgs>("env_add_args")
                     .field("track", &EnvAddArgs::track, idArg("tr", "Track"))
                     .field("fx", &EnvAddArgs::fx, idArg("fx", "Plugin instance"))
                     .field("route", &EnvAddArgs::route, idArg("sd", "Route"))
                     .field("param", &EnvAddArgs::param, schema::string())
                     .field("points", &EnvAddArgs::points, schema::arrayOf(envPointSchema()))
                     .build();
        c.result = schema::object<EnvAddResult>("env_add")
                       .field("env", &EnvAddResult::env, schema::string())
                       .field("points", &EnvAddResult::points, schema::integer())
                       .build();
        c.examples = {
            {"Fade a track out over bar 2",
             obj(R"({"track": "tr_1", "param": "volume", "points": [{"bar": 2, "value": 0}, {"bar": 3, "value": "-60 dB"}]})"),
             obj(R"({"env": "en_1", "points": 2})"), "song"},
            {"Sweep a plugin parameter",
             obj(R"({"fx": "fx_2", "param": "gain", "points": [{"beat": 0, "value": "-12 dB", "shape": "slow"}, {"beat": 4, "value": 0}]})"),
             obj(R"({"env": "en_1", "points": 2})"), "song_with_fx"}};
        c.run = [](const EnvAddArgs& a, Context& ctx) -> Result<EnvAddResult> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            if (a.track.has_value() + a.fx.has_value() + a.route.has_value() != 1)
                return Error("env.target", "give one of track, fx or route", "",
                             "e.g. {\"fx\": \"fx_1\", \"param\": \"filter1.cutoff\"}");
            model::Envelope env;
            if (a.track) {
                auto id = existing<model::Track>(s, *a.track, "/track");
                if (!id) return std::move(id).error();
                env.target.object = *id;
            } else if (a.fx) {
                auto id = existing<model::FxInstance>(s, *a.fx, "/fx");
                if (!id) return std::move(id).error();
                env.target.object = *id;
            } else {
                auto id = existing<model::Route>(s, *a.route, "/route");
                if (!id) return std::move(id).error();
                env.target.object = *id;
            }
            env.target.param = a.param;
            for (const auto& e : s.envelopes)
                if (e.value->target.object == env.target.object && e.value->target.param == a.param)
                    return Error("env.exists", "an envelope already automates this parameter",
                                 "/param", "env.points.set " + e.id.str());
            auto domain = envDomain(app, s, env.target, "/param");
            if (!domain) return std::move(domain).error();
            if (a.points) {
                auto pts = envPoints(s, *domain, *a.points, "/points");
                if (!pts) return std::move(pts).error();
                env.points = std::move(*pts);
            }
            const Id id = app.txn().create(env);
            return EnvAddResult{id.str(), static_cast<std::int64_t>(env.points.size())};
        };
        registry.add(std::move(c));
    }
    {
        Command<EnvPointsSetArgs, EnvCount> c;
        c.name = "env.points.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Replace an envelope's points (all of them, or those in a beat range).";
        c.description = "from_beat/to_beat: replace only points with from_beat <= beat < to_beat.";
        c.tags = {"automation"};
        c.args = schema::object<EnvPointsSetArgs>("env_points_set_args")
                     .field("env", &EnvPointsSetArgs::env, idArg("en", "Envelope"))
                     .field("points", &EnvPointsSetArgs::points, schema::arrayOf(envPointSchema()))
                     .field("from_beat", &EnvPointsSetArgs::fromBeat, schema::number().min(0))
                     .field("to_beat", &EnvPointsSetArgs::toBeat, schema::number().min(0))
                     .build();
        c.result = schema::object<EnvCount>("env_count")
                       .field("points", &EnvCount::points, schema::integer())
                       .build();
        c.examples = {
            {"Three points",
             obj(R"({"env": "en_1", "points": [{"beat": 0, "value": -6}, {"beat": 2, "value": 0, "shape": "square"}, {"beat": 3, "value": "-3 dB"}]})"),
             obj(R"({"points": 3})"), "song_with_env"}};
        c.run = [](const EnvPointsSetArgs& a, Context& ctx) -> Result<EnvCount> {
            EngineApp& app = A(ctx);
            const model::Session s = app.view();
            auto id = existing<model::Envelope>(s, a.env, "/env");
            if (!id) return std::move(id).error();
            const model::Envelope& env = *s.envelopes.find(*id);
            auto domain = envDomain(app, s, env.target, "/env");
            if (!domain) return std::move(domain).error();
            auto pts = envPoints(s, *domain, a.points, "/points");
            if (!pts) return std::move(pts).error();
            std::vector<model::EnvPoint> next;
            if (a.fromBeat || a.toBeat) {
                const double lo = a.fromBeat.value_or(0.0), hi = a.toBeat.value_or(1e300);
                for (const auto& pt : env.points)
                    if (pt.pos < lo || pt.pos >= hi) next.push_back(pt);
            }
            next.insert(next.end(), pts->begin(), pts->end());
            std::stable_sort(next.begin(), next.end(),
                             [](const auto& x, const auto& y) { return x.pos < y.pos; });
            const auto count = static_cast<std::int64_t>(next.size());
            app.txn().modify<model::Envelope>(
                *id, [&](model::Envelope& e) { e.points = std::move(next); });
            return EnvCount{count};
        };
        registry.add(std::move(c));
    }
    {
        Command<EnvSetArgs, Empty> c;
        c.name = "env.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set an envelope's options: active (heard), mode (read, touch, latch, write).";
        c.tags = {"automation"};
        c.args = schema::object<EnvSetArgs>("env_set_args")
                     .field("env", &EnvSetArgs::env, idArg("en", "Envelope"))
                     .field("active", &EnvSetArgs::active, schema::boolean())
                     .field("mode", &EnvSetArgs::mode, envModeSchema())
                     .build();
        c.result = emptySchema();
        c.examples = {{"Mute the automation", obj(R"({"env": "en_1", "active": false})"), obj("{}"),
                       "song_with_env"}};
        c.run = [](const EnvSetArgs& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            auto id = existing<model::Envelope>(app.view(), a.env, "/env");
            if (!id) return std::move(id).error();
            app.txn().modify<model::Envelope>(*id, [&](model::Envelope& e) {
                if (a.active) e.active = *a.active;
                if (a.mode) e.mode = *a.mode;
            });
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<EnvRef, Empty> c;
        c.name = "env.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove an envelope (the parameter returns to its set value).";
        c.tags = {"automation"};
        c.args = schema::object<EnvRef>("env_ref")
                     .field("env", &EnvRef::env, idArg("en", "Envelope"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", obj(R"({"env": "en_1"})"), obj("{}"), "song_with_env"}};
        c.run = [](const EnvRef& a, Context& ctx) -> Result<Empty> {
            EngineApp& app = A(ctx);
            auto id = existing<model::Envelope>(app.view(), a.env, "/env");
            if (!id) return std::move(id).error();
            app.txn().remove<model::Envelope>(*id);
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<EnvListArgs, EnvList> c;
        c.name = "env.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List envelopes (optionally of one track, route or plugin).";
        c.tags = {"automation"};
        c.args = schema::object<EnvListArgs>("env_list_args")
                     .field("object", &EnvListArgs::object, schema::string())
                     .build();
        auto entry = schema::object<EnvEntry>("env_entry")
                         .field("env", &EnvEntry::env, schema::string())
                         .field("target", &EnvEntry::target, schema::string())
                         .field("param", &EnvEntry::param, schema::string())
                         .field("points", &EnvEntry::points, schema::integer())
                         .field("active", &EnvEntry::active, schema::boolean())
                         .field("mode", &EnvEntry::mode, schema::string())
                         .build();
        c.result = schema::object<EnvList>("env_list")
                       .field("envelopes", &EnvList::envelopes, schema::arrayOf(entry))
                       .build();
        c.examples = {
            {"All", obj("{}"),
             obj(R"({"envelopes": [{"env": "en_1", "target": "fx_2", "param": "gain", "points": 2, "active": true, "mode": "read"}]})"),
             "song_with_env"}};
        c.run = [](const EnvListArgs& a, Context& ctx) -> Result<EnvList> {
            const model::Session s = A(ctx).view();
            static constexpr const char* kModes[] = {"trim",  "read",          "touch",
                                                     "latch", "latch_preview", "write"};
            EnvList out;
            for (const auto& e : s.envelopes) {
                const model::Envelope& env = *e.value;
                if (a.object && env.target.object.str() != *a.object) continue;
                out.envelopes.push_back({e.id.str(), env.target.object.str(), env.target.param,
                                         kModes[static_cast<int>(env.mode)],
                                         static_cast<std::int64_t>(env.points.size()), env.active});
            }
            return out;
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- engine.*
    {
        Command<EngineStartArgs, EngineStatus> c;
        c.name = "engine.start";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Start real-time playback on the audio device (jack) or without one (dummy).";
        c.description =
            "backend: jack (default; PipeWire's JACK or a JACK server) or dummy (no device, "
            "paced like one). block: the device block in frames (default 256); the device's "
            "period may differ and is reported. anticipative (default true): render ahead of "
            "the playhead on worker threads (edits are heard up to 200 ms later); false renders "
            "everything in the audio callback. The project must run at the device's rate.";
        c.tags = {"engine"};
        c.args = schema::object<EngineStartArgs>("engine_start_args")
                     .field("backend", &EngineStartArgs::backend,
                            schema::enumeration<BackendArg>(
                                {{BackendArg::Jack, "jack"}, {BackendArg::Dummy, "dummy"}}))
                     .field("block", &EngineStartArgs::block, schema::integer().range(16, 4096))
                     .field("anticipative", &EngineStartArgs::anticipative, schema::boolean())
                     .build();
        c.result = engineStatusSchema();
        c.errors = {{"engine.running", "the engine is already running", "engine.stop first"},
                    {"backend.unavailable", "JACK is not installed", "install pipewire-jack"},
                    {"backend.open", "no JACK server answered", "is PipeWire running?"},
                    {"engine.sample_rate", "the project's rate is not the device's", "match them"}};
        c.examples = {
            {"Without a device", obj(R"({"backend": "dummy", "block": 128})"),
             obj(R"({"running": true, "backend": "dummy", "sample_rate": 48000.0, "block": 128,
                     "period": 128, "plan_latency": 0, "device_latency": 0, "xruns": 0,
                     "cycles": "<any>", "process_errors": 0, "restart_needed": false,
                     "anticipative": true, "anticipation_misses": 0, "failed_fx": []})"),
             "empty"}};
        c.run = [](const EngineStartArgs& a, Context& ctx) -> Result<EngineStatus> {
            EngineApp& app = A(ctx);
            const bool dummy = a.backend.value_or(BackendArg::Jack) == BackendArg::Dummy;
            SUPPLE_TRY(app.startEngine(dummy ? "dummy" : "jack",
                                       static_cast<std::uint32_t>(a.block.value_or(256)),
                                       a.anticipative.value_or(true)));
            return app.engineStatus();
        };
        registry.add(std::move(c));
    }
    {
        Command<NoArgs, Empty> c;
        c.name = "engine.stop";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Stop real-time playback and release the audio device.";
        c.tags = {"engine"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = emptySchema();
        c.examples = {{"Stop", obj("{}"), obj("{}"), "engine_running"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<Empty> {
            A(ctx).stopEngine();
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<NoArgs, EngineStatus> c;
        c.name = "engine.status";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Real-time engine state: backend, rate, block, device period, latency, xruns.";
        c.description =
            "plan_latency: the session's plugin delay in frames; device_latency: frames from "
            "the engine's outputs to the device. restart_needed: the device changed rate or went "
            "away; engine.stop and engine.start again. failed_fx: isolated plugins whose "
            "process crashed or hung (silent now; fx.restart re-creates them).";
        c.tags = {"engine"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = engineStatusSchema();
        c.examples = {{"Not running", obj("{}"),
                       obj(R"({"running": false, "backend": "", "sample_rate": 0.0, "block": 0,
                     "period": 0, "plan_latency": 0, "device_latency": 0, "xruns": 0,
                     "cycles": 0, "process_errors": 0, "restart_needed": false,
                     "anticipative": false, "anticipation_misses": 0, "failed_fx": []})"),
                       "empty"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<EngineStatus> {
            return A(ctx).engineStatus();
        };
        registry.add(std::move(c));
    }
    // ---------------------------------------------------------------- transport.*
    for (const auto& [action, summary, playing] :
         {std::tuple{"play", "Start playback from the current position.", true},
          std::tuple{"pause", "Pause playback where it is (ends a recording).", false},
          std::tuple{"stop", "Stop playback and return to where it started (ends a recording).",
                     false},
          std::tuple{"record", "Play and record every armed track's input.", true}}) {
        Command<NoArgs, TransportInfo> c;
        c.name = std::string("transport.") + action;
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = summary;
        c.tags = {"transport"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = transportSchema();
        c.errors = {{"engine.stopped", "the engine is not running", "engine.start first"}};
        c.errors = {{"engine.stopped", "the engine is not running", "engine.start first"},
                    {"record.nothing_armed", "no armed track has an input",
                     "track.set {armed: true, record_input: [1, 2]}"}};
        const bool rec = std::string(action) == "record";
        c.examples = {{"While the engine runs", obj("{}"),
                       obj(std::string(R"({"playing": )") + (playing ? "true" : "false") +
                           R"(, "recording": )" + (rec ? "true" : "false") +
                           R"(, "seconds": "<any>", "beat": "<any>", "sample": "<any>"})"),
                       rec ? "engine_armed" : "engine_running"}};
        c.run = [act = std::string(action)](const NoArgs&, Context& ctx) -> Result<TransportInfo> {
            return A(ctx).transport(act);
        };
        registry.add(std::move(c));
    }
    {
        Command<SeekArgs, TransportInfo> c;
        c.name = "transport.seek";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Move the play position (one of seconds, beat or bar).";
        c.tags = {"transport"};
        c.args = schema::object<SeekArgs>("seek_args")
                     .field("seconds", &SeekArgs::seconds, schema::number().min(0))
                     .field("beat", &SeekArgs::beat, schema::number().min(0))
                     .field("bar", &SeekArgs::bar, schema::integer().min(1))
                     .build();
        c.result = transportSchema();
        c.errors = {
            {"engine.stopped", "the engine is not running", "engine.start first"},
            {"transport.position", "not exactly one position", "give seconds, beat or bar"}};
        c.examples = {
            {"To bar 2 (120 BPM, 4/4)", obj(R"({"bar": 2})"),
             obj(R"({"playing": false, "recording": false, "seconds": 2.0, "beat": 4.0, "sample": 96000})"),
             "engine_running"}};
        c.run = [](const SeekArgs& a, Context& ctx) -> Result<TransportInfo> {
            EngineApp& app = A(ctx);
            if (a.seconds.has_value() + a.beat.has_value() + a.bar.has_value() != 1)
                return Error("transport.position", "give exactly one position", "",
                             "seconds, beat or bar");
            const model::Session s = app.view();
            const double seconds =
                a.seconds
                    ? *a.seconds
                    : s.timeMap->secondsAt(beatsOf(
                          s, a.beat,
                          a.bar ? std::optional<int>(static_cast<int>(*a.bar)) : std::nullopt));
            return app.transport("seek", seconds);
        };
        registry.add(std::move(c));
    }
    {
        Command<NoArgs, TransportInfo> c;
        c.name = "transport.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "The play position and whether it is playing.";
        c.tags = {"transport"};
        c.args = schema::object<NoArgs>("no_args").build();
        c.result = transportSchema();
        c.errors = {{"engine.stopped", "the engine is not running", "engine.start first"}};
        c.examples = {
            {"Stopped at the start", obj("{}"),
             obj(R"({"playing": false, "recording": false, "seconds": 0.0, "beat": 0.0, "sample": 0})"),
             "engine_running"}};
        c.run = [](const NoArgs&, Context& ctx) -> Result<TransportInfo> {
            return A(ctx).transport("get");
        };
        registry.add(std::move(c));
    }
}

// --- describe ----------------------------------------------------------------------------------

std::string describeSession(const model::Session& s) {
    std::string out;
    const auto meter = s.timeMap->meterAt(1);
    double endBeat = 0;
    for (const auto& e : s.items) endBeat = std::max(endBeat, e.value->position + e.value->length);
    char head[256];
    std::snprintf(head, sizeof head, "Project \"%s\" · %g kHz · %g BPM %d/%d · %.1f s\n",
                  s.project->name.c_str(), s.project->sampleRate / 1000.0, s.timeMap->bpmAt(0),
                  meter.numerator, meter.denominator, s.timeMap->secondsAt(endBeat));
    out += head;
    auto barOf = [&](double beat) {
        const auto bb = s.timeMap->barBeat(beat);
        char b[48];
        if (bb.beat == 1 && bb.fraction == 0)
            std::snprintf(b, sizeof b, "bar %d", bb.bar);
        else
            std::snprintf(b, sizeof b, "bar %d.%d", bb.bar, bb.beat);
        return std::string(b);
    };
    std::function<void(Id, int)> walk = [&](Id id, int depth) {
        const model::Track& t = *s.tracks.find(id);
        if (id != model::kMaster) {
            std::string line(static_cast<std::size_t>(2 * (depth - 1)), ' ');
            line += id.str() + " " + (t.name.empty() ? "(unnamed)" : t.name);
            if (!t.children.empty()) line += " [folder]";
            if (t.volumeDb != 0) {
                char v[32];
                std::snprintf(v, sizeof v, " · vol %+.1f dB", t.volumeDb);
                line += v;
            }
            if (t.pan != 0) line += " · pan " + std::to_string(static_cast<int>(t.pan));
            if (t.mute) line += " · muted";
            if (t.solo == model::Solo::On || t.solo == model::Solo::InPlace) line += " · solo";
            if (!t.chain.empty()) {
                line += " · fx:";
                for (Id f : t.chain) {
                    const model::FxInstance& fx = *s.fx.find(f);
                    line += " " + f.str() + " " +
                            (fx.plugin.name.empty() ? fx.plugin.id : fx.plugin.name);
                    if (fx.bypass) line += " (bypassed)";
                }
            }
            out += line + "\n";
            for (const auto& e : s.items) {
                if (e.value->track != id) continue;
                const model::Item& it = *e.value;
                std::string il(static_cast<std::size_t>(2 * depth + 2), ' ');
                il += e.id.str() + " " + barOf(it.position) + "–" + barOf(it.position + it.length);
                const model::Take* take = it.activeTake ? s.takes.find(*it.activeTake) : nullptr;
                if (take && take->source) {
                    if (take->source->kind == model::Kind::Midi) {
                        const model::MidiSource* m = s.midi.find(*take->source);
                        il += " (MIDI " + take->source->str() + ", " +
                              std::to_string(m ? m->sequence.notes.size() : 0) + " notes)";
                    } else {
                        const model::MediaSource* m = s.media.find(*take->source);
                        il += " (audio " + take->source->str() + " " +
                              (m ? fs::path(m->path).filename().string() : "") + ")";
                    }
                }
                if (it.mute) il += " muted";
                out += il + "\n";
            }
        }
        for (Id c : t.children) walk(c, depth + 1);
    };
    walk(model::kMaster, 0);
    if (!s.routes.empty()) {
        out += "routes:";
        for (const auto& e : s.routes) {
            const model::Route& r = *e.value;
            out += " " + e.id.str() + " " + (r.from.track ? r.from.track->str() : "in") + "→" +
                   (r.to.track ? r.to.track->str() : "out");
        }
        out += "\n";
    }
    if (!s.markers.empty()) {
        out += "markers:";
        for (const auto& e : s.markers)
            out += " " + e.id.str() + " \"" + e.value->name + "\" " + barOf(e.value->pos);
        out += "\n";
    }
    return out;
}

}  // namespace supple::engine
