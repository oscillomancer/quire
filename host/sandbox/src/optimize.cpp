// optimize.start (docs/specs/optimizer.md §2–§4): tunes continuous parameters
// of a semantic plugin instance with CMA-ES against a reference render and/or
// an analysis profile. Runs as a job on a worker thread that owns a clone of
// the instance; the best candidate's render joins the session afterwards.
#include "optimize_commands.h"

#include "schema_params.h"

#include <chrono>
#include <cmath>
#include <deque>

#include "supple/analysis/analysis.h"
#include "supple/audio/audio_file.h"
#include "supple/base/closest.h"
#include "supple/notation/notation.h"
#include "supple/opt/cmaes.h"
#include "supple/params/params.h"

namespace supple::sandbox {

using cmd::Command;
using cmd::Context;
using cmd::Exposure;
using cmd::Kind;

namespace {

json::Value obj(std::string_view text) { return json::parse(text).value(); }
Session& S(Context& ctx) { return ctx.service<Session>(); }

constexpr int kMaxParams = 32;
constexpr double kInf = std::numeric_limits<double>::infinity();

struct OptimizeArgs {
    std::string instance;
    json::Value params = json::Value("auto");
    json::Value target = json::Value::object();
    std::optional<std::string> phrase;
    std::optional<double> seconds, tailS;
    std::optional<std::int64_t> evaluations;
    std::optional<double> maxSeconds;
    std::int64_t seed = 0;
};
struct JobStarted {
    std::string job, status;
    std::vector<std::string> params;
    std::string render;  // id the best candidate's render will have
};

// One tuned parameter: its spec (for the normalized mapping), search range
// in the normalized domain, and starting value.
struct Tuned {
    std::string key, unit;
    params::ParamSpec spec;
    double lo = 0.0, hi = 1.0;  // normalized sub-range
    double initial = 0.0;       // physical
    double toPhysical(double u) const {
        return params::fromNormalized(spec, lo + (hi - lo) * std::clamp(u, 0.0, 1.0));
    }
    double toUnit(double physical) const {
        const double n = params::toNormalized(spec, physical);
        return hi > lo ? std::clamp((n - lo) / (hi - lo), 0.0, 1.0) : 0.5;
    }
};

bool hasFlag(const json::Value& entry, std::string_view flag) {
    if (const auto* f = entry.find("flags"); f && f->isArray())
        for (const auto& v : f->asArray())
            if (v.isString() && v.asString() == flag) return true;
    return false;
}

bool tunable(const json::Value& e) {
    const auto* type = e.find("type");
    const auto* active = e.find("active");
    return type && type->isString() && type->asString() == "float" && hasFlag(e, "modulatable") &&
           !hasFlag(e, "readonly") && !(active && active->isBool() && !active->asBool());
}

Result<std::vector<Tuned>> resolveParams(Session& s, const std::string& instance,
                                         const json::Value& schema, const json::Value& request) {
    const json::Value* table = schema.find("params");
    if (!table || !table->isObject())
        return Error("optimize.not_semantic", "the plugin publishes no parameter schema",
                     "/instance", "optimize needs a plugin with the semantic-params extension");
    std::vector<std::pair<std::string, json::Value>> wanted;  // key, request entry
    if (request.isString() && request.asString() == "auto") {
        // "auto" leaves out performance and placement parameters: voice.*,
        // pans, envelope curves/delay/hold, velocity sensitivity, LFO fades
        // and the output level (loudness is matched, not optimized).
        auto excluded = [](std::string_view k) {
            auto ends = [&](std::string_view suffix) {
                return k.size() >= suffix.size() && k.substr(k.size() - suffix.size()) == suffix;
            };
            return k.rfind("voice.", 0) == 0 || k == "output.level" || ends(".pan") ||
                   ends("_curve") || ends(".delay") || ends(".hold") || ends(".velocity") ||
                   ends(".fade");
        };
        for (const auto& m : table->asObject())
            if (tunable(m.value) && !excluded(m.key))
                wanted.emplace_back(m.key, json::Value::object());
    } else if (request.isArray()) {
        for (std::size_t i = 0; i < request.size(); ++i) {
            const json::Value& r = request.at(i);
            if (r.isString())
                wanted.emplace_back(r.asString(), json::Value::object());
            else if (r.isObject() && r.find("key") && r.find("key")->isString())
                wanted.emplace_back(r.find("key")->asString(), r);
            else
                return Error("optimize.params", "each parameter is a key or {key, min?, max?}",
                             "/params/" + std::to_string(i),
                             R"(e.g. ["filter1.cutoff", {"key": "env2.decay", "max": 800}])");
        }
    } else {
        return Error("optimize.params", "params is \"auto\" or a list of keys", "/params",
                     R"("auto", or ["filter1.cutoff", "filter1.resonance"])");
    }
    if (wanted.empty())
        return Error("optimize.params", "no tunable parameters", "/params",
                     "continuous, modulatable parameters of slots present in the patch");
    if (wanted.size() > static_cast<std::size_t>(kMaxParams))
        return Error("optimize.too_many",
                     std::to_string(wanted.size()) + " parameters (at most 32)", "/params",
                     "name the parameters that matter instead of \"auto\"");
    std::vector<std::string> keys;
    for (const auto& [k, r] : wanted) keys.push_back(k);
    auto current = s.paramGet(instance, keys);
    if (!current) return std::move(current).error().at("/params");
    std::vector<Tuned> out;
    std::vector<std::string_view> names;
    for (const auto& m : table->asObject()) names.push_back(m.key);
    for (std::size_t i = 0; i < wanted.size(); ++i) {
        const auto& [key, req] = wanted[i];
        const std::string path = "/params/" + std::to_string(i);
        const json::Value* e = table->find(key);
        if (!e) {
            const std::string best = closestMatch(key, names);
            return Error("optimize.unknown_param", "unknown parameter \"" + key + "\"", path,
                         best.empty() ? "see plugin.describe" : "did you mean \"" + best + "\"?");
        }
        if (!tunable(*e))
            return Error(
                "optimize.not_tunable",
                key + " is not tunable (continuous, modulatable and active are required)", path,
                "structure and stepped values are the agent's choice, not the optimizer's");
        Tuned t;
        t.key = key;
        const SchemaParam sp = schemaParam(*e);
        t.spec = sp.spec;
        t.unit = sp.unitName;
        const json::Value* cur = current->find(key);
        t.initial = cur && cur->find("value") && cur->find("value")->isNumber()
                        ? cur->find("value")->asDouble()
                        : t.spec.min;
        auto bound = [&](const char* name, double def) -> Result<double> {
            const json::Value* v = req.find(name);
            if (!v) return def;
            auto phys = v->isNumber()   ? Result<double>(v->asDouble())
                        : v->isString() ? units::parseQuantity(v->asString(), t.spec.unit)
                                        : Result<double>(Error("optimize.range", "not a number"));
            if (!phys) return std::move(phys).error().at(path + "/" + name);
            if (*phys < t.spec.min || *phys > t.spec.max)
                return Error("optimize.range",
                             std::string(name) + " is outside the parameter's range",
                             path + "/" + name,
                             units::formatQuantity(t.spec.min, t.spec.unit) + " .. " +
                                 units::formatQuantity(t.spec.max, t.spec.unit));
            return params::toNormalized(t.spec, *phys);
        };
        auto lo = bound("min", 0.0), hi = bound("max", 1.0);
        if (!lo) return std::move(lo).error();
        if (!hi) return std::move(hi).error();
        t.lo = std::min(*lo, *hi);
        t.hi = std::max(*lo, *hi);
        out.push_back(std::move(t));
    }
    return out;
}

// --- loss (optimizer.md §3) -------------------------------------------------------------------

struct Loss {
    double total = std::numeric_limits<double>::infinity();
    std::map<std::string, double> terms;  // contribution w·(d/scale)² per term
};

void term(Loss& l, const std::string& name, double d, double weight, double scale) {
    if (!std::isfinite(d)) return;
    const double c = weight * (d / scale) * (d / scale);
    l.terms[name] += c;
    l.total += c;
}

Loss referenceLoss(const analysis::Report& ref, const analysis::Report& cand) {
    Loss l;
    l.total = 0.0;
    if (ref.spectral && cand.spectral) {
        term(l, "ltas", analysis::compare(ref, cand).ltasRmsDb, 1.0, 3.0);
        double sq = 0.0;
        for (std::size_t b = 0; b < ref.spectral->bands.size(); ++b) {
            const double d = cand.spectral->bands[b].levelDb - ref.spectral->bands[b].levelDb;
            sq += d * d;
        }
        term(l, "bands", std::sqrt(sq / static_cast<double>(ref.spectral->bands.size())), 0.5, 2.0);
        const double c = cand.spectral->centroidHz.median, cr = ref.spectral->centroidHz.median;
        if (c > 0.0 && cr > 0.0) term(l, "centroid", std::fabs(std::log2(c / cr)), 0.5, 0.25);
        term(l, "flatness",
             std::fabs(cand.spectral->flatness.median - ref.spectral->flatness.median), 0.3, 0.05);
        // Brightness over time (filter envelopes): the centroid tracks compared
        // point by point (both renders play the same phrase).
        const auto& rt = ref.spectral->centroidTrack;
        const auto& ct = cand.spectral->centroidTrack;
        if (rt.size() >= 4 && ct.size() >= 4) {
            const std::size_t n = std::min<std::size_t>(100, std::min(rt.size(), ct.size()));
            double sqT = 0.0;
            for (std::size_t i = 0; i < n; ++i) {
                const double a = rt[i * rt.size() / n], b = ct[i * ct.size() / n];
                if (a > 0.0 && b > 0.0) sqT += std::pow(std::log2(b / a), 2.0);
            }
            term(l, "trajectory", std::sqrt(sqT / static_cast<double>(n)), 1.0, 0.1);
        }
        term(l, "flux", std::fabs(cand.spectral->flux.median - ref.spectral->flux.median), 0.3,
             0.02);
    }
    if (ref.envelope && cand.envelope) {
        std::vector<double> rel;
        rel.push_back(std::fabs(cand.envelope->attackMs - ref.envelope->attackMs) /
                      std::max(10.0, ref.envelope->attackMs));
        if (ref.envelope->decay20Ms && cand.envelope->decay20Ms)
            rel.push_back(std::fabs(*cand.envelope->decay20Ms - *ref.envelope->decay20Ms) /
                          std::max(10.0, *ref.envelope->decay20Ms));
        else if (ref.envelope->decay20Ms.has_value() != cand.envelope->decay20Ms.has_value())
            rel.push_back(1.0);
        if (ref.envelope->releaseMs && cand.envelope->releaseMs)
            rel.push_back(std::fabs(*cand.envelope->releaseMs - *ref.envelope->releaseMs) /
                          std::max(10.0, *ref.envelope->releaseMs));
        double sum = 0.0;
        for (double r : rel) sum += r;
        term(l, "envelope", sum / static_cast<double>(rel.size()), 1.0, 0.3);
    }
    if (ref.harmonic && cand.harmonic && ref.harmonic->pitched && cand.harmonic->pitched) {
        double sq = 0.0;
        int n = 0;
        for (std::size_t k = 1;
             k < 8 && k < ref.harmonic->harmonicsDb.size() && k < cand.harmonic->harmonicsDb.size();
             ++k) {
            const double d = cand.harmonic->harmonicsDb[k] - ref.harmonic->harmonicsDb[k];
            sq += d * d;
            ++n;
        }
        if (n) term(l, "harmonics", std::sqrt(sq / n), 0.5, 3.0);
        term(l, "inharmonicity",
             std::fabs(cand.harmonic->inharmonicity - ref.harmonic->inharmonicity), 0.3, 0.01);
    }
    if (ref.stereo && cand.stereo)
        term(l, "stereo", std::fabs(cand.stereo->width - ref.stereo->width), 0.2, 0.1);
    return l;
}

// Loudness-matches the candidate to `targetLufs` (a gain on the analysis
// input only) and analyzes it.
Result<analysis::Report> analyzeMatched(const audio::AudioData& a, std::optional<double> targetLufs,
                                        std::optional<double> noteOff) {
    std::vector<const float*> ptr;
    for (const auto& c : a.channels) ptr.push_back(c.data());
    auto signal = [&](const std::vector<const float*>& p) {
        return analysis::Signal{
            std::span<const float* const>(p.data(), std::min<std::size_t>(p.size(), 2)),
            static_cast<std::int64_t>(a.channels.empty() ? 0 : a.channels[0].size()), a.sampleRate,
            p.size() >= 2 ? analysis::Layout::Stereo : analysis::Layout::Mono};
    };
    analysis::Options o;
    o.noteOffSeconds = noteOff;
    o.pitchFrames = 40;  // repeated analysis: a subsampled pitch track is enough
    o.monoLoss = false;
    if (!targetLufs) return analysis::analyze(signal(ptr), o);
    analysis::Options lo;
    lo.spectral = lo.harmonic = lo.envelope = lo.stereo = false;
    auto first = analysis::analyze(signal(ptr), lo);
    if (!first) return std::move(first).error();
    const double lufs = first->loudness ? first->loudness->integrated : -kInf;
    if (!std::isfinite(lufs)) return analysis::analyze(signal(ptr), o);
    const auto gain = static_cast<float>(std::pow(10.0, (*targetLufs - lufs) / 20.0));
    std::vector<std::vector<float>> scaled = a.channels;
    for (auto& c : scaled)
        for (float& v : c) v *= gain;
    std::vector<const float*> sp;
    for (const auto& c : scaled) sp.push_back(c.data());
    return analysis::analyze(signal(sp), o);
}

json::Value roundTo(double v, int decimals) {
    const double s = std::pow(10.0, decimals);
    return std::isfinite(v) ? json::Value(std::round(v * s) / s + 0.0) : json::Value(nullptr);
}

}  // namespace

void registerOptimizeCommands(cmd::Registry& registry) {
    Command<OptimizeArgs, JobStarted> c;
    c.name = "optimize.start";
    c.kind = Kind::Action;
    c.exposure = Exposure::McpTool;
    c.summary = "Tune continuous parameters toward a reference sound or a profile (a job).";
    c.description =
        "You choose the structure (patch) and which knobs matter; CMA-ES tunes the numbers. "
        "params: \"auto\" (continuous parameters of slots present, max 32) or keys, optionally "
        "{key, min, max} to narrow the search. target: {\"reference\": rd_<n> or file} and/or "
        "{\"profile\": supple.analysis-profile document}. phrase: notation rendered per candidate "
        "(default \"1: C3:w\"; use the reference's phrase when matching a reference). Returns a "
        "job: "
        "poll job.wait; the result has the best loss per term, changes, a merge patch "
        "({\"params\": {...}}) for patch.merge, and the best candidate's render.";
    c.tags = {"optimize"};
    c.args =
        schema::object<OptimizeArgs>("optimize_args")
            .field("instance", &OptimizeArgs::instance, schema::string().minLength(1))
            .field("params", &OptimizeArgs::params,
                   schema::any().doc("\"auto\" or a list of keys / {key, min, max}."))
            .defaultValue(json::Value("auto"))
            .field("target", &OptimizeArgs::target, schema::any().doc("{reference?, profile?}"))
            .field("phrase", &OptimizeArgs::phrase, schema::string())
            .field("seconds", &OptimizeArgs::seconds, schema::number().exclusiveMin(0).max(30))
            .field("tail_s", &OptimizeArgs::tailS, schema::number().range(0, 10))
            .field("evaluations", &OptimizeArgs::evaluations, schema::integer().range(10, 5000))
            .field("max_seconds", &OptimizeArgs::maxSeconds, schema::number().range(1, 600))
            .field("seed", &OptimizeArgs::seed, schema::integer().range(0, 1000000000))
            .defaultValue(std::int64_t{0})
            .build();
    c.result = schema::object<JobStarted>("optimize_started")
                   .field("job", &JobStarted::job, schema::string())
                   .field("status", &JobStarted::status, schema::string())
                   .field("params", &JobStarted::params, schema::arrayOf(schema::string()))
                   .field("render", &JobStarted::render, schema::string())
                   .build();
    c.errors = {
        {"optimize.not_semantic", "the plugin has no semantic schema", "use a stock plugin"},
        {"optimize.target", "no reference or profile", R"({"reference": "rd_1"})"},
        {"optimize.too_many", "more than 32 parameters", "name the ones that matter"}};
    c.examples = {
        {"Tune toward a profile", obj(R"({"instance": "in_2", "evaluations": 20, "max_seconds": 5,
                           "target": {"profile": {"schema": "supple.analysis-profile/1.0",
                                      "targets": {"spectral.centroid_hz.median": {"target": 800, "tolerance": 100}}}}})"),
         obj(R"({"job": "<any>", "status": "running", "params": "<any>", "render": "<any>"})"),
         "synth_loaded"}};
    c.run = [](const OptimizeArgs& a, Context& ctx) -> Result<JobStarted> {
        Session& s = S(ctx);
        auto inst = s.instance(a.instance);
        if (!inst) return std::move(inst).error();
        auto schema = (*inst)->semanticSchema();
        if (!schema)
            return Error("optimize.not_semantic", "the plugin has no semantic parameters",
                         "/instance", "optimize works with plugins that publish a semantic schema");
        auto tuned = resolveParams(s, a.instance, *schema, a.params);
        if (!tuned) return std::move(tuned).error();
        // Target.
        const json::Value* ref = a.target.find("reference");
        const json::Value* profile = a.target.find("profile");
        if (!a.target.isObject() || (!ref && !profile))
            return Error("optimize.target", "target needs a reference and/or a profile", "/target",
                         R"({"reference": "rd_1"} or {"profile": {...}})");
        std::optional<analysis::Report> refReport;
        std::optional<double> refLufs;
        if (ref) {
            if (!ref->isString())
                return Error("optimize.target", "reference is a render id or a path",
                             "/target/reference");
            std::filesystem::path file = ref->asString();
            std::optional<double> noteOff;
            if (ref->asString().rfind("rd_", 0) == 0) {
                auto r = s.findRender(ref->asString());
                if (!r) return std::move(r).error().at("/target/reference");
                file = (*r)->path;
                noteOff = (*r)->seconds - (*r)->tailSeconds;
            }
            auto audio = audio::readAudio(file);
            if (!audio) return std::move(audio).error().at("/target/reference");
            auto rep = analyzeMatched(*audio, std::nullopt, noteOff);
            if (!rep) return std::move(rep).error().at("/target/reference");
            refReport = std::move(*rep);
            if (refReport->loudness && std::isfinite(refReport->loudness->integrated))
                refLufs = refReport->loudness->integrated;
        }
        std::optional<json::Value> profileDoc;
        if (profile) {
            // Validate once against an empty report shape by evaluating the reference (or later).
            profileDoc = *profile;
        }
        // Phrase.
        RenderInput input;
        auto compiled = notation::compile(a.phrase.value_or("1: C3:w"));
        if (!compiled) {
            Error e = std::move(compiled).error();
            e.path = "/phrase";
            return e;
        }
        input.tempo = compiled->tempo;
        input.notes = std::move(compiled->sequence);
        input.seconds = a.seconds;
        input.tailSeconds = a.tailS.value_or(1.0);
        // Worker resources.
        auto clone = s.cloneInstance(a.instance);
        if (!clone) return std::move(clone).error();
        auto& jobs = ctx.service<cmd::JobManager>();
        const std::string job = jobs.create();
        RenderRecord best = s.reserveRender("optimize " + job);
        opt::Options options;
        options.seed = static_cast<std::uint64_t>(a.seed);
        options.maxEvaluations = static_cast<int>(a.evaluations.value_or(300));
        options.maxSeconds = a.maxSeconds.value_or(60.0);
        JobStarted out{job, "running", {}, best.id};
        for (const auto& t : *tuned) out.params.push_back(t.key);

        struct Work {
            std::shared_ptr<host::Instance> instance;
            std::vector<Tuned> tuned;
            std::optional<analysis::Report> refReport;
            std::optional<double> refLufs;
            std::optional<json::Value> profile;
            RenderInput input;
            SessionSettings settings;
            opt::Options options;
            RenderRecord best;
            std::string job;
            cmd::JobManager* jobs;
            Session* session;
        };
        auto w = std::make_shared<Work>(Work{std::shared_ptr<host::Instance>(std::move(*clone)),
                                             std::move(*tuned), std::move(refReport), refLufs,
                                             std::move(profileDoc), std::move(input), s.settings(),
                                             options, std::move(best), job, &jobs, &s});
        s.startJob([w](std::stop_token stop) {
            Work& W = *w;
            const auto noteOff =
                W.input.seconds;  // explicit length, else the notes' end (unknown here)
            auto candidate = [&](const opt::Vec& u, Loss* terms, audio::AudioData* keep) -> double {
                json::Value merge = json::Value::object();
                json::Value& p = merge["params"];
                p = json::Value::object();
                for (std::size_t i = 0; i < W.tuned.size(); ++i)
                    p[W.tuned[i].key] = W.tuned[i].toPhysical(u[i]);
                if (!W.instance->setPatch(json::writeCompact(merge), CLAP_SUPPLE_SET_MERGE))
                    return kInf;
                host::Instance* chain[1] = {W.instance.get()};
                auto r = Session::renderThrough(chain, W.input, W.settings);
                if (!r) return kInf;
                const double inputEnd =
                    (static_cast<double>(r->audio.frames()) - static_cast<double>(r->tailSamples)) /
                    W.settings.sampleRate;
                auto rep = analyzeMatched(r->audio, W.refLufs,
                                          noteOff ? noteOff : std::optional<double>(inputEnd));
                if (!rep) return kInf;
                Loss l;
                l.total = 0.0;
                if (W.refReport) l = referenceLoss(*W.refReport, *rep);
                if (W.profile) {
                    auto e = analysis::evaluate(*rep, *W.profile);
                    if (!e) return kInf;
                    for (const auto& t : e->targets)
                        term(l, "profile." + t.key, t.distance, 1.0, 1.0);
                }
                if (terms) *terms = l;
                if (keep) *keep = std::move(r->audio);
                return std::isfinite(l.total) ? l.total : kInf;
            };
            opt::Vec x0;
            for (const auto& t : W.tuned) x0.push_back(t.toUnit(t.initial));
            auto batch = [&](const std::vector<opt::Vec>& xs) {
                std::vector<double> losses;
                for (const auto& x : xs)
                    losses.push_back(stop.stop_requested() ? kInf : candidate(x, nullptr, nullptr));
                return losses;
            };
            const opt::Outcome o = opt::minimize(batch, x0, W.options, [&](const opt::Progress& p) {
                json::Value extra = json::Value::object();
                extra["best_loss"] = roundTo(p.bestLoss, 4);
                extra["evaluations"] = p.evaluations;
                (void)W.jobs->progress(
                    W.job,
                    std::min(1.0, static_cast<double>(p.evaluations) / W.options.maxEvaluations),
                    "generation " + std::to_string(p.generation), std::move(extra));
                return !stop.stop_requested() && !W.jobs->cancelled(W.job);
            });
            if (stop.stop_requested() || W.jobs->cancelled(W.job)) return;
            // The best candidate: loss terms, render, changes.
            Loss bestTerms;
            audio::AudioData bestAudio;
            const double bestLoss = candidate(o.best, &bestTerms, &bestAudio);
            json::Value result = json::Value::object();
            json::Value loss = json::Value::object();
            loss["initial"] = roundTo(o.initialLoss, 4);
            loss["best"] = roundTo(bestLoss, 4);
            json::Value terms = json::Value::object();
            for (const auto& [k, v] : bestTerms.terms) terms[k] = roundTo(v, 4);
            loss["terms"] = std::move(terms);
            result["loss"] = std::move(loss);
            result["evaluations"] = o.evaluations;
            result["seconds"] = roundTo(o.seconds, 2);
            result["stopped"] = std::string(opt::stopName(o.stopped));
            json::Value patch = json::Value::object(), changes = json::Value::array();
            patch["params"] = json::Value::object();
            for (std::size_t i = 0; i < W.tuned.size(); ++i) {
                const double v = W.tuned[i].toPhysical(o.best[i]);
                patch["params"][W.tuned[i].key] = roundTo(v, 6);
                json::Value ch = json::Value::object();
                ch["key"] = W.tuned[i].key;
                ch["from"] = roundTo(W.tuned[i].initial, 4);
                ch["to"] = roundTo(v, 4);
                if (!W.tuned[i].unit.empty()) ch["unit"] = W.tuned[i].unit;
                changes.push_back(std::move(ch));
            }
            result["patch"] = std::move(patch);
            result["changes"] = std::move(changes);
            if (!bestAudio.channels.empty()) {
                RenderRecord rec = W.best;
                rec.sampleRate = bestAudio.sampleRate;
                rec.frames = static_cast<std::int64_t>(bestAudio.frames());
                rec.seconds = bestAudio.seconds();
                double peak = 0.0;
                for (const auto& ch : bestAudio.channels)
                    for (float v : ch) peak = std::max(peak, std::fabs(static_cast<double>(v)));
                rec.peakDbfs = peak > 0.0 ? 20.0 * std::log10(peak) : -1000.0;
                audio::WavOptions wav;
                wav.format = audio::SampleFormat::Float32;
                if (audio::writeWav(rec.path, bestAudio, wav)) {
                    result["render"] = rec.id;
                    W.session->submitRender(std::move(rec));
                }
            }
            result["applied"] = false;
            (void)W.jobs->complete(W.job, std::move(result));
        });
        return out;
    };
    registry.add(std::move(c));
}

}  // namespace supple::sandbox
