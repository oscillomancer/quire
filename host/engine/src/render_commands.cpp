#include "render_commands.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>

#include "../../analysis-commands/analysis_shared.h"
#include "analysis_commands.h"
#include "supple/dsp/dsp.h"
#include "supple/engine/app.h"

namespace supple::engine {
namespace {
using cmd::Command;
using cmd::Context;
using cmd::Kind;
namespace fs = std::filesystem;

struct Args {
    std::optional<std::string> branch, pattern, name, mode, tap, format, operation;
    std::optional<json::Value> normalize;
    std::optional<std::vector<std::string>> tracks, regions;
    std::optional<double> startBeat, endBeat, tailS, target, limitTruePeakDb;
    std::optional<std::int64_t> bitDepth;
    std::optional<std::int64_t> sampleRate, channels;
    bool job = false;
};
struct AbArgs {
    std::string a, b;
    std::optional<double> startBeat, endBeat, tailS;
};
struct RegionArgs {
    std::string name;
    double startBeat = 0, endBeat = 0;
};
struct RegionResult {
    std::string region;
};
using PatternData = RenderPatternContext;
struct RenderTask {
    model::Session session;
    OfflineSettings settings;
    fs::path path;
    std::string label;
};

json::Value obj(std::string_view s) { return json::parse(s).value(); }
std::string pad(int n, int width) {
    std::ostringstream out;
    out << std::setw(width) << std::setfill('0') << n;
    return out.str();
}
std::string compact(double v) {
    std::ostringstream out;
    out << std::fixed << std::setprecision(3) << v;
    std::string s = out.str();
    while (s.ends_with('0')) s.pop_back();
    if (s.ends_with('.')) s.pop_back();
    return s;
}
Result<std::string> token(std::string_view name, const PatternData& d) {
    auto colon = name.find(':');
    const std::string key(name.substr(0, colon));
    const std::string arg =
        colon == std::string_view::npos ? "" : std::string(name.substr(colon + 1));
    if (key == "project" && arg.empty()) return d.project;
    if (key == "track" && arg.empty()) return d.track;
    if (key == "folder_path" && arg.empty()) return d.folderPath;
    if (key == "region" && arg.empty()) return d.region;
    if (key == "marker" && arg.empty()) return d.marker;
    if (key == "item" && arg.empty()) return d.item;
    if (key == "take" && arg.empty()) return d.take;
    if (key == "format" && arg.empty()) return d.format;
    if (key == "sample_rate" && arg.empty()) return std::to_string(d.sampleRate);
    if (key == "bit_depth" && arg.empty()) return std::to_string(d.bitDepth);
    if (key == "track_number" || key == "region_number" || key == "counter") {
        if (!arg.empty() &&
            (arg.size() > 3 || arg[0] != '0' ||
             !std::all_of(arg.begin(), arg.end(), [](unsigned char c) { return std::isdigit(c); })))
            return Error("render.pattern", "invalid number format", "/pattern", "use :02 or :03");
        int width = 1;
        if (!arg.empty()) {
            width = 0;
            for (char c : arg) width = width * 10 + (c - '0');
        }
        return pad(key == "track_number"    ? d.trackNumber
                   : key == "region_number" ? d.regionNumber
                                            : d.counter,
                   width);
    }
    if (key == "start") {
        if (arg == "beats") return compact(d.startBeats);
        if (arg == "seconds") return compact(d.startSeconds);
        if (arg == "timecode") {
            const int sec = static_cast<int>(d.startSeconds);
            return pad(sec / 3600, 2) + "-" + pad((sec / 60) % 60, 2) + "-" + pad(sec % 60, 2);
        }
    }
    if (key == "date" && arg == "YYYY-MM-DD") {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm t{};
        gmtime_r(&now, &t);
        char buf[16];
        std::strftime(buf, sizeof buf, "%Y-%m-%d", &t);
        return std::string(buf);
    }
    if (key == "time" && arg == "HH-mm-ss") {
        const auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        std::tm t{};
        gmtime_r(&now, &t);
        char buf[16];
        std::strftime(buf, sizeof buf, "%H-%M-%S", &t);
        return std::string(buf);
    }
    return Error("render.pattern", "unknown or invalid token {" + std::string(name) + "}",
                 "/pattern", "use the closed token set in session-model §7.3");
}
Result<std::string> expand(std::string_view pattern, const PatternData& d) {
    std::string out;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] != '{') {
            if (pattern[i] == '}') return Error("render.pattern", "unmatched }", "/pattern");
            out += pattern[i++];
            continue;
        }
        auto end = pattern.find('}', i + 1);
        if (end == std::string_view::npos) return Error("render.pattern", "unclosed {", "/pattern");
        std::string inside(pattern.substr(i + 1, end - i - 1));
        auto pipe = inside.find('|');
        auto value = token(std::string_view(inside).substr(0, pipe), d);
        if (!value) return std::move(value).error();
        while (pipe != std::string::npos) {
            auto next = inside.find('|', pipe + 1);
            std::string filter =
                inside.substr(pipe + 1, next == std::string::npos ? next : next - pipe - 1);
            if (filter == "lower")
                std::transform(value->begin(), value->end(), value->begin(),
                               [](unsigned char c) { return std::tolower(c); });
            else if (filter == "upper")
                std::transform(value->begin(), value->end(), value->begin(),
                               [](unsigned char c) { return std::toupper(c); });
            else if (filter.starts_with("max(") && filter.ends_with(')')) {
                const std::string n = filter.substr(4, filter.size() - 5);
                std::size_t maximum = 0;
                const auto parsed = std::from_chars(n.data(), n.data() + n.size(), maximum);
                if (n.empty() || parsed.ec != std::errc{} || parsed.ptr != n.data() + n.size())
                    return Error("render.pattern", "invalid max filter", "/pattern");
                value->resize(std::min(value->size(), maximum));
            } else if (filter.starts_with("replace(\"") && filter.ends_with("\")")) {
                const auto mid = filter.find("\",\"");
                if (mid == std::string::npos)
                    return Error("render.pattern", "invalid replace filter", "/pattern");
                const std::string from = filter.substr(9, mid - 9),
                                  to = filter.substr(mid + 3, filter.size() - mid - 5);
                if (from.empty())
                    return Error("render.pattern", "empty replace source", "/pattern");
                for (std::size_t at = 0; (at = value->find(from, at)) != std::string::npos;
                     at += to.size())
                    value->replace(at, from.size(), to);
            } else
                return Error("render.pattern", "unknown filter " + filter, "/pattern");
            pipe = next;
        }
        bool removedSeparator = false;
        if (value->empty() && !out.empty() &&
            std::string("-_. ").find(out.back()) != std::string::npos) {
            out.pop_back();
            removedSeparator = true;
        } else
            out += *value;
        i = end + 1;
        if (value->empty() && !removedSeparator && i < pattern.size() &&
            std::string("-_. ").find(pattern[i]) != std::string::npos)
            ++i;
    }
    if (out.empty() || out == "." || out == ".." || fs::path(out).is_absolute() ||
        out.find("..") != std::string::npos || out.find('\\') != std::string::npos)
        return Error("render.path", "render name escapes the renders directory", "/pattern",
                     "use a relative name without ..");
    return out;
}

Result<model::Session> sessionFor(EngineApp& app, const std::optional<std::string>& branch) {
    if (!branch) return app.view();
    auto head = app.versions().head(*branch);
    if (!head)
        return Error("branch.unknown", "unknown branch " + *branch, "/branch", "use branch.list");
    return *head;
}
Result<model::Id> trackId(const model::Session& s, const std::string& text) {
    auto id = model::Id::parse(text);
    if (!id || !s.tracks.contains(*id))
        return Error("track.unknown", "no track " + text, "/tracks", "use track.list");
    return *id;
}
Result<model::Id> regionId(const model::Session& s, const std::string& text) {
    auto id = model::Id::parse(text);
    if (!id || !s.regions.contains(*id))
        return Error("region.unknown", "no region " + text, "/regions", "use region.list");
    return *id;
}

double lastBeat(const model::Session& s) {
    double end = 0;
    for (const auto& e : s.items) end = std::max(end, e.value->position + e.value->length);
    return end;
}
Result<std::vector<RenderTask>> plan(EngineApp& app, std::string_view command, const Args& a) {
    auto source = sessionFor(app, a.branch);
    if (!source) return std::move(source).error();
    model::Session s = *source;
    const json::Value& defaults = s.project->render;
    auto setting = [&](std::string_view key) -> const json::Value* {
        return defaults.isObject() ? defaults.find(key) : nullptr;
    };
    const auto* formatSetting = setting("format");
    const std::string formatName = a.format.value_or(
        formatSetting && formatSetting->isString() ? formatSetting->asString()
        : formatSetting && formatSetting->isObject() && formatSetting->find("container") &&
                formatSetting->find("container")->isString()
            ? formatSetting->find("container")->asString()
            : "wav");
    if (formatName != "wav")
        return Error("render.format", "only WAV encoding is available", "/format");
    std::string pattern = a.name.value_or(a.pattern.value_or(
        setting("pattern") && setting("pattern")->isString() ? setting("pattern")->asString()
        : command == "render.mix"                            ? "mix-v" + std::to_string(s.version)
                                                             : "{project}-{track}-{region}"));
    std::string mode = a.mode.value_or("isolated");
    if (mode != "isolated" && mode != "through_master")
        return Error("render.mode", "unknown stem mode", "/mode", "use isolated or through_master");
    std::string tap = a.tap.value_or("post_fader");
    if (tap != "post_fader" && tap != "pre_fader" && tap != "pre_fx")
        return Error("render.tap", "unknown tap", "/tap");
    std::vector<RenderTask> tasks;
    auto add = [&](std::optional<model::Id> track, std::optional<model::Id> region, int tn,
                   int rn) -> Status {
        const auto* range = region ? s.regions.find(*region) : nullptr;
        const double first =
            range ? (range->timebase == model::Timebase::Time ? s.timeMap->beatAt(range->start)
                                                              : range->start)
                  : a.startBeat.value_or(0);
        const double last =
            range ? (range->timebase == model::Timebase::Time ? s.timeMap->beatAt(range->end)
                                                              : range->end)
                  : a.endBeat.value_or(lastBeat(s));
        if (!(last > first))
            return Error("render.empty", "nothing to render", "/end_beat",
                         "add items or set an end beat");
        RenderTask task;
        task.session = s;
        task.settings.startSeconds = s.timeMap->secondsAt(first);
        task.settings.endSeconds = s.timeMap->secondsAt(last);
        if (!app.projectPath().empty()) task.settings.projectDir = app.projectPath().parent_path();
        task.settings.tailSeconds =
            a.tailS.value_or(setting("tail_ms") && setting("tail_ms")->isNumber()
                                 ? setting("tail_ms")->asDouble() / 1000.0
                                 : 0.0);
        if (task.settings.tailSeconds < 0 || task.settings.tailSeconds > 600)
            return Error("render.tail", "invalid tail", "/tail_s");
        if (track) {
            if (mode == "through_master")
                task.settings.throughMaster = track;
            else
                task.settings.track = track;
            task.settings.tap = tap == "pre_fx"      ? model::Tap::PreFx
                                : tap == "pre_fader" ? model::Tap::PreFader
                                                     : model::Tap::PostFader;
        }
        PatternData data;
        data.project = s.project->name.empty() ? "project" : s.project->name;
        data.track = track ? s.tracks.find(*track)->name : "";
        data.region = region ? s.regions.find(*region)->name : "";
        if (track && tn == 0) {
            for (const auto& entry : s.tracks) {
                if (entry.id == model::kMaster) continue;
                ++tn;
                if (entry.id == *track) break;
            }
        }
        data.trackNumber = tn;
        data.regionNumber = rn;
        data.startBeats = first;
        data.startSeconds = task.settings.startSeconds;
        if (track) {
            auto parent = s.tracks.find(*track)->parent;
            std::vector<std::string> folders;
            while (parent && *parent != model::kMaster) {
                const auto* folder = s.tracks.find(*parent);
                if (!folder) break;
                folders.push_back(folder->name);
                parent = folder->parent;
            }
            for (auto it = folders.rbegin(); it != folders.rend(); ++it) {
                if (!data.folderPath.empty()) data.folderPath += '/';
                data.folderPath += *it;
            }
        }
        double nearestMarker = -1;
        for (const auto& marker : s.markers) {
            const double position = marker.value->timebase == model::Timebase::Time
                                        ? s.timeMap->beatAt(marker.value->pos)
                                        : marker.value->pos;
            if (position <= first && position >= nearestMarker) {
                nearestMarker = position;
                data.marker = marker.value->name;
            }
        }
        for (const auto& item : s.items)
            if ((!track || item.value->track == *track) && item.value->position <= first &&
                item.value->position + item.value->length > first) {
                data.item = item.id.str();
                if (item.value->activeTake) {
                    const auto* take = s.takes.find(*item.value->activeTake);
                    if (take)
                        data.take = take->name.empty() ? item.value->activeTake->str() : take->name;
                }
                break;
            }
        const auto* rate = setting("sample_rate");
        data.sampleRate = static_cast<int>(a.sampleRate.value_or(
            rate && rate->isNumber() ? rate->asInt()
                                     : static_cast<std::int64_t>(s.project->sampleRate)));
        const auto* formatDefault = setting("format");
        const auto* depth = formatDefault && formatDefault->isObject()
                                ? formatDefault->find("bit_depth")
                                : setting("bit_depth");
        data.bitDepth =
            static_cast<int>(a.bitDepth.value_or(depth && depth->isNumber() ? depth->asInt() : 32));
        data.format = formatName;
        data.counter = static_cast<int>(tasks.size() + 1);
        auto name = expand(pattern, data);
        if (!name) return std::move(name).error();
        fs::path rel = fs::path(*name + ".wav").lexically_normal();
        if (rel.is_absolute() || rel.empty() || *rel.begin() == "..")
            return Error("render.path", "render name escapes directory", "/pattern");
        task.path = app.rendersDir() / rel;
        std::error_code ec;
        const auto root = fs::weakly_canonical(app.rendersDir(), ec);
        if (ec) return Error("render.path", "cannot resolve renders directory", "/pattern");
        const auto parent = fs::weakly_canonical(task.path.parent_path(), ec);
        if (ec) return Error("render.path", "cannot resolve render parent directory", "/pattern");
        const auto relative = parent.lexically_relative(root);
        if (relative.empty() || *relative.begin() == ".." || relative.is_absolute())
            return Error("render.path", "render name escapes the renders directory", "/pattern");
        task.label = *name;
        for (const auto& prior : tasks)
            if (prior.path == task.path)
                return Error("render.path", "render files collide", "/pattern",
                             "include {track}, {region}, or {counter}");
        tasks.push_back(std::move(task));
        return {};
    };
    if (command == "render.mix") {
        SUPPLE_TRY(add({}, {}, 0, 0));
    } else if (command == "render.stems") {
        std::vector<model::Id> ids;
        if (a.tracks)
            for (const auto& text : *a.tracks) {
                auto id = trackId(s, text);
                if (!id) return std::move(id).error();
                ids.push_back(*id);
            }
        else
            for (const auto& e : s.tracks)
                if (e.value->parent == model::kMaster) ids.push_back(e.id);
        if (ids.empty()) return Error("render.empty", "no stem tracks", "/tracks");
        int n = 0;
        for (auto id : ids) SUPPLE_TRY(add(id, {}, ++n, 0));
    } else {
        std::vector<model::Id> regions;
        if (a.regions)
            for (const auto& text : *a.regions) {
                auto id = regionId(s, text);
                if (!id) return std::move(id).error();
                regions.push_back(*id);
            }
        else
            for (const auto& e : s.regions) regions.push_back(e.id);
        const json::Value* matrix = setting("matrix");
        const json::Value* projectRow =
            matrix && matrix->isObject() ? matrix->find("project") : nullptr;
        if (regions.empty() && !projectRow && command == "render.regions")
            return Error("render.empty", "no regions", "/regions");
        if (projectRow && projectRow->isArray() && !a.regions) {
            for (const auto& cell : projectRow->asArray()) {
                if (!cell.isString())
                    return Error("render.matrix", "matrix entries must be ids or master",
                                 "/matrix");
                if (cell.asString() == "master")
                    SUPPLE_TRY(add({}, {}, 0, 0));
                else {
                    auto tr = trackId(s, cell.asString());
                    if (!tr) return std::move(tr).error();
                    SUPPLE_TRY(add(*tr, {}, 0, 0));
                }
            }
        }
        int n = 0;
        for (auto id : regions) {
            ++n;
            const json::Value* row =
                matrix && matrix->isObject() ? matrix->find(id.str()) : nullptr;
            if (row && row->isArray()) {
                for (const auto& cell : row->asArray()) {
                    if (!cell.isString())
                        return Error("render.matrix", "matrix entries must be ids or master",
                                     "/matrix");
                    if (cell.asString() == "master")
                        SUPPLE_TRY(add({}, id, 0, n));
                    else {
                        auto tr = trackId(s, cell.asString());
                        if (!tr) return std::move(tr).error();
                        SUPPLE_TRY(add(*tr, id, 0, n));
                    }
                }
            } else
                SUPPLE_TRY(add({}, id, 0, n));
        }
    }
    if (tasks.empty())
        return Error("render.empty", "render selection produced no files", {},
                     "select at least one region or matrix cell");
    return tasks;
}

json::Value fileList(const std::vector<RenderTask>& tasks) {
    json::Value list = json::Value::array();
    for (const auto& task : tasks) {
        json::Value entry = json::Value::object();
        entry["path"] = task.path.string();
        entry["seconds"] =
            task.settings.endSeconds - task.settings.startSeconds + task.settings.tailSeconds;
        list.push_back(std::move(entry));
    }
    return list;
}

Result<json::Value> runTasks(EngineApp& app, const std::vector<RenderTask>& tasks, const Args& args,
                             cmd::JobManager* jobs = nullptr, std::string_view job = {},
                             const std::vector<host::PluginInfo>* catalogue = nullptr,
                             const host::HostIdentity* identity = nullptr,
                             std::stop_token stop = {}) {
    json::Value files = json::Value::array();
    for (std::size_t i = 0; i < tasks.size(); ++i) {
        if (jobs && jobs->cancelled(job)) return Error("job.cancelled", "render cancelled");
        const auto& task = tasks[i];
        OfflineSettings settings = task.settings;
        if (jobs) {
            settings.cancelFlag = jobs->cancellationFlag(job);
            settings.progressFlag = jobs->progressFlag(job);
            settings.progressBase = static_cast<double>(i) / tasks.size();
            settings.progressScale = 0.8 / tasks.size();
        }
        settings.stopToken = stop;
        auto rendered = catalogue && identity
                            ? renderSnapshot(task.session, settings, *catalogue, *identity)
                            : app.renderSession(task.session, settings);
        if (!rendered) return std::move(rendered).error();
        auto& audio = rendered->audio;
        const json::Value& defaults = task.session.project->render;
        auto setting = [&](std::string_view key) -> const json::Value* {
            return defaults.isObject() ? defaults.find(key) : nullptr;
        };
        const auto* sampleRate = setting("sample_rate");
        const double targetRate = args.sampleRate ? static_cast<double>(*args.sampleRate)
                                  : sampleRate && sampleRate->isNumber() ? sampleRate->asDouble()
                                                                         : audio.sampleRate;
        if (!(targetRate >= 8000 && targetRate <= 384000))
            return Error("render.sample_rate", "sample rate out of range", "/sample_rate");
        if (targetRate != audio.sampleRate) {
            auto converted = audio::resample(audio, targetRate);
            if (!converted) return std::move(converted).error();
            audio = std::move(*converted);
        }
        const auto* channelDefault = setting("channels");
        const int channels = static_cast<int>(args.channels.value_or(
            channelDefault && channelDefault->isNumber() ? channelDefault->asInt() : 2));
        if (channels != 1 && channels != 2)
            return Error("render.channels", "channels must be 1 or 2", "/channels");
        if (channels == 1 && audio.channelCount() == 2) {
            for (std::size_t n = 0; n < audio.frames(); ++n)
                audio.channels[0][n] =
                    static_cast<float>(0.5 * (static_cast<double>(audio.channels[0][n]) +
                                              static_cast<double>(audio.channels[1][n])));
            audio.channels.resize(1);
        }
        const json::Value* norm = args.normalize ? &*args.normalize : setting("normalize");
        std::string normMode = "none";
        double normTarget = args.target.value_or(-14.0);
        std::optional<double> limit = args.limitTruePeakDb;
        if (norm) {
            if (norm->isString())
                normMode = norm->asString();
            else if (norm->isObject()) {
                const auto* mode = norm->find("mode");
                if (!mode || !mode->isString())
                    return Error("render.normalize", "normalize needs a mode", "/normalize/mode");
                normMode = mode->asString();
                if (const auto* target = norm->find("target")) {
                    if (!target->isNumber())
                        return Error("render.normalize", "target must be a number",
                                     "/normalize/target");
                    normTarget = target->asDouble();
                }
                if (const auto* peak = norm->find("limit_true_peak_db")) {
                    if (!peak->isNumber())
                        return Error("render.normalize", "peak limit must be a number",
                                     "/normalize/limit_true_peak_db");
                    limit = peak->asDouble();
                }
            } else
                return Error("render.normalize", "normalize must be a mode or object",
                             "/normalize");
        }
        SUPPLE_TRY(normalizeRenderAudio(audio, normMode, normTarget, limit));
        audio::WavOptions wav;
        const auto* formatDefault = setting("format");
        const std::string formatName = args.format.value_or(
            formatDefault && formatDefault->isString() ? formatDefault->asString()
            : formatDefault && formatDefault->isObject() && formatDefault->find("container") &&
                    formatDefault->find("container")->isString()
                ? formatDefault->find("container")->asString()
                : "wav");
        if (formatName != "wav")
            return Error("render.format", "only WAV encoding is available", "/format");
        const json::Value* depthDefault = formatDefault && formatDefault->isObject()
                                              ? formatDefault->find("bit_depth")
                                              : setting("bit_depth");
        const int depth = static_cast<int>(args.bitDepth.value_or(
            depthDefault && depthDefault->isNumber() ? depthDefault->asInt() : 32));
        if (depth != 16 && depth != 24 && depth != 32)
            return Error("render.format", "bit_depth must be 16, 24, or 32", "/bit_depth");
        wav.format = depth == 16   ? audio::SampleFormat::Pcm16
                     : depth == 24 ? audio::SampleFormat::Pcm24
                                   : audio::SampleFormat::Float32;
        wav.ditherSeed = task.session.project->seed;
        if (formatDefault && formatDefault->isObject())
            if (const auto* dither = formatDefault->find("dither"); dither && dither->isBool())
                wav.dither = dither->asBool();
        if (jobs && jobs->cancelled(job)) return Error("job.cancelled", "render cancelled");
        SUPPLE_TRY(audio::writeWav(task.path, audio, wav));
        json::Value entry = json::Value::object();
        entry["path"] = task.path.string();
        entry["render"] = app.rememberRender(task.path);
        entry["seconds"] = audio.seconds();
        entry["output_latency"] = static_cast<std::int64_t>(rendered->outputLatency);
        if (!rendered->warnings.empty()) {
            json::Value warnings = json::Value::array();
            for (const auto& warning : rendered->warnings) warnings.push_back(warning);
            entry["warnings"] = std::move(warnings);
        }
        auto report = analyzeAudio(audio, analysis::Detail::Summary);
        if (report) entry["analysis"] = std::move(*report);
        files.push_back(std::move(entry));
        if (jobs)
            (void)jobs->progress(job, static_cast<double>(i + 1) / tasks.size(),
                                 "rendered " + task.label);
    }
    json::Value result = json::Value::object();
    result["files"] = std::move(files);
    return result;
}

schema::Schema<Args> argsSchema() {
    return schema::object<Args>("render_args")
        .field("branch", &Args::branch, schema::string())
        .field("pattern", &Args::pattern, schema::string())
        .field("name", &Args::name, schema::string())
        .field("mode", &Args::mode, schema::string())
        .field("tap", &Args::tap, schema::string())
        .field("format", &Args::format, schema::string())
        .field("normalize", &Args::normalize, schema::any())
        .field("operation", &Args::operation, schema::string())
        .field("tracks", &Args::tracks, schema::arrayOf(schema::string()))
        .field("regions", &Args::regions, schema::arrayOf(schema::string()))
        .field("start_beat", &Args::startBeat, schema::number().min(0))
        .field("end_beat", &Args::endBeat, schema::number().exclusiveMin(0))
        .field("tail_s", &Args::tailS, schema::number().range(0, 600))
        .field("target", &Args::target, schema::number())
        .field("limit_true_peak_db", &Args::limitTruePeakDb, schema::number())
        .field("bit_depth", &Args::bitDepth, schema::integer().range(16, 32))
        .field("sample_rate", &Args::sampleRate, schema::integer().range(8000, 384000))
        .field("channels", &Args::channels, schema::integer().range(1, 2))
        .field("job", &Args::job, schema::boolean())
        .defaultValue(false)
        .build();
}
}  // namespace

Result<OfflineResult> renderSnapshot(const model::Session& session, OfflineSettings settings,
                                     const std::vector<host::PluginInfo>& catalogue,
                                     const host::HostIdentity& identity) {
    InstancePool pool(
        [&catalogue](const model::PluginRef& ref) -> Result<host::PluginInfo> {
            for (const auto& plugin : catalogue)
                if (plugin.id == ref.id) return plugin;
            return Error("plugin.unknown", "plugin unavailable in the render snapshot: " + ref.id);
        },
        identity);
    return renderOffline(session, pool, settings);
}

Status normalizeRenderAudio(audio::AudioData& data, std::string_view mode, double target,
                            std::optional<double> limitTruePeakDb) {
    if (mode == "none") return okStatus();
    auto measure = [&]() -> Result<analysis::Report> {
        auto pointers = data.pointers();
        const auto layout =
            pointers.size() == 1 ? analysis::Layout::Mono : analysis::Layout::Stereo;
        analysis::Options options;
        options.spectral = false;
        options.harmonic = false;
        options.envelope = false;
        options.stereo = false;
        return analysis::analyze(
            {std::span<const float* const>(pointers.data(), pointers.size()),
             static_cast<std::int64_t>(data.frames()), data.sampleRate, layout},
            options);
    };
    auto report = measure();
    if (!report) return std::move(report).error();
    double measured = 0;
    if (mode == "lufs_i")
        measured = report->loudness->integrated;
    else if (mode == "lufs_s")
        measured = report->loudness->maxShortTerm;
    else if (mode == "lufs_m")
        measured = report->loudness->maxMomentary;
    else if (mode == "peak")
        measured = report->loudness->samplePeak;
    else if (mode == "true_peak")
        measured = report->loudness->truePeak;
    else if (mode == "rms") {
        double sum = 0;
        for (const auto& channel : data.channels)
            for (float sample : channel)
                sum += static_cast<double>(sample) * static_cast<double>(sample);
        measured = sum > 0 ? 10.0 * std::log10(sum / (data.frames() * data.channelCount()))
                           : -std::numeric_limits<double>::infinity();
    } else
        return Error("render.normalize", "unsupported normalization mode", "/normalize");
    if (std::isfinite(measured)) {
        const double gain = std::pow(10.0, (target - measured) / 20.0);
        for (auto& channel : data.channels)
            for (float& sample : channel)
                sample = static_cast<float>(static_cast<double>(sample) * gain);
    }
    if (limitTruePeakDb) {
        const double ceiling = std::pow(10.0, *limitTruePeakDb / 20.0);
        if (!(ceiling > 0 && ceiling <= 1))
            return Error("render.normalize", "true peak limit must be at or below 0 dBTP",
                         "/normalize/limit_true_peak_db");
        const std::size_t frames = data.frames();
        std::vector<double> peaks(frames, 0.0);
        const int factor = data.sampleRate <= 48000 ? 4 : data.sampleRate <= 96000 ? 2 : 1;
        for (const auto& channel : data.channels) {
            dsp::PolyphaseUpsampler upsampler;
            if (factor > 1) upsampler.prepare(factor);
            std::array<double, 4> phases{};
            for (std::size_t i = 0; i < frames + (factor > 1 ? 16 : 0); ++i) {
                const float sample = i < frames ? channel[i] : 0.0f;
                if (i < frames)
                    peaks[i] = std::max(peaks[i], std::abs(static_cast<double>(sample)));
                if (factor > 1) {
                    upsampler.process(sample, std::span<double>(phases.data(), factor));
                    if (i >= 16 && i - 16 < frames)
                        for (int phase = 0; phase < factor; ++phase)
                            peaks[i - 16] = std::max(
                                peaks[i - 16], std::abs(phases[static_cast<std::size_t>(phase)]));
                }
            }
        }
        std::vector<double> gain(frames, 1.0);
        for (std::size_t i = 0; i < frames; ++i)
            if (peaks[i] > ceiling) gain[i] = ceiling / peaks[i];
        const double attackStep = 1.0 / std::max(1.0, data.sampleRate * 0.005);
        for (std::size_t i = frames; i-- > 1;)
            gain[i - 1] = std::min(gain[i - 1], gain[i] + attackStep);
        const double releaseStep = 1.0 / std::max(1.0, data.sampleRate * 0.05);
        double envelope = 1.0;
        for (std::size_t i = 0; i < frames; ++i) {
            envelope = std::min(gain[i], envelope + releaseStep);
            for (auto& channel : data.channels)
                channel[i] = static_cast<float>(static_cast<double>(channel[i]) * envelope);
        }
        // The final check catches intersample peaks introduced by gain changes.
        auto post = measure();
        if (!post) return std::move(post).error();
        if (post->loudness->truePeak > *limitTruePeakDb) {
            const double finalTrim =
                std::pow(10.0, (*limitTruePeakDb - post->loudness->truePeak) / 20.0);
            for (auto& channel : data.channels)
                for (float& sample : channel)
                    sample = static_cast<float>(static_cast<double>(sample) * finalTrim);
        }
    }
    return okStatus();
}

Result<std::string> expandRenderPattern(std::string_view pattern,
                                        const RenderPatternContext& context) {
    return expand(pattern, context);
}

void registerRenderCommands(cmd::Registry& registry) {
    {
        Command<RegionArgs, RegionResult> c;
        c.name = "region.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add a named render region in beats.";
        c.tags = {"region"};
        c.args = schema::object<RegionArgs>("region_add_args")
                     .field("name", &RegionArgs::name, schema::string().minLength(1))
                     .field("start_beat", &RegionArgs::startBeat, schema::number().min(0))
                     .field("end_beat", &RegionArgs::endBeat, schema::number().exclusiveMin(0))
                     .build();
        c.result = schema::object<RegionResult>("region_add_result")
                       .field("region", &RegionResult::region, schema::string())
                       .build();
        c.examples = {{"A verse", obj(R"({"name":"Verse","start_beat":0,"end_beat":4})"),
                       obj(R"({"region":"rg_1"})"), "song"}};
        c.run = [](const RegionArgs& a, Context& ctx) -> Result<RegionResult> {
            if (!(a.endBeat > a.startBeat))
                return Error("region.range", "region end must follow start", "/end_beat");
            model::Region region;
            region.name = a.name;
            region.start = a.startBeat;
            region.end = a.endBeat;
            return RegionResult{ctx.service<EngineApp>().txn().create(std::move(region)).str()};
        };
        registry.add(std::move(c));
    }
    for (const char* name : {"render.stems", "render.regions", "render.dry-run"}) {
        Command<Args, json::Value> c;
        c.name = name;
        c.kind = std::string_view(name) == "render.dry-run" ? Kind::Query : Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = std::string(name) + " resolves and renders project audio files.";
        c.tags = {"render"};
        c.args = argsSchema();
        c.result = schema::any();
        c.examples = {{"Render project", obj(R"({"end_beat":1,"pattern":"example-{counter:02}"})"),
                       obj(R"({"files":"<any>"})"),
                       std::string_view(name) == "render.regions" ? "song_with_region" : "song"}};
        c.run = [name](const Args& a, Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            const std::string mode = std::string_view(name) == "render.dry-run"
                                         ? (a.operation ? "render." + *a.operation
                                            : a.regions ? "render.regions"
                                            : a.tracks  ? "render.stems"
                                                        : "render.mix")
                                         : name;
            if (mode != "render.mix" && mode != "render.stems" && mode != "render.regions")
                return Error("render.operation", "unknown render operation", "/operation",
                             "use mix, stems, or regions");
            auto tasks = plan(app, mode, a);
            if (!tasks) return std::move(tasks).error();
            if (std::string_view(name) == "render.dry-run") {
                json::Value out = json::Value::object();
                out["files"] = fileList(*tasks);
                return out;
            }
            if (a.format && *a.format != "wav")
                return Error("render.format", "only WAV encoding is available", "/format");
            if (!a.job) return runTasks(app, *tasks, a);
            auto available = app.plugins();
            if (!available) return std::move(available).error();
            std::vector<host::PluginInfo> catalogue(available->begin(), available->end());
            const auto identity = app.options().identity;
            auto& jobs = ctx.service<cmd::JobManager>();
            const std::string id = jobs.create();
            app.startJob([&app, &jobs, id, tasks = std::move(*tasks), a,
                          catalogue = std::move(catalogue), identity](std::stop_token stop) {
                if (stop.stop_requested() || jobs.cancelled(id)) return;
                auto result = runTasks(app, tasks, a, &jobs, id, &catalogue, &identity, stop);
                if (result)
                    (void)jobs.complete(id, std::move(*result));
                else if (!jobs.cancelled(id))
                    (void)jobs.fail(id, std::move(result).error());
            });
            json::Value out = json::Value::object();
            out["job"] = id;
            out["status"] = "running";
            return out;
        };
        registry.add(std::move(c));
    }
    Command<AbArgs, json::Value> ab;
    ab.name = "branch.render-ab";
    ab.kind = Kind::Action;
    ab.exposure = cmd::Exposure::McpTool;
    ab.summary = "Render two branch heads over the same range and compare them.";
    ab.tags = {"branch", "render"};
    ab.args = schema::object<AbArgs>("render_ab_args")
                  .field("a", &AbArgs::a, schema::string())
                  .field("b", &AbArgs::b, schema::string())
                  .field("start_beat", &AbArgs::startBeat, schema::number().min(0))
                  .field("end_beat", &AbArgs::endBeat, schema::number().exclusiveMin(0))
                  .field("tail_s", &AbArgs::tailS, schema::number().range(0, 600))
                  .build();
    ab.result = schema::any();
    ab.examples = {{"Compare main with itself", obj(R"({"a":"main","b":"main","end_beat":1})"),
                    obj(R"({"a":"<any>","b":"<any>","comparison":"<any>"})"), "song"}};
    ab.run = [](const AbArgs& a, Context& ctx) -> Result<json::Value> {
        auto& app = ctx.service<EngineApp>();
        Args opts;
        opts.startBeat = a.startBeat;
        opts.endBeat = a.endBeat;
        opts.tailS = a.tailS;
        opts.pattern = "ab-a";
        opts.branch = a.a;
        auto first = plan(app, "render.mix", opts);
        if (!first) return std::move(first).error();
        auto ra = runTasks(app, *first, opts);
        if (!ra) return std::move(ra).error();
        opts.pattern = "ab-b";
        opts.branch = a.b;
        auto second = plan(app, "render.mix", opts);
        if (!second) return std::move(second).error();
        auto rb = runTasks(app, *second, opts);
        if (!rb) return std::move(rb).error();
        auto left = audio::readAudio((*first)[0].path);
        if (!left) return std::move(left).error();
        auto right = audio::readAudio((*second)[0].path);
        if (!right) return std::move(right).error();
        analysis_commands::Loaded la, lb;
        la.data = std::move(*left);
        lb.data = std::move(*right);
        auto ar = analysis_commands::analyze(la), br = analysis_commands::analyze(lb);
        if (!ar) return std::move(ar).error();
        if (!br) return std::move(br).error();
        const auto comparison = analysis_commands::compare(*ar, *br);
        json::Value out = json::Value::object();
        out["a"] = ra->find("files")->at(0).find("render")->asString();
        out["b"] = rb->find("files")->at(0).find("render")->asString();
        json::Value report = json::Value::object();
        report["notable"] = comparison.notable;
        report["ltas_rms_db"] = comparison.ltasRmsDb;
        report["summary"] = comparison.summary;
        out["comparison"] = std::move(report);
        return out;
    };
    registry.add(std::move(ab));
}
}  // namespace supple::engine
