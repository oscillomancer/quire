// dsp.export (ADR-0032 §5), shared by the engine and the sandbox: a script
// as a plugin for any CLAP host. It copies the script player that plays the
// script here and appends the script to the copy (supple/script_export.hpp);
// the copy offers that one plugin, under the script's id, and reads no
// library. Beside it go the licenses of the player and what it includes.
//
// Host is the Context's service and has scriptDir(), findPlugin(id) and
// plugins(rescan).
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dsp_shared.h"
#include "supple/base/files.h"
#include "supple/cmd/bus.h"
#include "supple/script_export.hpp"

namespace supple::dsp_commands {
namespace exporting {
namespace fs = std::filesystem;

struct ExportArgs {
    std::string script;
    std::optional<std::string> folder;
};
struct ExportResult {
    std::string path, id, name, format, player;
    std::int64_t bytes = 0;
    std::optional<std::string> notices;
};

// A plugin's name as a file name: what file systems refuse becomes '-'.
inline std::string fileName(std::string_view name, std::string_view fallback) {
    std::string out;
    for (const char c : name) {
        const bool bad = static_cast<unsigned char>(c) < 0x20 || c == '/' || c == '\\' ||
                         c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                         c == '|';
        out += bad ? '-' : c;
    }
    while (!out.empty() && (out.front() == '.' || out.front() == ' ')) out.erase(out.begin());
    while (!out.empty() && (out.back() == '.' || out.back() == ' ')) out.pop_back();
    return out.empty() ? std::string(fallback) : out;
}

// The notices to ship with an export of `module`: the player's
// <stem>-LICENSE.txt and <stem>-NOTICES.txt as one text, or, when `module` is
// itself an export, the <stem>-NOTICES.txt that already is that text.
inline std::optional<std::string> playerNotices(const fs::path& module, bool fromExport) {
    const fs::path dir = module.parent_path();
    const std::string stem = module.stem().string();
    auto notices = readTextFile(dir / (stem + "-NOTICES.txt"), std::size_t(4) << 20);
    if (!notices) return std::nullopt;
    if (fromExport) return std::move(*notices);
    auto license = readTextFile(dir / (stem + "-LICENSE.txt"), std::size_t(1) << 20);
    if (!license) return std::nullopt;
    return "This plugin is a copy of the script player with a DSP script appended. The script "
           "is its author's. The player's license and the notices of the software it includes "
           "follow.\n\n"
           "==============================================================================\n" +
           *license + "\n" + *notices;
}

}  // namespace exporting

template <class Host>
void registerDspExport(cmd::Registry& registry) {
    using namespace exporting;
    using cmd::Command;
    using cmd::Context;
    using cmd::Kind;
    Command<ExportArgs, ExportResult> c;
    c.name = "dsp.export";
    c.kind = Kind::Action;
    c.exposure = cmd::Exposure::McpTool;
    c.summary = "Export a DSP script as a CLAP plugin file for any DAW.";
    c.description =
        "Copies the script player that plays the script here and appends the script: the "
        "copy is a plugin of its own, under the script's id and name, that plays this "
        "version of the script in any CLAP host (it reads no library). Written to "
        "<folder>/<plugin name>.clap, by default in the script's folder under export/, with "
        "the licenses to ship beside it (notices). It runs on this platform, as fast as the "
        "player it copies (a debug build's is slow). Export again after changing the script; "
        "an installed copy does not follow the library.";
    c.tags = {"dsp"};
    c.args = schema::object<ExportArgs>("dsp_export_args")
                 .field("script", &ExportArgs::script,
                        schema::string().minLength(1).doc("a name from dsp.list"))
                 .field("folder", &ExportArgs::folder,
                        schema::string().minLength(1).doc(
                            "an absolute folder to write to (~/.clap installs it for this user); "
                            "default <library>/<script>/export"))
                 .build();
    c.result = schema::object<ExportResult>("dsp_export_result")
                   .field("path", &ExportResult::path, schema::string().doc("the plugin file"))
                   .field("id", &ExportResult::id, schema::string())
                   .field("name", &ExportResult::name, schema::string())
                   .field("format", &ExportResult::format, schema::string().doc("clap"))
                   .field("player", &ExportResult::player,
                          schema::string().doc("the script player it was copied from"))
                   .field("bytes", &ExportResult::bytes, schema::integer().min(0))
                   .field("notices", &ExportResult::notices,
                          schema::string().doc("the licenses to ship with the plugin"))
                   .build();
    c.errors = {{"dsp.compile", "the script does not compile", "dsp.check gives its diagnostics"},
                {"dsp.not_listed", "the script compiles but no plugin has its id",
                 "put the script player on the plugin path"},
                {"dsp.player", "the plugin with the script's id is not the script player",
                 "give the script an id no other plugin has"},
                {"dsp.path", "the folder is relative", "give an absolute folder"}};
    {
        json::Value args = json::Value::object();
        args["script"] = "gain";
        c.examples = {{"Export a script from the library", std::move(args),
                       detail::j(R"({"path":"<any>","id":"com.example.gain","name":"Gain",
                                     "format":"clap","player":"<any>","bytes":"<any>",
                                     "notices":"<any>"})"),
                       "dsp_player"}};
    }
    c.run = [](const ExportArgs& a, Context& ctx) -> Result<ExportResult> {
        Host& s = ctx.service<Host>();
        if (a.folder && !fs::path(*a.folder).is_absolute())
            return Error("dsp.path", "'" + *a.folder + "' is not an absolute folder", "/folder",
                         "give the whole path, such as /home/<you>/.clap");
        auto text = detail::readScript(s, a.script, "/script");
        if (!text) return std::move(text).error();
        const detail::Compiled compiled = detail::compile(*text);
        if (!compiled.plugin)
            return Error("dsp.compile", "the script does not compile", "/script",
                         "dsp.check {name: \"" + a.script + "\"} gives its diagnostics");
        const std::string& id = compiled.plugin->id;
        auto info = s.findPlugin(id);
        if (!info) {
            (void)s.plugins(true);  // a script written since the last scan
            info = s.findPlugin(id);
        }
        if (!info)
            return Error("dsp.not_listed", "no plugin has the id " + id, "/script",
                         "the script player must be on the plugin path");
        if (info->format != "clap" || info->path.extension() != ".clap")
            return Error("dsp.player",
                         id + " is played by " + info->path.string() + ", not a .clap", "/script",
                         "put the script player's .clap on the plugin path");
        auto module = readTextFile(info->path);
        if (!module) return std::move(module).error();
        // An export of an export starts again from the player. The player is
        // the module that reads exports, so it carries their schema's name.
        const std::string_view player = scriptc::exportPlayer(*module);
        if (player.find(scriptc::kExportSchema) == std::string_view::npos)
            return Error(
                "dsp.player",
                id + " belongs to " + info->path.string() + ", which is not the script player",
                "/script", "give the script an id no other plugin has");

        json::Value payload = json::Value::object();
        payload["schema"] = std::string(scriptc::kExportSchema);
        payload["name"] = a.script;
        payload["source"] = *text;
        std::string bytes(player);
        bytes += scriptc::exportTrailer(json::writeCompact(payload));

        const fs::path folder =
            a.folder ? fs::path(*a.folder) : s.scriptDir() / a.script / "export";
        const fs::path file = folder / (fileName(compiled.plugin->name, a.script) + ".clap");
        SUPPLE_TRY(writeFileAtomic(file, bytes));
        std::error_code ec;
        fs::permissions(file,
                        fs::perms::owner_exec | fs::perms::group_exec | fs::perms::others_exec,
                        fs::perm_options::add, ec);
        ExportResult out{file.string(),
                         id,
                         compiled.plugin->name,
                         "clap",
                         info->path.string(),
                         static_cast<std::int64_t>(bytes.size()),
                         {}};
        if (auto notices = playerNotices(info->path, player.size() != module->size())) {
            const fs::path notes = folder / (file.stem().string() + "-NOTICES.txt");
            SUPPLE_TRY(writeFileAtomic(notes, *notices));
            out.notices = notes.string();
        }
        return out;
    };
    registry.add(std::move(c));
}

}  // namespace supple::dsp_commands
