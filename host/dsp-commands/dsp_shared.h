// DSP scripts (ADR-0032), shared by the engine and the sandbox: the commands
// that write, check, read and list the scripts the script player turns into
// plugins. A script is <library>/<name>/<name>.cpp (Host::scriptDir()); once
// it compiles, plugin.list shows it under its own id and it is loaded, set
// and saved like any other plugin. Live instances follow edits by themselves.
//
// Host needs scriptDir() and plugins(bool rescan), and is the Context's
// service: the engine's app in the engine, the session in the sandbox.
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "script_library.h"
#include "supple/base/files.h"
#include "supple/cmd/bus.h"
#include "supple/script_compiler.hpp"
#include "supple/script_program.hpp"

namespace supple::dsp_commands {
namespace detail {
namespace fs = std::filesystem;

// The longest script: the player reads no more.
inline constexpr std::size_t kMaxScriptBytes = std::size_t(1) << 20;

inline json::Value j(std::string_view text) { return json::parse(text).value(); }

struct Diagnostic {
    std::string severity, code, message, hint;
    std::int64_t line = 0, column = 0;
};
struct ParamEntry {
    std::string key, name, kind;
    std::optional<std::string> unit;
    double min = 0.0, max = 0.0, def = 0.0;
    std::vector<std::string> choices;
    bool meter = false;
};
struct FileEntry {
    std::string key, name, kind;
    std::optional<std::string> description;
};
struct PluginSummary {
    std::string id, name, kind;
    std::vector<ParamEntry> params;
    std::optional<std::vector<FileEntry>> files;
};
struct Compiled {
    bool ok = false;
    std::vector<Diagnostic> diagnostics;
    std::optional<PluginSummary> plugin;
};

struct WriteArgs {
    std::string name, source;
};
struct WriteResult {
    std::string name, path;
    bool ok = false;
    std::vector<Diagnostic> diagnostics;
    std::optional<PluginSummary> plugin;
    bool listed = false;
};
struct CheckArgs {
    std::optional<std::string> name, source;
};
struct NameArgs {
    std::string name;
};
struct GetResult {
    std::string name, path, source;
};
struct ListArgs {};
struct ListEntry {
    std::string name, path;
    bool ok = false;
    std::int64_t errors = 0;
    std::optional<std::string> id, pluginName;
};
struct ListResult {
    std::string library;
    std::vector<ListEntry> scripts;
};

inline const char* unitName(scriptc::ParamModel::Unit u) {
    using U = scriptc::ParamModel::Unit;
    switch (u) {
        case U::Decibels:
            return "dB";
        case U::Hertz:
            return "Hz";
        case U::Milliseconds:
            return "ms";
        case U::Percent:
            return "%";
        case U::Ratio:
            return "ratio";
        case U::Semitones:
            return "st";
        case U::Cents:
            return "ct";
        case U::Octaves:
            return "oct";
        case U::Degrees:
            return "deg";
        case U::Bpm:
            return "bpm";
        case U::Samples:
            return "samples";
        default:
            return nullptr;
    }
}

inline const char* kindName(scriptc::ParamModel::Kind k) {
    using K = scriptc::ParamModel::Kind;
    switch (k) {
        case K::Integer:
            return "integer";
        case K::Choice:
            return "choice";
        case K::Toggle:
            return "toggle";
        default:
            return "continuous";
    }
}

inline Compiled compile(const std::string& source) {
    const scriptc::Result r = scriptc::compile(source);
    Compiled out;
    out.ok = r.program != nullptr;
    for (const scriptc::Message& m : r.diagnostics)
        out.diagnostics.push_back(
            {m.error ? "error" : "warning", m.code, m.message, m.hint, m.line, m.column});
    if (r.program) {
        PluginSummary p{
            r.program->info().id,
            r.program->info().name,
            r.program->kind() == scriptc::PluginKind::Instrument ? "instrument" : "effect",
            {},
            {}};
        for (const scriptc::ParamModel& m : r.program->params()) {
            ParamEntry e{m.key, m.name, kindName(m.kind), {},      m.min,
                         m.max, m.def,  m.choices,        m.output};
            if (const char* u = unitName(m.unit)) e.unit = u;
            p.params.push_back(std::move(e));
        }
        for (const scriptc::FileModel& m : r.program->files()) {
            FileEntry e{m.key,
                        m.name,
                        m.kind == scriptc::FileModel::Kind::Impulse ? "impulse" : "audio",
                        {}};
            if (!m.description.empty()) e.description = m.description;
            if (!p.files) p.files.emplace();
            p.files->push_back(std::move(e));
        }
        out.plugin = std::move(p);
    }
    return out;
}

inline Status validName(const std::string& name, const std::string& path) {
    const bool ok =
        !name.empty() && name.size() <= 64 && name[0] >= 'a' && name[0] <= 'z' &&
        std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
    if (!ok)
        return Error("dsp.name", "'" + name + "' is not a script name", path,
                     "use 1-64 lower-case letters, digits, '_' or '-', starting with a letter: "
                     "\"tilt_eq\"");
    return {};
}

template <class Host>
fs::path scriptPath(const Host& a, const std::string& name) {
    return a.scriptDir() / name / (name + ".cpp");
}

template <class Host>
Result<std::string> readScript(const Host& a, const std::string& name, const std::string& path) {
    SUPPLE_TRY(validName(name, path));
    const fs::path file = scriptPath(a, name);
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) {
        std::vector<std::string> names;
        for (fs::directory_iterator it(a.scriptDir(), ec), end; !ec && it != end; it.increment(ec))
            if (fs::is_regular_file(it->path() / (it->path().filename().string() + ".cpp"), ec))
                names.push_back(it->path().filename().string());
        std::sort(names.begin(), names.end());
        std::string known;
        for (const auto& n : names) known += (known.empty() ? "" : ", ") + n;
        return Error("dsp.unknown", "no script '" + name + "' in " + a.scriptDir().string(), path,
                     known.empty() ? "write one with dsp.write" : "the library has: " + known);
    }
    return readTextFile(file, kMaxScriptBytes);
}

inline schema::Schema<Diagnostic> diagnosticSchema() {
    return schema::object<Diagnostic>("dsp_diagnostic")
        .field("severity", &Diagnostic::severity, schema::string().doc("error or warning"))
        .field("code", &Diagnostic::code, schema::string().doc("script.<area>.<what>"))
        .field("message", &Diagnostic::message, schema::string())
        .field("hint", &Diagnostic::hint, schema::string().doc("what to write instead"))
        .field("line", &Diagnostic::line, schema::integer())
        .field("column", &Diagnostic::column, schema::integer())
        .build();
}

inline schema::Schema<PluginSummary> pluginSchema() {
    auto param = schema::object<ParamEntry>("dsp_param")
                     .field("key", &ParamEntry::key, schema::string().doc("the patch key"))
                     .field("name", &ParamEntry::name, schema::string())
                     .field("kind", &ParamEntry::kind,
                            schema::string().doc("continuous, integer, choice or toggle"))
                     .field("unit", &ParamEntry::unit, schema::string().doc("dB, Hz, ms, % ..."))
                     .field("min", &ParamEntry::min, schema::number())
                     .field("max", &ParamEntry::max, schema::number())
                     .field("default", &ParamEntry::def, schema::number())
                     .field("choices", &ParamEntry::choices, schema::arrayOf(schema::string()))
                     .field("meter", &ParamEntry::meter,
                            schema::boolean().doc("an output the plugin reports"))
                     .build();
    auto file =
        schema::object<FileEntry>("dsp_file")
            .field("key", &FileEntry::key, schema::string().doc("the patch's properties key"))
            .field("name", &FileEntry::name, schema::string())
            .field("kind", &FileEntry::kind,
                   schema::string().doc("audio (a sample) or impulse (a Convolver's response)"))
            .field("description", &FileEntry::description, schema::string())
            .build();
    return schema::object<PluginSummary>("dsp_plugin")
        .field("id", &PluginSummary::id, schema::string())
        .field("name", &PluginSummary::name, schema::string())
        .field("kind", &PluginSummary::kind,
               schema::string().doc("effect (audio in, audio out) or instrument (notes in)"))
        .field("params", &PluginSummary::params, schema::arrayOf(param))
        .field("files", &PluginSummary::files,
               schema::arrayOf(file).doc(
                   "the script's files (AudioFile, Convolver), when it has any: give each an "
                   "absolute path in a patch, {\"properties\": {\"<key>\": \"/path.wav\"}}"))
        .build();
}

inline const char* const kGain = R"(#include <supple/script.h>

struct Gain : Effect {
    static constexpr Info info = {.name = "Gain", .id = "com.example.gain"};
    Param gain = decibels("Gain", -24, 24, 0).smoothed(20).help("The level change.");

    void process(Block& io) {
        for (int c = 0; c < io.channels; ++c)
            for (int i = 0; i < io.frames; ++i) io[c][i] *= dbToGain(gain.at(i));
    }
};
)";

}  // namespace detail

template <class Host>
void registerDspCommands(cmd::Registry& registry) {
    using namespace detail;
    using cmd::Command;
    using cmd::Context;
    using cmd::Kind;
    const auto app = [](Context& ctx) -> Host& { return ctx.service<Host>(); };
    const auto gainArgs = [] {
        json::Value a = json::Value::object();
        a["name"] = "gain";
        a["source"] = kGain;
        return a;
    };
    {
        Command<WriteArgs, WriteResult> c;
        c.name = "dsp.write";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save a DSP script to the library and compile it, with diagnostics.";
        c.description =
            "Writes <library>/<name>/<name>.cpp (replacing it), compiles it and returns the "
            "compiler's diagnostics, each with a hint. A script that compiles becomes a plugin "
            "of the script player under the id its Info gives: plugin.list shows it (listed) "
            "and fx.add loads it. Instances already playing it pick up the change within half "
            "a second, crossfaded; one that fails to compile leaves them playing the old code. "
            "Language: the dsp-language guide.";
        c.tags = {"dsp"};
        c.args = schema::object<WriteArgs>("dsp_write_args")
                     .field("name", &WriteArgs::name,
                            schema::string().minLength(1).doc("the script's folder and file name"))
                     .field("source", &WriteArgs::source,
                            schema::string().minLength(1).doc("the whole C++ file"))
                     .build();
        c.result = schema::object<WriteResult>("dsp_write_result")
                       .field("name", &WriteResult::name, schema::string())
                       .field("path", &WriteResult::path, schema::string())
                       .field("ok", &WriteResult::ok, schema::boolean().doc("it compiled"))
                       .field("diagnostics", &WriteResult::diagnostics,
                              schema::arrayOf(diagnosticSchema()))
                       .field("plugin", &WriteResult::plugin, pluginSchema())
                       .field("listed", &WriteResult::listed,
                              schema::boolean().doc("plugin.list shows it (the script player is on "
                                                    "the plugin path)"))
                       .build();
        c.errors = {{"dsp.name", "not a script name", "use lower-case letters, digits, _ or -"},
                    {"dsp.size", "the script is longer than 1 MiB", "split it up"}};
        c.examples = {{"Save a gain script", gainArgs(),
                       j(R"({"name":"gain","path":"<any>","ok":true,"diagnostics":[],
                             "plugin":"<any>","listed":"<any>"})"),
                       "empty"}};
        c.run = [app](const WriteArgs& a, Context& ctx) -> Result<WriteResult> {
            Host& owner = app(ctx);
            SUPPLE_TRY(validName(a.name, "/name"));
            if (a.source.size() > kMaxScriptBytes)
                return Error(
                    "dsp.size",
                    "the script is " + std::to_string(a.source.size()) + " bytes, more than 1 MiB",
                    "/source", "split it up");
            const fs::path file = scriptPath(owner, a.name);
            SUPPLE_TRY(writeFileAtomic(file, a.source));
            Compiled compiled = compile(a.source);
            WriteResult out{a.name,
                            file.string(),
                            compiled.ok,
                            std::move(compiled.diagnostics),
                            std::move(compiled.plugin),
                            false};
            if (out.plugin) {
                // The player lists the library afresh at each scan.
                auto found = owner.plugins(true);
                if (found)
                    out.listed = std::any_of(found->begin(), found->end(),
                                             [&](const auto& p) { return p.id == out.plugin->id; });
            }
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<CheckArgs, Compiled> c;
        c.name = "dsp.check";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Compile a DSP script without saving it; return its diagnostics.";
        c.description =
            "Give source (a whole script) or name (one in the library). ok is true when it "
            "compiles; plugin then lists its id, name and parameters as the patch shows them.";
        c.tags = {"dsp"};
        c.args = schema::object<CheckArgs>("dsp_check_args")
                     .field("name", &CheckArgs::name, schema::string().minLength(1))
                     .field("source", &CheckArgs::source, schema::string().minLength(1))
                     .build();
        c.result =
            schema::object<Compiled>("dsp_check_result")
                .field("ok", &Compiled::ok, schema::boolean())
                .field("diagnostics", &Compiled::diagnostics, schema::arrayOf(diagnosticSchema()))
                .field("plugin", &Compiled::plugin, pluginSchema())
                .build();
        c.errors = {{"dsp.args", "neither or both of name and source", "give exactly one"},
                    {"dsp.unknown", "no such script", "dsp.list names the library's scripts"}};
        c.examples = {{"Check a script in the library", j(R"({"name":"gain"})"),
                       j(R"({"ok":true,"diagnostics":[],"plugin":"<any>"})"), "dsp_script"},
                      {"A mistake, with its hint", j(R"({"source":"struct X : Effect {};"})"),
                       j(R"({"ok":false,"diagnostics":"<any>"})"), "empty"}};
        c.run = [app](const CheckArgs& a, Context& ctx) -> Result<Compiled> {
            if (a.name.has_value() == a.source.has_value())
                return Error("dsp.args", "give exactly one of name and source", "",
                             "source checks text; name checks a script in the library");
            if (a.source) return compile(*a.source);
            auto text = readScript(app(ctx), *a.name, "/name");
            if (!text) return std::move(text).error();
            return compile(*text);
        };
        registry.add(std::move(c));
    }
    {
        Command<NameArgs, GetResult> c;
        c.name = "dsp.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read a DSP script from the library.";
        c.tags = {"dsp"};
        c.args = schema::object<NameArgs>("dsp_get_args")
                     .field("name", &NameArgs::name, schema::string().minLength(1))
                     .build();
        c.result = schema::object<GetResult>("dsp_get_result")
                       .field("name", &GetResult::name, schema::string())
                       .field("path", &GetResult::path, schema::string())
                       .field("source", &GetResult::source, schema::string())
                       .build();
        c.errors = {{"dsp.unknown", "no such script", "dsp.list names the library's scripts"}};
        c.examples = {{"Read a script", j(R"({"name":"gain"})"),
                       j(R"({"name":"gain","path":"<any>","source":"<any>"})"), "dsp_script"}};
        c.run = [app](const NameArgs& a, Context& ctx) -> Result<GetResult> {
            auto text = readScript(app(ctx), a.name, "/name");
            if (!text) return std::move(text).error();
            return GetResult{a.name, scriptPath(app(ctx), a.name).string(), std::move(*text)};
        };
        registry.add(std::move(c));
    }
    {
        Command<ListArgs, ListResult> c;
        c.name = "dsp.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List the DSP scripts in the library, compiled, with their plugin ids.";
        c.tags = {"dsp"};
        c.args = schema::object<ListArgs>("dsp_list_args").build();
        auto entry = schema::object<ListEntry>("dsp_list_entry")
                         .field("name", &ListEntry::name, schema::string())
                         .field("path", &ListEntry::path, schema::string())
                         .field("ok", &ListEntry::ok, schema::boolean().doc("it compiles"))
                         .field("errors", &ListEntry::errors, schema::integer().min(0))
                         .field("id", &ListEntry::id, schema::string().doc("its plugin's id"))
                         .field("plugin_name", &ListEntry::pluginName, schema::string())
                         .build();
        c.result = schema::object<ListResult>("dsp_list_result")
                       .field("library", &ListResult::library, schema::string())
                       .field("scripts", &ListResult::scripts, schema::arrayOf(entry))
                       .build();
        c.examples = {
            {"An empty library", j("{}"), j(R"({"library":"<any>","scripts":[]})"), "empty"}};
        c.run = [app](const ListArgs&, Context& ctx) -> Result<ListResult> {
            const Host& owner = app(ctx);
            ListResult out{owner.scriptDir().string(), {}};
            std::error_code ec;
            std::vector<fs::path> files;
            for (fs::directory_iterator it(owner.scriptDir(), ec), end; !ec && it != end;
                 it.increment(ec)) {
                const fs::path file = it->path() / (it->path().filename().string() + ".cpp");
                if (fs::is_regular_file(file, ec)) files.push_back(file);
            }
            std::sort(files.begin(), files.end());
            for (const fs::path& file : files) {
                ListEntry e{
                    file.parent_path().filename().string(), file.string(), false, 0, {}, {}};
                if (auto text = readTextFile(file, kMaxScriptBytes)) {
                    const Compiled compiled = compile(*text);
                    e.ok = compiled.ok;
                    e.errors =
                        std::count_if(compiled.diagnostics.begin(), compiled.diagnostics.end(),
                                      [](const Diagnostic& d) { return d.severity == "error"; });
                    if (compiled.plugin) {
                        e.id = compiled.plugin->id;
                        e.pluginName = compiled.plugin->name;
                    }
                } else {
                    e.errors = 1;
                }
                out.scripts.push_back(std::move(e));
            }
            return out;
        };
        registry.add(std::move(c));
    }
}

}  // namespace supple::dsp_commands
