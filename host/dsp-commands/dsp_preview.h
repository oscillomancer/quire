// dsp.preview, a plugin's editor as a picture (ADR-0032 §8), shared by the
// engine and the sandbox: a fresh instance, set by a patch, plays a test
// signal (or notes) while its editor is watched, so meters and the
// script's own displays show what played; then the editor is drawn without
// a window (com.suppleaudio.editor-picture/1) and saved as a PNG for an agent
// to look at. Renders as dsp.test does (TestTraits<Host>).
#pragma once

#include <cmath>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "dsp_test.h"
#include "png.h"

namespace supple::dsp_commands {

namespace preview {

struct PreviewArgs {
    std::optional<std::string> plugin, script;
    std::optional<json::Value> patch;
    std::optional<test::Signal> signal;
    double levelDb = -12.0;
    double frequencyHz = 1000.0;
    double seconds = 1.0;
    double scale = 1.0;
};
struct PreviewResult {
    std::string plugin;
    std::string path;
    std::int64_t width = 0, height = 0;
    std::vector<std::string> problems;
    std::vector<std::string> log;
};

}  // namespace preview

template <class Host>
void registerDspPreview(cmd::Registry& registry) {
    using namespace test;
    using namespace preview;
    using cmd::Command;
    using cmd::Context;
    using cmd::Kind;
    Command<PreviewArgs, PreviewResult> c;
    c.name = "dsp.preview";
    c.kind = Kind::Action;
    c.exposure = cmd::Exposure::McpTool;
    c.summary = "Draw a plugin's editor to a PNG, after a test signal plays through it.";
    c.description =
        "Give plugin (an id) or script (a name in the library). A fresh instance, set by patch (a "
        "merge patch) when given, plays signal -- an effect: sine (the default, at frequency_hz), "
        "sweep, impulse, noise or silence, peaking at level_db; an instrument: notes (the "
        "default), note or silence -- for seconds while its editor watches, so meters and the "
        "script's draw() show what played. Then the editor is drawn without a window, scale "
        "times its design size, and saved as a PNG (path; width and height in pixels): read the "
        "file to see it. A script with neither editor() nor draw() shows the automatic editor. "
        "problems says what went wrong in the editor's code (a loop stopped by its budget, an "
        "index out of range), with the line.";
    c.tags = {"dsp", "editor"};
    c.args = schema::object<PreviewArgs>("dsp_preview_args")
                 .field("plugin", &PreviewArgs::plugin, schema::string().minLength(1))
                 .field("script", &PreviewArgs::script, schema::string().minLength(1))
                 .field("patch", &PreviewArgs::patch, schema::any())
                 .field("signal", &PreviewArgs::signal,
                        schema::enumeration<Signal>({{Signal::Sine, "sine"},
                                                     {Signal::Sweep, "sweep"},
                                                     {Signal::Impulse, "impulse"},
                                                     {Signal::Noise, "noise"},
                                                     {Signal::Silence, "silence"},
                                                     {Signal::Notes, "notes"},
                                                     {Signal::Note, "note"}}))
                 .field("level_db", &PreviewArgs::levelDb,
                        schema::number().range(-60, 0).unit(units::Unit::dB))
                 .defaultValue(-12.0)
                 .field("frequency_hz", &PreviewArgs::frequencyHz,
                        schema::number().range(10, 20000).unit(units::Unit::Hz))
                 .defaultValue(1000.0)
                 .field("seconds", &PreviewArgs::seconds, schema::number().range(0, 10))
                 .defaultValue(1.0)
                 .field("scale", &PreviewArgs::scale, schema::number().range(0.5, 3))
                 .defaultValue(1.0)
                 .build();
    c.result = schema::object<PreviewResult>("dsp_preview_result")
                   .field("plugin", &PreviewResult::plugin, schema::string())
                   .field("path", &PreviewResult::path, schema::string().doc("the PNG"))
                   .field("width", &PreviewResult::width, schema::integer().doc("pixels"))
                   .field("height", &PreviewResult::height, schema::integer().doc("pixels"))
                   .field("problems", &PreviewResult::problems, schema::arrayOf(schema::string()))
                   .field("log", &PreviewResult::log, schema::arrayOf(schema::string()))
                   .build();
    c.errors = {{"dsp.args", "neither or both of plugin and script", "give exactly one"},
                {"dsp.not_listed", "the script compiles but no plugin has its id",
                 "put the script player on the plugin path"},
                {"dsp.signal", "a signal the plugin cannot play",
                 "an effect plays audio signals, an instrument notes"},
                {"dsp.no_picture", "the plugin cannot draw its editor without a window",
                 "scripts can, through the script player; other plugins need the editor-picture "
                 "extension"}};
    {
        json::Value args = json::Value::object();
        args["script"] = "gain";
        c.examples = {{"See a script's editor", std::move(args),
                       detail::j(R"({"plugin":"com.example.gain","path":"<any>","width":"<any>",
                                     "height":"<any>","problems":[],"log":"<any>"})"),
                       "dsp_player"}};
    }
    c.run = [](const PreviewArgs& a, Context& ctx) -> Result<PreviewResult> {
        Host& s = ctx.service<Host>();
        using Traits = TestTraits<Host>;
        if (a.plugin.has_value() == a.script.has_value())
            return Error("dsp.args", "give exactly one of plugin and script", "",
                         "plugin is an id from plugin.list; script is a name from dsp.list");
        std::string id;
        if (a.plugin) {
            id = *a.plugin;
        } else {
            auto text = detail::readScript(s, *a.script, "/script");
            if (!text) return std::move(text).error();
            auto compiled = detail::compile(*text);
            if (!compiled.plugin)
                return Error("dsp.compile", "the script does not compile", "/script",
                             "dsp.check {name: \"" + *a.script + "\"} gives its diagnostics");
            id = compiled.plugin->id;
        }
        auto info = s.findPlugin(id);
        if (!info && a.script) {
            (void)s.plugins(true);  // a script written since the last scan
            info = s.findPlugin(id);
            if (!info)
                return Error("dsp.not_listed", "no plugin has the id " + id, "/script",
                             "the script player must be on the plugin path (--plugin-path)");
        }
        if (!info) return std::move(info).error().at("/plugin");
        auto created = host::Instance::create(*info, Traits::identity(s));
        if (!created) return std::move(created).error();
        host::Instance& instance = **created;
        if (!instance.drawsEditor())
            return Error("dsp.no_picture", info->name + " cannot draw its editor without a window",
                         a.plugin ? "/plugin" : "/script",
                         "scripts can, through the script player; other plugins need "
                         "com.suppleaudio.editor-picture/1");
        const bool instrument = instance.audioInputChannels() == 0;
        const Signal sig = a.signal.value_or(instrument ? Signal::Notes : Signal::Sine);
        if (sig != Signal::Silence && forInstruments(sig) != instrument)
            return Error("dsp.signal",
                         info->name +
                             (instrument ? " is an instrument: it plays notes, not "
                                         : " takes audio, not ") +
                             signalName(sig),
                         "/signal",
                         instrument ? "use notes, note or silence"
                                    : "use sine, sweep, impulse, noise or silence");
        if (a.patch) {
            if (!instance.info().semantic)
                return Error("dsp.not_semantic", info->name + " takes no patch", "/patch",
                             "set it by plugin.load and param.set instead");
            SUPPLE_TRY(instance.setPatch(json::writeCompact(*a.patch), CLAP_SUPPLE_SET_MERGE));
        }

        PreviewResult out;
        out.plugin = info->id;
        SUPPLE_TRY(instance.watchEditor(true));
        if (a.seconds > 0.0) {
            const sandbox::RenderSettings settings = Traits::settings(s);
            const audio::AudioData input =
                instrument
                    ? audio::AudioData{}
                    : stimulus(sig, settings.sampleRate, a.seconds, a.levelDb, a.frequencyHz);
            sandbox::RenderRequest request;
            if (instrument) {
                request.events = noteEvents(sig, settings.sampleRate, a.seconds);
                request.lengthSamples =
                    static_cast<std::int64_t>(std::llround(a.seconds * settings.sampleRate));
            } else {
                request.input = &input;
            }
            host::Instance* chain[] = {&instance};
            auto r = sandbox::render(chain, settings, request);
            if (!r) return std::move(r).error();
            out.log = r->log;
        }
        auto picture = instance.drawEditor(a.scale);
        (void)instance.watchEditor(false);
        if (!picture)
            return Error(
                "dsp.no_picture", picture.error().message, a.plugin ? "/plugin" : "/script",
                "drawing without a window needs EGL (Mesa's libEGL), with or without a GPU");
        for (auto& line : instance.takeLog()) {
            if (line.find("loop budget") != std::string::npos ||
                line.find("outside an array") != std::string::npos)
                out.problems.push_back(line);
            out.log.push_back(info->name + ": " + line);
        }
        const std::filesystem::path file =
            Traits::outputDir(s) / ("dsp-preview-" + info->id + ".png");
        SUPPLE_TRY(png::write(file, picture->width, picture->height, picture->rgba));
        out.path = file.string();
        out.width = picture->width;
        out.height = picture->height;
        return out;
    };
    registry.add(std::move(c));
}

}  // namespace supple::dsp_commands
