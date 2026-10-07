// Sandbox analysis commands (sandbox.md §4): analyze, compare, masking,
// profile.evaluate — over renders (rd_<n>) or audio files.
#include "analysis_commands.h"
#include "../../analysis-commands/analysis_shared.h"
#include "optimize_loss.h"
#include "source_ref.h"

#include "supple/analysis/analysis.h"
#include "supple/audio/audio_file.h"

namespace supple::sandbox {

using cmd::Command;
using cmd::Context;
using cmd::Exposure;
using cmd::Kind;

namespace {

json::Value obj(std::string_view text) { return json::parse(text).value(); }
Session& S(Context& ctx) { return ctx.service<Session>(); }

using analysis_commands::DetailArg;
using analysis_commands::Loaded;

// "rd_<n>" names a render; anything else is a file path.
Result<Loaded> load(Session& s, const std::string& ref, std::string_view path) {
    Loaded l;
    std::filesystem::path file = ref;
    if (isRenderRef(ref)) {
        auto r = s.findRender(ref);
        if (!r) return std::move(r).error().at(std::string(path));
        file = (*r)->path;
        l.noteOff = (*r)->seconds - (*r)->tailSeconds;
    }
    auto a = audio::readAudio(file);
    if (!a) {
        Error e = std::move(a).error();
        e.path = std::string(path);
        if (e.hint.empty()) e.hint = "a render id (rd_1) or a WAV/FLAC/MP3 path";
        return e;
    }
    l.data = std::move(*a);
    return l;
}

// A session render knows where its input ended (its length minus the rendered
// tail): by id, or by the path it was written to.
const RenderRecord* sessionRender(const Session& s, const std::string& source) {
    if (isRenderRef(source)) {
        auto r = s.findRender(source);
        return r ? *r : nullptr;
    }
    const auto path = std::filesystem::path(source).lexically_normal();
    for (const auto& [id, record] : s.renders()) {
        (void)id;
        if (record.path.lexically_normal() == path) return &record;
    }
    return nullptr;
}

// Note-off for one side of a scored comparison: the render's own input end,
// else the caller's argument (a plain file has none unless one is given).
std::optional<double> noteOffFor(const Session& s, const std::string& source,
                                 const std::optional<double>& given) {
    if (const RenderRecord* record = sessionRender(s, source))
        return record->seconds - record->tailSeconds;
    return given;
}

struct AnalyzeArgs {
    std::string source;
    DetailArg detail = DetailArg::Summary;
    std::optional<double> noteOffS;
};
struct AnalyzeResult {
    json::Value report;
    std::string summary;
};
struct CompareArgs {
    std::string reference, test;
    std::optional<double> referenceNoteOffS, testNoteOffS;
};
using analysis_commands::CompareResult;

// `compare`'s result: the shared comparison plus the scored reference loss
// (optimize_loss.h) — the scalar the optimizer minimizes and the evaluation
// scorer judges.
struct CompareScoredResult {
    json::Value notable;
    double ltasRmsDb = 0;
    std::string summary;
    json::Value loss;
};

// {total, terms, note_off_s: {reference, test}}, plus note_off_assumed when
// one side had no note-off of its own and borrowed the other's.
json::Value lossJson(const detail::ReferenceScore& score, std::optional<double> referenceNoteOff,
                     std::optional<double> testNoteOff, bool assumed) {
    json::Value loss = json::Value::object();
    loss["total"] = score.loss.total;
    json::Value terms = json::Value::object();
    for (const auto& [key, value] : score.loss.terms) terms[key] = value;
    loss["terms"] = std::move(terms);
    json::Value ends = json::Value::object();
    ends["reference"] = referenceNoteOff ? json::Value(*referenceNoteOff) : json::Value(nullptr);
    ends["test"] = testNoteOff ? json::Value(*testNoteOff) : json::Value(nullptr);
    loss["note_off_s"] = std::move(ends);
    if (assumed) loss["note_off_assumed"] = true;
    return loss;
}

schema::Schema<CompareScoredResult> compareScoredSchema() {
    return schema::object<CompareScoredResult>("comparison")
        .field("notable", &CompareScoredResult::notable, schema::any())
        .field("ltas_rms_db", &CompareScoredResult::ltasRmsDb, schema::number())
        .field("summary", &CompareScoredResult::summary, schema::string())
        .field("loss", &CompareScoredResult::loss,
               schema::any().doc("The score the optimizer minimizes: total, per-term "
                                 "contributions, and the note-off_s used per side."))
        .build();
}
struct MaskingArgs {
    std::string masker, maskee;
};
struct JsonResult {
    json::Value result;
};
struct ProfileArgs {
    std::string source;
    json::Value profile;
};

schema::Schema<std::string> sourceField() {
    return schema::string().minLength(1).doc("A render id (rd_1) or an audio file path.");
}

}  // namespace

Result<json::Value> analyzeRender(Session& s, const std::string& render, analysis::Detail detail) {
    auto l = load(s, render, "/render");
    if (!l) return std::move(l).error();
    analysis::Options o;
    o.noteOffSeconds = l->noteOff;
    auto r = analysis::analyze(l->signal(), o);
    if (!r) return std::move(r).error();
    return analysis_commands::reportJson(*r, detail);
}

void registerAnalysisCommands(cmd::Registry& registry) {
    // analyze
    {
        Command<AnalyzeArgs, AnalyzeResult> c;
        c.name = "analyze";
        c.kind = Kind::Query;
        c.exposure = Exposure::McpTool;
        c.summary = "Analyze a render or audio file: loudness, spectrum, pitch, envelope, stereo.";
        c.description =
            "source: rd_<n> or a file path. Report supple.analysis-report/1.0: loudness (LUFS, "
            "LRA, "
            "true peak), spectral (centroid, rolloff, flatness, tilt, named band levels relative "
            "to "
            "the total), harmonic (f0, note, harmonics in dB relative to the fundamental), "
            "envelope "
            "(attack, decay to -20 dB, release after note-off, onsets), stereo (width, "
            "correlation). "
            "note_off_s defaults to the end of a render's input. Envelope values (attack, decay, "
            "release) describe single notes: render one note to measure them.";
        c.tags = {"analysis"};
        c.args = schema::object<AnalyzeArgs>("analyze_args")
                     .field("source", &AnalyzeArgs::source, sourceField())
                     .field("detail", &AnalyzeArgs::detail, analysis_commands::detailSchema())
                     .defaultValue(DetailArg::Summary)
                     .field("note_off_s", &AnalyzeArgs::noteOffS, schema::number().range(0, 3600))
                     .build();
        c.result = schema::object<AnalyzeResult>("analysis")
                       .field("report", &AnalyzeResult::report, schema::any())
                       .field("summary", &AnalyzeResult::summary, schema::string())
                       .build();
        c.errors = {{"sandbox.unknown_render", "no such render", "session.get lists the renders"},
                    {"audio.*", "the file cannot be read", "WAV, FLAC or MP3"}};
        c.examples = {{"Analyze the last render", obj(R"({"source": "rd_1"})"),
                       obj(R"({"report": "<any>", "summary": "<any>"})"), "synth_and_render"}};
        c.run = [](const AnalyzeArgs& a, Context& ctx) -> Result<AnalyzeResult> {
            auto l = load(S(ctx), a.source, "/source");
            if (!l) return std::move(l).error();
            analysis::Options o;
            o.noteOffSeconds = a.noteOffS ? a.noteOffS : l->noteOff;
            auto r = analysis::analyze(l->signal(), o);
            if (!r) return std::move(r).error();
            return AnalyzeResult{analysis::toJson(*r, analysis_commands::toDetail(a.detail)),
                                 analysis::summarize(*r)};
        };
        registry.add(std::move(c));
    }

    // compare
    {
        Command<CompareArgs, CompareScoredResult> c;
        c.name = "compare";
        c.kind = Kind::Query;
        c.exposure = Exposure::McpTool;
        c.summary = "Compare two renders or files: notable descriptor changes and timbre distance.";
        c.description =
            "Notable changes use fixed thresholds (loudness 1 LU, centroid 10 %, band levels 1.5 "
            "dB, "
            "harmonics 3 dB, f0 10 cents, ...). ltas_rms_db is the RMS difference of the "
            "1/3-octave "
            "spectra after removing the level offset: a pure gain change reads 0. loss is the "
            "score the optimizer minimizes and the evaluation scorer judges: the test is "
            "loudness-matched to the reference, both are analyzed at their note-off times "
            "(note_off_s reports them), and loss.total is the number to get down. A render's "
            "note-off is its own input end; give reference_note_off_s / test_note_off_s for "
            "files, and one unknown side borrows the other's (note_off_assumed).";
        c.tags = {"analysis"};
        c.args = schema::object<CompareArgs>("compare_args")
                     .field("reference", &CompareArgs::reference, sourceField())
                     .field("test", &CompareArgs::test, sourceField())
                     .field("reference_note_off_s", &CompareArgs::referenceNoteOffS,
                            schema::number().range(0, 3600).doc(
                                "The reference's note-off; defaults to a render's input end."))
                     .field("test_note_off_s", &CompareArgs::testNoteOffS,
                            schema::number().range(0, 3600).doc(
                                "The test's note-off; defaults to a render's input end."))
                     .build();
        c.result = compareScoredSchema();
        c.examples = {
            {"A render against itself", obj(R"({"reference": "rd_1", "test": "rd_1"})"),
             obj(R"({"notable": [], "ltas_rms_db": 0.0, "summary": "<any>", "loss": {"total": "<any>",
                    "terms": "<any>", "note_off_s": {"reference": "<any>", "test": "<any>"}}})"),
             "synth_and_render"}};
        c.run = [](const CompareArgs& a, Context& ctx) -> Result<CompareScoredResult> {
            Session& s = S(ctx);
            auto ref = load(s, a.reference, "/reference");
            if (!ref) return std::move(ref).error();
            auto test = load(s, a.test, "/test");
            if (!test) return std::move(test).error();
            // One unknown side borrows the other's note-off: the usual case is
            // two renders of the same phrase, where either end fits.
            auto refNoteOff = noteOffFor(s, a.reference, a.referenceNoteOffS);
            auto testNoteOff = noteOffFor(s, a.test, a.testNoteOffS);
            bool assumed = false;
            if (!refNoteOff && testNoteOff) {
                refNoteOff = testNoteOff;
                assumed = true;
            } else if (refNoteOff && !testNoteOff) {
                testNoteOff = refNoteOff;
                assumed = true;
            }
            analysis::Options o;
            o.noteOffSeconds = refNoteOff;
            auto ra = analysis::analyze(ref->signal(), o);
            if (!ra) return std::move(ra).error();
            o.noteOffSeconds = testNoteOff;
            auto rb = analysis::analyze(test->signal(), o);
            if (!rb) return std::move(rb).error();
            CompareResult base = analysis_commands::compare(*ra, *rb);
            auto scored = detail::referenceScore(ref->data, refNoteOff, test->data, testNoteOff);
            if (!scored) return std::move(scored).error();
            return CompareScoredResult{std::move(base.notable), base.ltasRmsDb,
                                       std::move(base.summary),
                                       lossJson(*scored, refNoteOff, testNoteOff, assumed)};
        };
        registry.add(std::move(c));
    }

    // masking
    {
        Command<MaskingArgs, JsonResult> c;
        c.name = "masking";
        c.kind = Kind::Query;
        c.exposure = Exposure::McpTool;
        c.summary = "Estimate where one sound masks another (critical bands).";
        c.description =
            "masker (e.g. the kick) and maskee (e.g. the bass): renders or files at the same "
            "sample "
            "rate. Per Bark band: % of the maskee's active frames masked and by how much; the "
            "three "
            "worst bands. An estimate for corrective mixing, not a psychoacoustic model of record.";
        c.tags = {"analysis"};
        c.args = schema::object<MaskingArgs>("masking_args")
                     .field("masker", &MaskingArgs::masker, sourceField())
                     .field("maskee", &MaskingArgs::maskee, sourceField())
                     .build();
        c.result = schema::object<JsonResult>("masking")
                       .field("result", &JsonResult::result, schema::any())
                       .build();
        c.examples = {{"A render masking itself", obj(R"({"masker": "rd_1", "maskee": "rd_1"})"),
                       obj(R"({"result": "<any>"})"), "synth_and_render"}};
        c.run = [](const MaskingArgs& a, Context& ctx) -> Result<JsonResult> {
            auto m = load(S(ctx), a.masker, "/masker");
            if (!m) return std::move(m).error();
            auto e = load(S(ctx), a.maskee, "/maskee");
            if (!e) return std::move(e).error();
            auto r = analysis::masking(m->signal(), e->signal());
            if (!r) return std::move(r).error();
            return JsonResult{analysis::toJson(*r)};
        };
        registry.add(std::move(c));
    }

    // profile.evaluate
    {
        Command<ProfileArgs, JsonResult> c;
        c.name = "profile.evaluate";
        c.kind = Kind::Query;
        c.exposure = Exposure::McpTool;
        c.summary = "Score a render against a target descriptor profile.";
        c.description =
            "profile: {\"schema\": \"supple.analysis-profile/1.0\", \"targets\": {\"<report "
            "key>\": "
            "{\"target\": x, \"tolerance\": t, \"weight\"?: w} | {\"min\"?: a, \"max\"?: b}}, "
            "\"ltas\"?: [31 relative dB]}. Keys are dotted report paths such as "
            "spectral.centroid_hz.median or harmonic.harmonics_db.3. Returns per-target pass and "
            "normalized distance, and a weighted loss (0 = on target).";
        c.tags = {"analysis"};
        c.args = schema::object<ProfileArgs>("profile_args")
                     .field("source", &ProfileArgs::source, sourceField())
                     .field("profile", &ProfileArgs::profile, schema::any())
                     .build();
        c.result = schema::object<JsonResult>("profile_result")
                       .field("result", &JsonResult::result, schema::any())
                       .build();
        c.examples = {
            {"A brightness target",
             obj(R"({"source": "rd_1", "profile": {"schema": "supple.analysis-profile/1.0",
                            "targets": {"spectral.centroid_hz.median": {"target": 1500, "tolerance": 500}}}})"),
             obj(R"({"result": "<any>"})"), "synth_and_render"}};
        c.run = [](const ProfileArgs& a, Context& ctx) -> Result<JsonResult> {
            auto l = load(S(ctx), a.source, "/source");
            if (!l) return std::move(l).error();
            analysis::Options o;
            o.noteOffSeconds = l->noteOff;
            auto r = analysis::analyze(l->signal(), o);
            if (!r) return std::move(r).error();
            auto e = analysis::evaluate(*r, a.profile);
            if (!e) {
                Error err = std::move(e).error();
                err.path = "/profile" + err.path;
                return err;
            }
            return JsonResult{analysis::toJson(*e)};
        };
        registry.add(std::move(c));
    }
}

}  // namespace supple::sandbox
