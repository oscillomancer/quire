// Sandbox analysis commands (sandbox.md §4): analyze, compare, masking,
// profile.evaluate — over renders (rd_<n>) or audio files.
#include "analysis_commands.h"
#include "../../analysis-commands/analysis_shared.h"

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
    if (ref.rfind("rd_", 0) == 0) {
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
};
using analysis_commands::CompareResult;
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
        Command<CompareArgs, CompareResult> c;
        c.name = "compare";
        c.kind = Kind::Query;
        c.exposure = Exposure::McpTool;
        c.summary = "Compare two renders or files: notable descriptor changes and timbre distance.";
        c.description =
            "Notable changes use fixed thresholds (loudness 1 LU, centroid 10 %, band levels 1.5 "
            "dB, "
            "harmonics 3 dB, f0 10 cents, ...). ltas_rms_db is the RMS difference of the "
            "1/3-octave "
            "spectra after removing the level offset: a pure gain change reads 0.";
        c.tags = {"analysis"};
        c.args = schema::object<CompareArgs>("compare_args")
                     .field("reference", &CompareArgs::reference, sourceField())
                     .field("test", &CompareArgs::test, sourceField())
                     .build();
        c.result = analysis_commands::compareSchema();
        c.examples = {{"A render against itself", obj(R"({"reference": "rd_1", "test": "rd_1"})"),
                       obj(R"({"notable": [], "ltas_rms_db": 0.0, "summary": "<any>"})"),
                       "synth_and_render"}};
        c.run = [](const CompareArgs& a, Context& ctx) -> Result<CompareResult> {
            auto ref = load(S(ctx), a.reference, "/reference");
            if (!ref) return std::move(ref).error();
            auto test = load(S(ctx), a.test, "/test");
            if (!test) return std::move(test).error();
            analysis::Options o;
            o.noteOffSeconds = ref->noteOff;
            auto ra = analysis::analyze(ref->signal(), o);
            if (!ra) return std::move(ra).error();
            o.noteOffSeconds = test->noteOff;
            auto rb = analysis::analyze(test->signal(), o);
            if (!rb) return std::move(rb).error();
            return analysis_commands::compare(*ra, *rb);
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
