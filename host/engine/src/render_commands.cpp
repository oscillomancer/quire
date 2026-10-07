#include "render_commands.h"
#include "supple/model/numeric.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <limits>
#include <set>
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
    std::optional<std::string> branch, pattern, name, mode, tap, format, operation, master;
    std::optional<json::Value> normalize, secondary;
    std::optional<std::vector<std::string>> tracks, regions;
    std::optional<double> startBeat, endBeat, tailS, target, limitTruePeakDb;
    std::optional<std::int64_t> bitDepth;
    std::optional<std::int64_t> sampleRate, channels, version;
    bool job = false;
    bool addToProject = false;
};
struct AbArgs {
    std::string a, b;
    std::optional<double> startBeat, endBeat, tailS;
};
using PatternData = RenderPatternContext;
struct RenderTask {
    model::Session session;
    std::shared_ptr<const model::Session> pin;
    OfflineSettings settings;
    fs::path path;
    std::string label;
};

json::Value obj(std::string_view s) { return json::parse(s).value(); }
std::string pad(std::int64_t n, int width) {
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
            if (!model::checkedSamples(d.startSeconds, 1, "/start_beat") || d.startSeconds < 0)
                return model::numericError("/start_beat");
            const auto sec = static_cast<std::int64_t>(d.startSeconds);
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
// Pattern arguments are literal quoted strings. Delimiters inside them do
// not end a token or start another filter; the language has no quote escapes.
std::size_t unquotedDelimiter(std::string_view text, char delimiter, std::size_t from = 0) {
    bool quoted = false;
    for (std::size_t i = from; i < text.size(); ++i) {
        if (text[i] == '\"')
            quoted = !quoted;
        else if (!quoted && text[i] == delimiter)
            return i;
    }
    return std::string_view::npos;
}

Result<std::string> expand(std::string_view pattern, const PatternData& d) {
    std::string out;
    for (std::size_t i = 0; i < pattern.size();) {
        if (pattern[i] != '{') {
            if (pattern[i] == '}')
                return Error("render.pattern", "unmatched }", "/pattern",
                             "remove the unmatched } from pattern or include it inside a quoted "
                             "replace argument");
            out += pattern[i++];
            continue;
        }
        auto end = unquotedDelimiter(pattern, '}', i + 1);
        if (end == std::string_view::npos)
            return Error("render.pattern", "unclosed {", "/pattern",
                         "close each token with } and each replace argument with a double quote");
        std::string inside(pattern.substr(i + 1, end - i - 1));
        auto pipe = unquotedDelimiter(inside, '|');
        auto value = token(std::string_view(inside).substr(0, pipe), d);
        if (!value) return std::move(value).error();
        while (pipe != std::string::npos) {
            auto next = unquotedDelimiter(inside, '|', pipe + 1);
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
                    return Error("render.pattern", "invalid max filter", "/pattern",
                                 "use max with a nonnegative integer, for example {track|max(8)}");
                value->resize(std::min(value->size(), maximum));
            } else if (filter.starts_with("replace(\"") && filter.ends_with("\")")) {
                const auto mid = filter.find('"', 9);
                if (mid == std::string::npos || filter.substr(mid, 3) != "\",\"" ||
                    filter.find('"', mid + 3) != filter.size() - 2)
                    return Error(
                        "render.pattern", "invalid replace filter", "/pattern",
                        "use replace(\"from\",\"to\") with two balanced literal quoted strings");
                const std::string from = filter.substr(9, mid - 9),
                                  to = filter.substr(mid + 3, filter.size() - mid - 5);
                if (from.empty())
                    return Error("render.pattern", "empty replace source", "/pattern",
                                 "provide a nonempty first argument to replace, for example "
                                 "replace(\" \",\"_\")");
                for (std::size_t at = 0; (at = value->find(from, at)) != std::string::npos;
                     at += to.size())
                    value->replace(at, from.size(), to);
            } else
                return Error("render.pattern", "unknown filter " + filter, "/pattern",
                             "use balanced token braces and literal quoted replace arguments with "
                             "the filters lower, upper, max or replace");
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
    if (out.find('\0') != std::string::npos)
        return Error("render.path", "render name contains a NUL byte", "/pattern",
                     "remove NUL bytes from the pattern and its token values");
    if (out.empty() || out == "." || out == ".." || fs::path(out).is_absolute() ||
        out.find("..") != std::string::npos || out.find('\\') != std::string::npos)
        return Error("render.path", "render name escapes the renders directory", "/pattern",
                     "use a relative name without ..");
    return out;
}

Result<model::Session> sessionFor(EngineApp& app, const std::optional<std::string>& branch,
                                  std::optional<std::int64_t> version = std::nullopt) {
    if (version) {
        if (branch)
            return Error("render.source", "give branch or version, not both", "/version",
                         "choose either branch or version and omit the other selector");
        const model::Version* v = app.versions().version(static_cast<std::uint64_t>(*version));
        if (!v || !v->session)
            return Error("history.version", "no version " + std::to_string(*version), "/version",
                         "history.list shows the versions");
        return *v->session;
    }
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

double lastBeat(const model::Session& s) { return model::lastItemBeat(s); }
Result<std::vector<RenderTask>> plan(EngineApp& app, std::string_view command, const Args& a) {
    auto source = sessionFor(app, a.branch, a.version);
    if (!source) return std::move(source).error();
    model::Session s = *source;
    const json::Value& defaults = s.project->render;
    auto setting = [&](std::string_view key) -> const json::Value* {
        return defaults.isObject() ? defaults.find(key) : nullptr;
    };
    auto encoding = renderEncoding(*s.project, a.format, a.bitDepth);
    if (!encoding) return std::move(encoding).error();
    auto secondary = secondaryEncoding(*s.project, a.secondary);
    if (!secondary) return std::move(secondary).error();
    const std::string formatName = encoding->container;
    std::string pattern = a.name.value_or(a.pattern.value_or(
        setting("pattern") && setting("pattern")->isString() ? setting("pattern")->asString()
        : command == "render.mix"                            ? "mix-v" + std::to_string(s.version)
                                                             : "{project}-{track}-{region}"));
    std::string mode = a.mode.value_or("isolated");
    if (mode != "isolated" && mode != "through_master")
        return Error("render.mode", "unknown stem mode", "/mode", "use isolated or through_master");
    std::string tap = a.tap.value_or("post_fader");
    if (tap != "post_fader" && tap != "pre_fader" && tap != "pre_fx")
        return Error("render.tap", "unknown tap", "/tap", "use pre_fx, pre_fader or post_fader");
    std::vector<RenderTask> tasks;
    std::set<fs::path> destinations;
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
        task.pin = app.versions().pin(s.version);
        task.settings.startSeconds = s.timeMap->secondsAt(first);
        task.settings.endSeconds = s.timeMap->secondsAt(last);
        task.settings.projectDir = app.projectDir();
        task.settings.tailSeconds =
            a.tailS.value_or(setting("tail_ms") && setting("tail_ms")->isNumber()
                                 ? setting("tail_ms")->asDouble() / 1000.0
                                 : 0.0);
        SUPPLE_TRY(checkRenderRange(s, task.settings, a.sampleRate));
        if (task.settings.tailSeconds < 0 || task.settings.tailSeconds > 600)
            return Error("render.tail", "invalid tail", "/tail_s",
                         "provide tail_s as a finite number of seconds from 0 to 600");
        if (!track && a.master) {
            auto id = trackId(s, *a.master);
            if (!id) return std::move(id).error();
            if (s.tracks.find(*id)->type != model::StripType::Master)
                return Error("track.strip_type", "mix output must be a master", "/master",
                             "choose a master id from track.list");
            task.settings.master = *id;
        }
        if (track && s.tracks.find(*track)->type == model::StripType::Master) {
            task.settings.master = *track;
        } else if (track) {
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
            if ((!track || item.value->track == *track) &&
                model::itemSpan(s, *item.value).startBeat <= first &&
                model::itemEndBeat(s, *item.value) > first) {
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
        data.bitDepth = encoding->bits();
        data.format = formatName;
        data.counter = static_cast<int>(tasks.size() + 1);
        auto name = expand(pattern, data);
        if (!name) return std::move(name).error();
        fs::path rel = fs::path(*name + encoding->extension()).lexically_normal();
        if (rel.is_absolute() || rel.empty() || *rel.begin() == "..")
            return Error("render.path", "render name escapes directory", "/pattern",
                         "use a relative pattern beneath a readable and writable renders "
                         "directory; remove traversal components or symlinks that escape it");
        task.path = app.rendersDir() / rel;
        std::error_code ec;
        const auto root = fs::weakly_canonical(app.rendersDir(), ec);
        if (ec)
            return Error("render.path", "cannot resolve renders directory", "/pattern",
                         "use a relative pattern beneath a readable and writable renders "
                         "directory; remove traversal components or symlinks that escape it");
        // The final path, not only its parent: an entry that already exists at
        // this name (a symlink pointing out of the renders directory) resolves
        // to its target here, so the check cannot be walked out of. The write
        // itself is a temp file plus an atomic rename (audio::writeWav,
        // writeFileAtomic), which replaces such an entry rather than following
        // it, and a temp name is never a directory.
        const auto resolved = fs::weakly_canonical(task.path, ec);
        if (ec)
            return Error("render.path", "cannot resolve the render path", "/pattern",
                         "use a relative pattern beneath a readable and writable renders "
                         "directory; remove traversal components or symlinks that escape it");
        const auto relative = resolved.lexically_relative(root);
        if (relative.empty() || *relative.begin() == ".." || relative.is_absolute())
            return Error("render.path", "render name escapes the renders directory", "/pattern",
                         "use a relative pattern beneath a readable and writable renders "
                         "directory; remove traversal components or symlinks that escape it");
        task.label = *name;
        const auto second =
            *secondary ? secondaryPath(task.path, *encoding, **secondary) : fs::path{};
        // Atomic writes replace the final entry; only parent-directory
        // symlinks change their destination. Compare that parent identity plus
        // the final name, for both encodings, without following a final symlink.
        for (const auto& path : {task.path, second}) {
            if (path.empty()) continue;
            const auto parent = fs::weakly_canonical(path.parent_path(), ec);
            if (ec)
                return Error("render.path", "cannot resolve the render parent", "/pattern",
                             "choose a pattern under a readable renders directory");
            if (!destinations.insert(parent / path.filename()).second)
                return Error("render.path", "two renders have the same destination", "/pattern",
                             "include track, region or counter in a pattern that gives each file a "
                             "unique destination");
        }
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
        if (ids.empty())
            return Error(
                "render.empty", "no stem tracks", "/tracks",
                "add a channel or select existing track ids with tracks before rendering stems");
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
            return Error("render.empty", "no regions", "/regions",
                         "add a region with region.add or select existing ids from region.list");
        if (projectRow && projectRow->isArray() && !a.regions) {
            for (const auto& cell : projectRow->asArray()) {
                if (!cell.isString())
                    return Error("render.matrix", "matrix entries must be track or master ids",
                                 "/matrix",
                                 "provide matrix as an object whose region/project rows are arrays "
                                 "of existing track or master ids from track.list");
                {
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
                        return Error("render.matrix", "matrix entries must be track or master ids",
                                     "/matrix",
                                     "provide matrix as an object whose region/project rows are "
                                     "arrays of existing track or master ids from track.list");
                    {
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
        if (jobs && jobs->cancelled(job))
            return Error("job.cancelled", "render cancelled", {},
                         "submit a new render job if its output is still wanted; the cancelled job "
                         "cannot resume");
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
        RenderOutputArgs output{args.format,   args.normalize,  args.target,   args.limitTruePeakDb,
                                args.bitDepth, args.sampleRate, args.channels, args.secondary};
        auto encoded = finishRenderAudio(audio, *task.session.project, output);
        if (!encoded) return std::move(encoded).error();
        if (jobs && jobs->cancelled(job))
            return Error("job.cancelled", "render cancelled", {},
                         "submit a new render job if its output is still wanted; the cancelled job "
                         "cannot resume");
        SUPPLE_TRY(writeRender(task.path, audio, encoded->primary));
        json::Value entry = json::Value::object();
        entry["path"] = task.path.string();
        if (encoded->secondary) {
            const fs::path second = secondaryPath(task.path, encoded->primary, *encoded->secondary);
            SUPPLE_TRY(writeRender(second, audio, *encoded->secondary));
            entry["secondary_path"] = second.string();
        }
        entry["render"] = app.rememberRender(task.path);
        entry["start_beat"] = task.session.timeMap->beatAt(task.settings.startSeconds);
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
        .field("master", &Args::master, schema::string().pattern("^tr_[0-9]+$", "master id"))
        .field("branch", &Args::branch, schema::string())
        .field("pattern", &Args::pattern,
               schema::string().doc("File name pattern (session-model §7.3), e.g. "
                                    "\"{project}-{track_number:02}-{track|lower}\""))
        .field("name", &Args::name, schema::string().doc("A fixed file name instead of a pattern"))
        .field("mode", &Args::mode,
               schema::string().doc("Stems: isolated (default; the track alone, no master "
                                    "processing) or through_master (the others muted)"))
        .field("tap", &Args::tap,
               schema::string().doc("Stems: post_fader (default), pre_fader or pre_fx"))
        .field("format", &Args::format, schema::string().doc("wav (default) or flac (16/24 bit)"))
        .field("secondary", &Args::secondary,
               schema::any().doc("A second file of the same render beside the first: "
                                 "{format: wav|flac, bit_depth}, or \"none\"; default: the "
                                 "project's render settings"))
        .field("normalize", &Args::normalize,
               schema::any().doc("none, lufs_i, lufs_s, lufs_m, peak, true_peak or rms; or "
                                 "{mode, target, limit_true_peak_db}"))
        .field("operation", &Args::operation,
               schema::string().doc("Dry run: mix, stems or regions (default from the other "
                                    "arguments)"))
        .field("tracks", &Args::tracks,
               schema::arrayOf(schema::string())
                   .doc("Stems: track ids (default: every "
                        "top-level track)"))
        .field("regions", &Args::regions,
               schema::arrayOf(schema::string())
                   .doc("Regions: region ids (default: all, with "
                        "the project render matrix)"))
        .field("start_beat", &Args::startBeat, schema::number().min(0))
        .field("end_beat", &Args::endBeat, schema::number().exclusiveMin(0))
        .field("tail_s", &Args::tailS, schema::number().range(0, 600))
        .field("target", &Args::target, schema::number())
        .field("limit_true_peak_db", &Args::limitTruePeakDb, schema::number())
        .field("bit_depth", &Args::bitDepth, schema::integer().range(16, 32))
        .field("sample_rate", &Args::sampleRate, schema::integer().range(8000, 384000))
        .field("channels", &Args::channels, schema::integer().range(1, 2))
        .field("version", &Args::version,
               schema::integer().min(0).doc("Render the session as of this version "
                                            "(history.list); the render queue pins its renders"))
        .field("job", &Args::job, schema::boolean())
        .defaultValue(false)
        .field("add_to_project", &Args::addToProject, schema::boolean())
        .defaultValue(false)
        .build();
}
}  // namespace

Status checkRenderRange(const model::Session& s, const OfflineSettings& settings,
                        std::optional<std::int64_t> sampleRate) {
    SUPPLE_TRY(model::checkedSeconds(s, settings.startSeconds, "/start_beat"));
    SUPPLE_TRY(model::checkedSeconds(s, settings.endSeconds, "/end_beat"));
    if (!std::isfinite(settings.tailSeconds) || settings.tailSeconds < 0 ||
        settings.tailSeconds > 600)
        return model::numericError("/tail_s");
    SUPPLE_TRY(model::checkedSeconds(s, settings.endSeconds + settings.tailSeconds, "/tail_s"));
    const auto* rateDefault = s.project->render.find("sample_rate");
    const double rate = sampleRate    ? static_cast<double>(*sampleRate)
                        : rateDefault ? rateDefault->asDouble()
                                      : s.project->sampleRate;
    if (!std::isfinite(rate) || rate < 8000 || rate > 384000)
        return model::numericError("/sample_rate");
    SUPPLE_TRY(model::checkedSeconds(s, settings.endSeconds + settings.tailSeconds, "/end_beat",
                                     std::max(rate, s.project->sampleRate)));
    const long double duration = static_cast<long double>(settings.endSeconds) -
                                 static_cast<long double>(settings.startSeconds) +
                                 static_cast<long double>(settings.tailSeconds);
    if (!(settings.endSeconds > settings.startSeconds) || !(duration > 0))
        return Error("render.empty", "the end must come after the start", "/end_beat",
                     "set end_beat later than start_beat");
    // Match the engine's existing in-memory output/variant capacity, including resampling.
    const long double frames =
        std::ceil(duration * static_cast<long double>(std::max(rate, s.project->sampleRate)));
    if (!std::isfinite(frames) || frames > (std::int64_t{1} << 28) - 8192)
        return Error("render.budget", "render exceeds the in-memory frame budget", "/end_beat",
                     "render a shorter range or use a lower sample rate");
    return {};
}

Result<OfflineResult> renderSnapshot(const model::Session& session, OfflineSettings settings,
                                     const std::vector<host::PluginInfo>& catalogue,
                                     const host::HostIdentity& identity) {
    SUPPLE_TRY(checkRenderRange(session, settings));
    InstancePool pool(
        [&catalogue](const model::PluginRef& ref) -> Result<host::PluginInfo> {
            for (const auto& plugin : catalogue)
                if (plugin.id == ref.id) return plugin;
            return Error(
                "plugin.unknown", "plugin unavailable in the render snapshot: " + ref.id, {},
                "select an installed plugin from plugin.list before queuing the render snapshot");
        },
        identity);
    return renderOffline(session, pool, settings);
}

Status normalizeRenderAudio(audio::AudioData& data, std::string_view mode, double target,
                            std::optional<double> limitTruePeakDb) {
    if (mode == "none") return okStatus();
    for (const auto& channel : data.channels)
        for (float sample : channel)
            if (!std::isfinite(sample))
                return Error(
                    "render.normalize", "source audio contains non-finite samples", "/normalize",
                    "bypass or replace the FX producing non-finite audio before normalizing");
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
        return Error("render.normalize", "unsupported normalization mode", "/normalize",
                     "use none, lufs_i, lufs_s, lufs_m, peak, true_peak or rms as normalize.mode");
    if (std::isfinite(measured)) {
        const double gain = std::pow(10.0, (target - measured) / 20.0);
        if (!std::isfinite(gain))
            return Error("render.normalize", "normalization gain is not finite",
                         "/normalize/target", "reduce the target so its gain is finite");
        // Check the complete result before mutating any channel. A finite
        // double gain can still overflow the float buffers used by encoders.
        for (const auto& channel : data.channels)
            for (float sample : channel) {
                const double scaled = static_cast<double>(sample) * gain;
                if (!std::isfinite(scaled) ||
                    std::abs(scaled) > static_cast<double>(std::numeric_limits<float>::max()))
                    return Error("render.normalize",
                                 "normalization would produce non-finite samples",
                                 "/normalize/target",
                                 "reduce the target to keep samples finite; bypass any FX "
                                 "producing non-finite audio");
            }
        for (auto& channel : data.channels)
            for (float& sample : channel)
                sample = static_cast<float>(static_cast<double>(sample) * gain);
    }
    if (limitTruePeakDb) {
        const double ceiling = std::pow(10.0, *limitTruePeakDb / 20.0);
        if (!(ceiling > 0 && ceiling <= 1))
            return Error("render.normalize", "true peak limit must be at or below 0 dBTP",
                         "/normalize/limit_true_peak_db",
                         "provide a finite limit_true_peak_db at or below 0 whose linear ceiling "
                         "remains greater than zero");
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

Result<RenderOutput> finishRenderAudio(audio::AudioData& audio, const model::Project& project,
                                       const RenderOutputArgs& a) {
    const json::Value& defaults = project.render;
    auto setting = [&](std::string_view key) -> const json::Value* {
        return defaults.isObject() ? defaults.find(key) : nullptr;
    };
    const auto* sampleRate = setting("sample_rate");
    const double targetRate = a.sampleRate ? static_cast<double>(*a.sampleRate)
                              : sampleRate && sampleRate->isNumber() ? sampleRate->asDouble()
                                                                     : audio.sampleRate;
    if (!(targetRate >= 8000 && targetRate <= 384000))
        return Error("render.sample_rate", "sample rate out of range", "/sample_rate",
                     "provide sample_rate as a finite value from 8000 to 384000 Hz");
    if (targetRate != audio.sampleRate) {
        auto converted = audio::resample(audio, targetRate);
        if (!converted) return std::move(converted).error();
        audio = std::move(*converted);
    }
    const auto* channelDefault = setting("channels");
    const int channels = static_cast<int>(a.channels.value_or(
        channelDefault && channelDefault->isNumber() ? channelDefault->asInt() : 2));
    if (channels != 1 && channels != 2)
        return Error("render.channels", "channels must be 1 or 2", "/channels",
                     "set channels to 1 for mono or 2 for stereo");
    if (channels == 1 && audio.channelCount() == 2) {
        for (std::size_t n = 0; n < audio.frames(); ++n)
            audio.channels[0][n] =
                static_cast<float>(0.5 * (static_cast<double>(audio.channels[0][n]) +
                                          static_cast<double>(audio.channels[1][n])));
        audio.channels.resize(1);
    }
    const json::Value* norm = a.normalize ? &*a.normalize : setting("normalize");
    std::string normMode = "none";
    double normTarget = a.target.value_or(-14.0);
    std::optional<double> limit = a.limitTruePeakDb;
    if (norm) {
        if (norm->isString())
            normMode = norm->asString();
        else if (norm->isObject()) {
            const auto* mode = norm->find("mode");
            if (!mode || !mode->isString())
                return Error(
                    "render.normalize", "normalize needs a mode", "/normalize/mode",
                    "provide normalize.mode, for example {\"mode\":\"peak\",\"target\":-1}");
            normMode = mode->asString();
            if (const auto* target = norm->find("target")) {
                if (!target->isNumber())
                    return Error("render.normalize", "target must be a number", "/normalize/target",
                                 "provide a numeric target in dB or LUFS");
                normTarget = target->asDouble();
            }
            if (const auto* peak = norm->find("limit_true_peak_db")) {
                if (!peak->isNumber())
                    return Error(
                        "render.normalize", "peak limit must be a number",
                        "/normalize/limit_true_peak_db",
                        "provide limit_true_peak_db as a numeric ceiling at or below 0 dBTP");
                limit = peak->asDouble();
            }
        } else
            return Error("render.normalize", "normalize must be a mode or object", "/normalize",
                         "provide normalize as a supported mode string or {mode, target, "
                         "limit_true_peak_db}");
    }
    if (auto normalized = normalizeRenderAudio(audio, normMode, normTarget, limit); !normalized) {
        auto error = normalized.error();
        if (error.path == "/normalize/target" && a.target &&
            (!norm || !norm->isObject() || !norm->find("target")))
            error.path = "/target";
        if (error.path == "/normalize/limit_true_peak_db" && a.limitTruePeakDb &&
            (!norm || !norm->isObject() || !norm->find("limit_true_peak_db")))
            error.path = "/limit_true_peak_db";
        return error;
    }
    RenderOutput out;
    auto primary = renderEncoding(project, a.format, a.bitDepth);
    if (!primary) return std::move(primary).error();
    out.primary = std::move(*primary);
    auto second = secondaryEncoding(project, a.secondary);
    if (!second) return std::move(second).error();
    out.secondary = std::move(*second);
    return out;
}

int RenderEncoding::bits() const {
    return wav.format == audio::SampleFormat::Pcm16   ? 16
           : wav.format == audio::SampleFormat::Pcm24 ? 24
                                                      : 32;
}

std::string RenderEncoding::extension() const { return container == "flac" ? ".flac" : ".wav"; }

namespace {
Result<RenderEncoding> encodingOf(const std::string& container, std::int64_t depth,
                                  const json::Value* formatDefault, std::uint64_t seed,
                                  const std::string& at) {
    if (container != "wav" && container != "flac")
        return Error("render.format", "unknown format \"" + container + "\"", at + "/format",
                     "wav or flac");
    if (container == "flac" && depth != 16 && depth != 24)
        return Error("render.format", "FLAC is 16 or 24 bit", at + "/bit_depth",
                     "use 16 or 24 (32-bit float is WAV only)");
    if (depth != 16 && depth != 24 && depth != 32)
        return Error("render.format", "bit_depth must be 16, 24, or 32", at + "/bit_depth",
                     "provide bit_depth 16 or 24 for WAV/FLAC, or 32 for float WAV");
    RenderEncoding e;
    e.container = container;
    e.wav.format = depth == 16   ? audio::SampleFormat::Pcm16
                   : depth == 24 ? audio::SampleFormat::Pcm24
                                 : audio::SampleFormat::Float32;
    e.wav.ditherSeed = seed;
    if (formatDefault && formatDefault->isObject())
        if (const auto* dither = formatDefault->find("dither"); dither && dither->isBool())
            e.wav.dither = dither->asBool();
    return e;
}
}  // namespace

Result<RenderEncoding> renderEncoding(const model::Project& project,
                                      const std::optional<std::string>& format,
                                      std::optional<std::int64_t> bitDepth) {
    const json::Value& defaults = project.render;
    auto setting = [&](std::string_view key) -> const json::Value* {
        return defaults.isObject() ? defaults.find(key) : nullptr;
    };
    const auto* formatDefault = setting("format");
    const std::string container = format.value_or(
        formatDefault && formatDefault->isString() ? formatDefault->asString()
        : formatDefault && formatDefault->isObject() && formatDefault->find("container") &&
                formatDefault->find("container")->isString()
            ? formatDefault->find("container")->asString()
            : "wav");
    const json::Value* depthDefault = formatDefault && formatDefault->isObject()
                                          ? formatDefault->find("bit_depth")
                                          : setting("bit_depth");
    const std::int64_t depth =
        bitDepth.value_or(depthDefault && depthDefault->isNumber() ? depthDefault->asInt()
                          : container == "flac"                    ? 24
                                                                   : 32);
    return encodingOf(container, depth, formatDefault, project.seed, "");
}

Result<std::optional<RenderEncoding>> secondaryEncoding(
    const model::Project& project, const std::optional<json::Value>& secondary) {
    const json::Value* spec = secondary ? &*secondary : nullptr;
    if (!spec && project.render.isObject()) spec = project.render.find("secondary");
    if (!spec || spec->isNull() || (spec->isString() && spec->asString() == "none"))
        return std::optional<RenderEncoding>{};
    if (!spec->isObject())
        return Error("render.secondary", "secondary must be {format, bit_depth} or \"none\"",
                     "/secondary", "e.g. {\"format\": \"flac\", \"bit_depth\": 16}");
    const auto* f = spec->find("format");
    if (!f || !f->isString())
        return Error("render.secondary", "secondary needs a format", "/secondary/format",
                     "wav or flac");
    const auto* d = spec->find("bit_depth");
    if (d && !d->isNumber())
        return Error("render.secondary", "bit_depth must be a number", "/secondary/bit_depth",
                     "provide secondary.bit_depth as an integer: 16 or 24, or 32 for WAV only");
    const std::int64_t depth = d ? d->asInt() : f->asString() == "flac" ? 24 : 32;
    auto e = encodingOf(f->asString(), depth, nullptr, project.seed, "/secondary");
    if (!e) return std::move(e).error();
    return std::optional<RenderEncoding>(std::move(*e));
}

std::filesystem::path secondaryPath(const std::filesystem::path& primary,
                                    const RenderEncoding& first, const RenderEncoding& second) {
    std::filesystem::path p = primary;
    if (first.container == second.container)
        p.replace_extension("." + std::to_string(second.bits()) + second.extension());
    else
        p.replace_extension(second.extension());
    return p;
}

Status writeRender(const std::filesystem::path& path, const audio::AudioData& data,
                   const RenderEncoding& encoding) {
    if (encoding.container == "flac") {
        audio::FlacOptions flac;
        flac.bitsPerSample = encoding.bits();
        flac.dither = encoding.wav.dither;
        flac.ditherSeed = encoding.wav.ditherSeed;
        return audio::writeFlac(path, data, flac);
    }
    return audio::writeWav(path, data, encoding.wav);
}

Result<std::string> expandRenderPattern(std::string_view pattern,
                                        const RenderPatternContext& context) {
    return expand(pattern, context);
}

RenderTarget renderTarget(const EngineApp& app) {
    return {app.projectGeneration(), app.versions().branch()};
}
Status checkRenderTarget(const EngineApp& app, const RenderTarget& target) {
    if (app.projectGeneration() != target.generation || app.versions().branch() != target.branch)
        return Error("render.stale_target", "the render target project or branch changed", {},
                     "files remain on disk; import them into the intended project manually");
    return {};
}
void completeRenderJob(EngineApp& app, cmd::Dispatcher& dispatcher, cmd::JobManager& jobs,
                       std::string id, RenderTarget target, json::Value result,
                       std::function<void()> publish) {
    dispatcher.post("render.complete",
                    [&app, &jobs, id = std::move(id), target = std::move(target),
                     result = std::move(result), publish = std::move(publish)]() mutable -> Status {
                        if (auto valid = checkRenderTarget(app, target); !valid) {
                            json::Value error = json::Value::object();
                            error["code"] = valid.error().code;
                            error["message"] = valid.error().message;
                            error["path"] = valid.error().path;
                            error["hint"] = valid.error().hint;
                            result["status"] = "render.stale_target";
                            result["error"] = std::move(error);
                            app.emit("render.stale_target", result);
                            // Project replacement may already have cancelled the job.
                            if (jobs.cancelled(id)) return jobs.cancel(id, std::move(result));
                            return jobs.complete(id, std::move(result));
                        }
                        if (jobs.cancelled(id)) return {};
                        return jobs.completeWith(id, std::move(result), publish);
                    });
}

void postAddToProject(EngineApp& app, RenderTarget target, cmd::Dispatcher& dispatcher,
                      const cmd::Origin& origin, const json::Value& files) {
    std::vector<std::pair<std::string, double>> add;  // path, start beat
    if (files.isArray())
        for (const json::Value& f : files.asArray())
            if (const auto* path = f.find("path"); path && path->isString()) {
                const auto* start = f.find("start_beat");
                add.emplace_back(path->asString(),
                                 start && start->isNumber() ? start->asDouble() : 0.0);
            }
    if (add.empty()) return;
    dispatcher.post(
        "render.add_to_project",
        [&app, target = std::move(target), &dispatcher, origin, add]() -> Status {
            SUPPLE_TRY(checkRenderTarget(app, target));
            auto done =
                dispatcher.transact("Add renders to project", origin, [&]() -> Result<json::Value> {
                    for (const auto& [path, start] : add) {
                        json::Value track = json::Value::object();
                        track["name"] = fs::path(path).stem().string();
                        auto t = dispatcher.execute({"track.add", std::move(track), origin});
                        if (!t) return t;
                        json::Value item = json::Value::object();
                        item["track"] = (*t)["track"];
                        item["position"] = start;
                        item["audio_file"] = path;
                        if (auto i = dispatcher.execute({"item.add", std::move(item), origin}); !i)
                            return i;
                    }
                    return json::Value::object();
                });
            if (!done) return std::move(done).error();
            return {};
        },
        cmd::Dispatcher::PostKind::Edit);
}

namespace {

struct QueueAddArgs {
    std::string command;
    json::Value args = json::Value::object();
    std::optional<std::string> label;
};
struct QueueRemoveArgs {
    std::optional<std::string> id;
    bool all = false;
};
struct QueueRunArgs {
    bool job = false;
};

json::Value queueEntry(const EngineApp::QueuedRender& q) {
    json::Value e = json::Value::object();
    e["id"] = q.id;
    e["command"] = q.command;
    e["label"] = q.label;
    e["version"] = static_cast<std::int64_t>(q.version);
    e["files"] = q.files;
    e["args"] = q.args;
    return e;
}

// One queued render ready to run: its stored arguments decoded and planned.
struct QueuedWork {
    std::string id, label;
    Args args;
    std::vector<RenderTask> tasks;
};

Result<QueuedWork> planQueued(EngineApp& app, const EngineApp::QueuedRender& q) {
    auto decoded = argsSchema().decode(q.args);
    if (!decoded) return std::move(decoded).error();
    QueuedWork w{q.id, q.label, std::move(*decoded), {}};
    w.args.job = false;
    auto tasks = plan(app, q.command, w.args);
    if (!tasks) return std::move(tasks).error();
    w.tasks = std::move(*tasks);
    return w;
}

Error queueArgsError(Error error) {
    auto prefix = [](auto&& self, Error& e) -> void {
        e.path = "/args/args" + e.path;
        for (auto& detail : e.details) self(self, detail);
    };
    prefix(prefix, error);
    return error;
}

void registerQueueCommands(cmd::Registry& registry) {
    {
        Command<QueueAddArgs, json::Value> c;
        c.name = "render.queue.add";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Queue a render (mix, stems or regions) to run later with render.queue.run.";
        c.description =
            "args: the render command's arguments (as for render.mix/stems/regions). The queued "
            "render is pinned to the session as it is now (its version; a branch's head when "
            "branch is given), so later edits do not change it; a dry run lists its files now. "
            "The queue is transport-like state: not an undo step, not saved with the project.";
        c.tags = {"render"};
        c.args = schema::object<QueueAddArgs>("render_queue_add_args")
                     .field("command", &QueueAddArgs::command,
                            schema::string().pattern("^render\\.(mix|stems|regions)$",
                                                     "render.mix, render.stems or render.regions"))
                     .field("args", &QueueAddArgs::args, schema::any())
                     .field("label", &QueueAddArgs::label, schema::string())
                     .build();
        c.result = schema::any();
        c.errors = {{"render.empty", "the render would write nothing", "add items or a range"}};
        c.examples = {{"Queue the mix as it is now",
                       obj(R"({"command":"render.mix","args":{"end_beat":1,"name":"queued"}})"),
                       obj(R"({"id":"q1","command":"render.mix","label":"<any>","version":"<any>",
                               "files":"<any>","args":"<any>"})"),
                       "song"}};
        c.run = [](const QueueAddArgs& a, Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            auto decoded = argsSchema().decode(a.args);
            if (!decoded) return queueArgsError(std::move(decoded).error());
            Args args = std::move(*decoded);
            args.job = false;
            if (!args.version) {  // pin it: later edits must not change what renders
                auto source = sessionFor(app, args.branch);
                if (!source) return queueArgsError(std::move(source).error());
                args.version = static_cast<std::int64_t>(source->version);
                args.branch.reset();
            }
            auto tasks = plan(app, a.command, args);
            if (!tasks) return queueArgsError(std::move(tasks).error());
            EngineApp::QueuedRender q;
            q.id = app.nextQueueId();
            q.command = a.command;
            q.version = static_cast<std::uint64_t>(*args.version);
            q.pin = app.versions().pin(q.version);
            q.args = argsSchema().encode(args);
            q.files = json::Value::array();
            const json::Value planned = fileList(*tasks);  // (kept alive for the loop)
            for (const auto& f : planned.asArray()) q.files.push_back(f.find("path")->asString());
            const std::string what = a.command.substr(std::string_view("render.").size());
            q.label = a.label.value_or(what + " @ v" + std::to_string(q.version) + " (" +
                                       std::to_string(tasks->size()) +
                                       (tasks->size() == 1 ? " file)" : " files)"));
            json::Value out = queueEntry(q);
            app.renderQueue().push_back(std::move(q));
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<cmd::NoArgs, json::Value> c;
        c.name = "render.queue.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "The queued renders, in the order render.queue.run runs them.";
        c.tags = {"render"};
        c.args = schema::object<cmd::NoArgs>("no_args").build();
        c.result = schema::any();
        c.examples = {{"Empty", obj("{}"), obj(R"({"queue":[]})"), "empty"}};
        c.run = [](const cmd::NoArgs&, Context& ctx) -> Result<json::Value> {
            json::Value list = json::Value::array();
            for (const auto& q : ctx.service<EngineApp>().renderQueue())
                list.push_back(queueEntry(q));
            json::Value out = json::Value::object();
            out["queue"] = std::move(list);
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<QueueRemoveArgs, json::Value> c;
        c.name = "render.queue.remove";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Take a render off the queue (id), or clear it (all: true).";
        c.tags = {"render"};
        c.args = schema::object<QueueRemoveArgs>("render_queue_remove_args")
                     .field("id", &QueueRemoveArgs::id, schema::string())
                     .field("all", &QueueRemoveArgs::all, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::any();
        c.errors = {{"render.queue_unknown", "no queued render with that id",
                     "render.queue.list shows them"}};
        c.examples = {{"Clear", obj(R"({"all":true})"), obj(R"({"removed":0})"), "empty"}};
        c.run = [](const QueueRemoveArgs& a, Context& ctx) -> Result<json::Value> {
            auto& queue = ctx.service<EngineApp>().renderQueue();
            json::Value out = json::Value::object();
            if (a.all) {
                out["removed"] = static_cast<std::int64_t>(queue.size());
                queue.clear();
                return out;
            }
            if (!a.id)
                return Error("render.queue_unknown", "give id or all", "/id",
                             "render.queue.list shows the ids");
            const auto before = queue.size();
            std::erase_if(queue, [&](const auto& q) { return q.id == *a.id; });
            if (queue.size() == before)
                return Error("render.queue_unknown", "no queued render " + *a.id, "/id",
                             "render.queue.list shows the ids");
            out["removed"] = 1;
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<QueueRunArgs, json::Value> c;
        c.name = "render.queue.run";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Run every queued render in order; each one leaves the queue when it is done.";
        c.description =
            "Each render renders the version it was queued at. job: true runs them as one job "
            "(job.get for progress). The result lists each render's files under its id.";
        c.tags = {"render"};
        c.args = schema::object<QueueRunArgs>("render_queue_run_args")
                     .field("job", &QueueRunArgs::job, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::any();
        c.errors = {{"render.queue_empty", "nothing is queued", "render.queue.add first"}};
        c.examples = {
            {"Render the queue", obj("{}"), obj(R"({"renders":"<any>"})"), "render_queued"}};
        c.run = [](const QueueRunArgs& a, Context& ctx) -> Result<json::Value> {
            auto& app = ctx.service<EngineApp>();
            if (app.renderQueue().empty())
                return Error("render.queue_empty", "the render queue is empty", {},
                             "render.queue.add a render first");
            std::vector<QueuedWork> work;
            for (const auto& q : app.renderQueue()) {  // all planned before any renders
                auto w = planQueued(app, q);
                if (!w) return std::move(w).error();
                work.push_back(std::move(*w));
            }
            auto& dispatcher = ctx.service<cmd::Dispatcher>();
            const cmd::Origin origin = ctx.origin();
            const auto target = renderTarget(app);
            if (!a.job) {
                json::Value renders = json::Value::array();
                for (auto& w : work) {
                    auto result = runTasks(app, w.tasks, w.args);
                    if (!result) return std::move(result).error();
                    std::erase_if(app.renderQueue(), [&](const auto& q) { return q.id == w.id; });
                    if (w.args.addToProject)
                        postAddToProject(app, target, dispatcher, origin, *result->find("files"));
                    json::Value entry = json::Value::object();
                    entry["id"] = w.id;
                    entry["label"] = w.label;
                    entry["files"] = *result->find("files");
                    renders.push_back(std::move(entry));
                }
                json::Value out = json::Value::object();
                out["renders"] = std::move(renders);
                return out;
            }
            auto available = app.plugins();
            if (!available) return std::move(available).error();
            std::vector<host::PluginInfo> catalogue(available->begin(), available->end());
            const auto identity = app.options().identity;
            auto& jobs = ctx.service<cmd::JobManager>();
            const std::string id = jobs.create();
            app.startJob([&app, &jobs, &dispatcher, origin, target, id, work = std::move(work),
                          catalogue = std::move(catalogue), identity](std::stop_token stop) {
                json::Value renders = json::Value::array();
                std::vector<std::string> done;
                for (const auto& w : work) {
                    if (stop.stop_requested() || jobs.cancelled(id)) return;
                    auto result =
                        runTasks(app, w.tasks, w.args, &jobs, id, &catalogue, &identity, stop);
                    if (!result) {
                        if (!jobs.cancelled(id)) (void)jobs.fail(id, std::move(result).error());
                        // What finished leaves the queue (on the model thread).
                        dispatcher.post("render.queue.done", [&app, target, done]() -> Status {
                            SUPPLE_TRY(checkRenderTarget(app, target));
                            std::erase_if(app.renderQueue(), [&](const auto& q) {
                                return std::find(done.begin(), done.end(), q.id) != done.end();
                            });
                            return {};
                        });
                        return;
                    }
                    json::Value entry = json::Value::object();
                    entry["id"] = w.id;
                    entry["label"] = w.label;
                    entry["files"] = *result->find("files");
                    renders.push_back(std::move(entry));
                    done.push_back(w.id);
                }
                json::Value out = json::Value::object();
                const auto completedRenders = renders;
                out["renders"] = std::move(renders);
                completeRenderJob(
                    app, dispatcher, jobs, id, target, std::move(out),
                    [&app, &dispatcher, target, done, origin, work, completedRenders] {
                        for (std::size_t index = 0; index < work.size(); ++index)
                            if (work[index].args.addToProject)
                                postAddToProject(app, target, dispatcher, origin,
                                                 *completedRenders.at(index).find("files"));
                        dispatcher.post("render.queue.done", [&app, target, done]() -> Status {
                            SUPPLE_TRY(checkRenderTarget(app, target));
                            std::erase_if(app.renderQueue(), [&](const auto& q) {
                                return std::find(done.begin(), done.end(), q.id) != done.end();
                            });
                            return {};
                        });
                    });
            });
            json::Value out = json::Value::object();
            out["job"] = id;
            out["status"] = "running";
            return out;
        };
        registry.add(std::move(c));
    }
}

}  // namespace

void registerRenderCommands(cmd::Registry& registry) {
    for (const char* name : {"render.stems", "render.regions", "render.dry-run"}) {
        Command<Args, json::Value> c;
        c.name = name;
        c.kind = std::string_view(name) == "render.dry-run" ? Kind::Query : Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        const std::string_view which(name);
        c.summary = which == "render.stems" ? "Render each selected track to its own file (stems)."
                    : which == "render.regions"
                        ? "Render each region (or the render matrix's tracks per region) to files."
                        : "List the files a render would write, with lengths, without rendering.";
        c.description =
            "Files go to the renders directory, named by pattern. job: true runs the render as a "
            "job (job.get for progress, job.cancel to stop); results carry a render id and an "
            "analysis summary per file.";
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
            auto& dispatcher = ctx.service<cmd::Dispatcher>();
            const cmd::Origin origin = ctx.origin();
            const auto target = renderTarget(app);
            if (!a.job) {
                auto result = runTasks(app, *tasks, a);
                if (result && a.addToProject)
                    postAddToProject(app, target, dispatcher, origin, *result->find("files"));
                return result;
            }
            auto available = app.plugins();
            if (!available) return std::move(available).error();
            std::vector<host::PluginInfo> catalogue(available->begin(), available->end());
            const auto identity = app.options().identity;
            auto& jobs = ctx.service<cmd::JobManager>();
            const std::string id = jobs.create();
            app.startJob([&app, &jobs, &dispatcher, origin, target, id, tasks = std::move(*tasks),
                          a, catalogue = std::move(catalogue), identity](std::stop_token stop) {
                if (stop.stop_requested() || jobs.cancelled(id)) return;
                auto result = runTasks(app, tasks, a, &jobs, id, &catalogue, &identity, stop);
                if (result) {
                    json::Value files = *result->find("files");
                    completeRenderJob(app, dispatcher, jobs, id, target, std::move(*result),
                                      [&app, target, &dispatcher, origin, files, a] {
                                          if (a.addToProject)
                                              postAddToProject(app, target, dispatcher, origin,
                                                               files);
                                      });
                } else if (!jobs.cancelled(id))
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
    registerQueueCommands(registry);
}
}  // namespace supple::engine
