// Additional session edits and queries from session-model §7.
#include <algorithm>
#include <functional>
#include <set>

#include "supple/engine/app.h"
#include "supple/model/schemas.h"

namespace supple::engine {
namespace {

using cmd::Command;
using cmd::Context;
using cmd::Kind;
using model::Id;

EngineApp& app(Context& ctx) { return ctx.service<EngineApp>(); }
json::Value j(std::string_view s) { return json::parse(s).value(); }

template <class T>
Result<Id> require(const model::Session& s, const std::string& text, std::string path) {
    auto id = Id::parse(text);
    if (id && id->kind == model::Traits<T>::kind && model::Traits<T>::map(s).contains(*id))
        return *id;
    return Error(std::string(model::prefix(model::Traits<T>::kind)) + ".unknown", "no " + text,
                 std::move(path), "use a live id from the corresponding list command");
}

auto idSchema(const char* prefix) {
    return schema::string().pattern(std::string("^") + prefix + "_[0-9]+$", "object id");
}

struct Empty {};
auto emptySchema() { return schema::object<Empty>("empty").build(); }
struct IdResult {
    std::string id;
};
auto idResultSchema() {
    return schema::object<IdResult>("created_id")
        .field("id", &IdResult::id, schema::string())
        .build();
}

struct RouteArgs {
    std::string from, to;
    std::optional<model::Tap> tap;
    std::optional<double> volumeDb, pan;
    std::optional<bool> mute;
};
struct RouteSetArgs {
    std::string route;
    std::optional<std::string> from, to;
    std::optional<model::Tap> tap;
    std::optional<double> volumeDb, pan;
    std::optional<bool> mute;
};
struct RouteRef {
    std::string route;
};
struct RouteEntry {
    std::string id, from, to, tap;
    double volumeDb = 0, pan = 0;
    bool mute = false;
};
struct RouteList {
    std::vector<RouteEntry> routes;
};

struct RulerArgs {
    std::optional<double> pos, start, end;
    std::optional<std::string> name, color;
};
struct RulerSetArgs {
    std::string id;
    std::optional<double> pos, start, end;
    std::optional<std::string> name, color;
};
struct RulerRef {
    std::string id;
};
struct RulerEntry {
    std::string id, name, color;
    std::optional<double> pos, start, end;
};
struct RulerList {
    std::vector<RulerEntry> entries;
};

struct PluginArgs {
    std::optional<std::string> filter;
};
struct PluginEntry {
    std::string id, name, vendor, format;
    bool instrument = false, semantic = false;
};
struct PluginList {
    std::vector<PluginEntry> plugins;
};
struct TimeArgs {
    std::optional<double> seconds, beat;
    std::optional<std::int64_t> bar;
};
struct TimeResult {
    double seconds = 0, beat = 0;
    std::int64_t bar = 1, beatInBar = 1;
    double fraction = 0;
};

struct ItemSetArgs {
    std::string item;
    std::optional<double> position, length, gainDb, fadeIn, fadeOut;
    std::optional<std::int64_t> bar;
    std::optional<bool> mute, loop;
    std::optional<std::string> name, color;
};
struct ItemDuplicateArgs {
    std::string item;
    std::optional<double> to;
};
struct ItemSplitArgs {
    std::string item;
    double at = 0;
};
struct SplitResult {
    std::string left, right;
};
struct TakeRef {
    std::string item, take;
};
struct ItemQuery {
    std::string item;
};
struct TakeAddArgs {
    std::string item;
    std::optional<std::string> from;
};
struct TakeSetArgs {
    std::string take;
    std::optional<double> gainDb, pan, startOffset, playrate, pitch;
    std::optional<model::ChannelMode> channelMode;
    std::optional<std::string> name, pitchMode;
    std::optional<bool> preservePitch;
};
struct TakeEntry {
    std::string id, name, source;
    bool active = false;
    double gainDb = 0, pan = 0, startOffset = 0;
    double playrate = 1, pitch = 0;
    bool preservePitch = true;
    std::string pitchMode;
};
struct TakeList {
    std::vector<TakeEntry> takes;
};
struct FxMoveArgs {
    std::string fx;
    std::optional<std::string> track;
    std::int64_t index = 0;
};
struct FxPatchGetArgs {
    std::string fx;
    bool changedOnly = false;
};
struct FxPatchResult {
    json::Value patch;
};
struct FxParamGetArgs {
    std::string fx, param;
};
struct FxParamResult {
    json::Value value;
};
struct FxPatchSetArgs {
    std::string fx;
    json::Value patch;
};

auto tapSchema() {
    return schema::enumeration<model::Tap>({{model::Tap::PostFader, "post_fader"},
                                            {model::Tap::PreFader, "pre_fader"},
                                            {model::Tap::PreFx, "pre_fx"}});
}

template <class T>
schema::Schema<T> routeArgsSchema(const char* name) {
    auto b = schema::object<T>(name);
    if constexpr (std::is_same_v<T, RouteSetArgs>) b.field("route", &T::route, idSchema("sd"));
    b.field("from", &T::from, idSchema("tr"));
    b.field("to", &T::to, idSchema("tr"));
    b.field("tap", &T::tap, tapSchema());
    b.field("volume_db", &T::volumeDb,
            schema::number().range(model::kSilenceDb, 12).unit(units::Unit::dB));
    b.field("pan", &T::pan, schema::number().range(-100, 100).unit(units::Unit::percent));
    b.field("mute", &T::mute, schema::boolean());
    return b.build();
}

std::optional<std::vector<Id>> routeCycle(const model::Session& s, Id from, Id to,
                                          std::optional<Id> ignored = {}) {
    if (from == to) return std::vector<Id>{from, to};
    std::map<Id, std::vector<Id>> edges;
    for (const auto& e : s.tracks)
        if (e.value->parent && e.value->parentSend.enabled) edges[e.id].push_back(*e.value->parent);
    for (const auto& e : s.routes)
        if (e.id != ignored && e.value->from.track && e.value->to.track)
            edges[*e.value->from.track].push_back(*e.value->to.track);
    std::set<Id> seen;
    std::vector<Id> path{to};
    std::function<bool(Id)> walk = [&](Id cur) {
        if (cur == from) return true;
        if (!seen.insert(cur).second) return false;
        for (Id next : edges[cur]) {
            path.push_back(next);
            if (walk(next)) return true;
            path.pop_back();
        }
        return false;
    };
    if (!walk(to)) return {};
    path.push_back(to);
    return path;
}

Result<model::Route> buildRoute(const RouteArgs& a, const model::Session& s,
                                const model::Route* old = nullptr, std::optional<Id> ignored = {}) {
    model::Route r = old ? *old : model::Route{};
    if (!a.from.empty()) {
        auto from = require<model::Track>(s, a.from, "/from");
        if (!from) return std::move(from).error();
        r.from = model::RouteEnd{};
        r.from.track = *from;
    }
    if (!a.to.empty()) {
        auto to = require<model::Track>(s, a.to, "/to");
        if (!to) return std::move(to).error();
        r.to = model::RouteEnd{};
        r.to.track = *to;
    }
    if (!r.from.track || !r.to.track)
        return Error("route.endpoint", "both track endpoints are required", "",
                     "give from and to track ids");
    if (auto cycle = routeCycle(s, *r.from.track, *r.to.track, ignored)) {
        std::string names;
        for (Id id : *cycle) names += (names.empty() ? "" : " -> ") + id.str();
        return Error("route.cycle", "route creates a cycle: " + names, "/to",
                     "choose a destination that does not feed the source");
    }
    if (a.tap) r.tap = *a.tap;
    if (a.volumeDb) r.volumeDb = *a.volumeDb;
    if (a.pan) r.pan = *a.pan;
    if (a.mute) r.mute = *a.mute;
    return r;
}

void unpoolCopiedSource(model::Take& take, const model::Session& s, model::EditTxn& txn) {
    if (!take.source || take.source->kind != model::Kind::Midi) return;
    const model::MidiSource* source = s.midi.find(*take.source);
    if (source && !source->pooled) take.source = txn.create(*source);
}

template <class T, bool Region, bool WithId>
schema::Schema<T> rulerArgsSchema(const char* name) {
    auto b = schema::object<T>(name);
    if constexpr (WithId) b.field("id", &T::id, idSchema(Region ? "rg" : "mk"));
    if constexpr (Region) {
        b.field("start", &T::start, schema::number().min(0));
        b.field("end", &T::end, schema::number().min(0));
    } else {
        b.field("pos", &T::pos, schema::number().min(0));
    }
    b.field("name", &T::name, schema::string());
    b.field("color", &T::color, schema::string());
    return b.build();
}

auto rulerListSchema(bool region) {
    auto entry = schema::object<RulerEntry>("ruler_entry")
                     .field("id", &RulerEntry::id, schema::string())
                     .field("name", &RulerEntry::name, schema::string())
                     .field("color", &RulerEntry::color, schema::string())
                     .field("pos", &RulerEntry::pos, schema::number())
                     .field("start", &RulerEntry::start, schema::number())
                     .field("end", &RulerEntry::end, schema::number())
                     .build();
    return schema::object<RulerList>("ruler_list")
        .field(region ? "regions" : "markers", &RulerList::entries, schema::arrayOf(entry))
        .build();
}

}  // namespace

void registerExtraCommands(cmd::Registry& registry) {
    {
        Command<RouteArgs, IdResult> c;
        c.name = "route.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Send one track to another track, bus, or the master.";
        c.description =
            "The destination must not feed the source. Master accepts routes. Hardware and plugin "
            "sidechain routing are not supported yet.";
        c.tags = {"route", "mix"};
        c.args = routeArgsSchema<RouteArgs>("route_add_args");
        c.result = idResultSchema();
        c.errors = {{"route.cycle", "the send would feed back", "choose another destination"}};
        c.examples = {{"Send a track to the master",
                       j(R"({"from":"tr_1","to":"tr_0","volume_db":"-6 dB"})"),
                       j(R"({"id":"sd_1"})"), "song"}};
        c.run = [](const RouteArgs& a, Context& ctx) -> Result<IdResult> {
            auto r = buildRoute(a, app(ctx).view());
            if (!r) return std::move(r).error();
            return IdResult{app(ctx).txn().create(std::move(*r)).str()};
        };
        registry.add(std::move(c));
    }
    {
        Command<RouteSetArgs, Empty> c;
        c.name = "route.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Change a route's endpoints, tap, level, pan, or mute.";
        c.tags = {"route", "mix"};
        c.args = routeArgsSchema<RouteSetArgs>("route_set_args");
        c.result = emptySchema();
        c.examples = {
            {"Mute a route", j(R"({"route":"sd_1","mute":true})"), j("{}"), "song_with_route"}};
        c.run = [](const RouteSetArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::Route>(s, a.route, "/route");
            if (!id) return std::move(id).error();
            RouteArgs edits{
                a.from.value_or(""), a.to.value_or(""), a.tap, a.volumeDb, a.pan, a.mute};
            auto r = buildRoute(edits, s, s.routes.find(*id), *id);
            if (!r) return std::move(r).error();
            app(ctx).txn().put<model::Route>(*id, std::move(*r));
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<RouteRef, Empty> c;
        c.name = "route.remove";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Remove a send.";
        c.tags = {"route"};
        c.args = schema::object<RouteRef>("route_ref")
                     .field("route", &RouteRef::route, idSchema("sd"))
                     .build();
        c.result = emptySchema();
        c.examples = {{"Remove", j(R"({"route":"sd_1"})"), j("{}"), "song_with_route"}};
        c.run = [](const RouteRef& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::Route>(s, a.route, "/route");
            if (!id) return std::move(id).error();
            for (const auto& e : s.tracks)
                if (e.value->record.input == *id)
                    return Error("route.in_use", a.route + " is a record input on " + e.id.str(),
                                 "/route", "change that track's record input first");
            auto& t = app(ctx).txn();
            for (const auto& e : s.envelopes)
                if (e.value->target.object == *id) t.remove<model::Envelope>(e.id);
            t.remove<model::Route>(*id);
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<cmd::NoArgs, RouteList> c;
        c.name = "route.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List sends with endpoints and mixer settings.";
        c.tags = {"route"};
        c.args = schema::object<cmd::NoArgs>("no_args").build();
        auto entry = schema::object<RouteEntry>("route_entry")
                         .field("id", &RouteEntry::id, schema::string())
                         .field("from", &RouteEntry::from, schema::string())
                         .field("to", &RouteEntry::to, schema::string())
                         .field("tap", &RouteEntry::tap, schema::string())
                         .field("volume_db", &RouteEntry::volumeDb, schema::number())
                         .field("pan", &RouteEntry::pan, schema::number())
                         .field("mute", &RouteEntry::mute, schema::boolean())
                         .build();
        c.result = schema::object<RouteList>("route_list")
                       .field("routes", &RouteList::routes, schema::arrayOf(entry))
                       .build();
        c.examples = {{"No sends yet", j("{}"), j(R"({"routes":[]})"), "empty"}};
        c.run = [](const cmd::NoArgs&, Context& ctx) -> Result<RouteList> {
            RouteList out;
            for (const auto& e : app(ctx).view().routes) {
                const auto& r = *e.value;
                out.routes.push_back({e.id.str(), r.from.track ? r.from.track->str() : "",
                                      r.to.track ? r.to.track->str() : "",
                                      r.tap == model::Tap::PostFader  ? "post_fader"
                                      : r.tap == model::Tap::PreFader ? "pre_fader"
                                                                      : "pre_fx",
                                      r.volumeDb, r.pan, r.mute});
            }
            return out;
        };
        registry.add(std::move(c));
    }

    auto rulerCommands = [&]<bool Region>() {
        using Object = std::conditional_t<Region, model::Region, model::Marker>;
        const std::string stem = Region ? "region" : "marker";
        const std::string fixture = Region ? "song_with_region" : "song_with_marker";
        const std::string exampleId = Region ? "rg_1" : "mk_1";
        {
            Command<RulerArgs, IdResult> c;
            c.name = stem + ".add";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = Region ? "Add a named region in beats." : "Add a named marker in beats.";
            c.tags = {"ruler"};
            c.args = rulerArgsSchema<RulerArgs, Region, false>((stem + "_add_args").c_str());
            c.result = idResultSchema();
            c.examples = {{"Add",
                           Region ? j(R"({"start":0,"end":4,"name":"Verse"})")
                                  : j(R"({"pos":4,"name":"Verse"})"),
                           j(std::string("{\"id\":\"") + exampleId + "\"}"), "empty"}};
            c.run = [](const RulerArgs& a, Context& ctx) -> Result<IdResult> {
                Object o;
                if constexpr (Region) {
                    if (!a.start || !a.end || *a.end <= *a.start)
                        return Error("region.range", "end must follow start", "/end",
                                     "give start and end in beats");
                    o.start = *a.start;
                    o.end = *a.end;
                } else {
                    if (!a.pos) return Error("marker.pos", "marker position is required", "/pos");
                    o.pos = *a.pos;
                }
                if (a.name) o.name = *a.name;
                if (a.color) o.color = *a.color;
                return IdResult{app(ctx).txn().create(std::move(o)).str()};
            };
            registry.add(std::move(c));
        }
        {
            Command<RulerSetArgs, Empty> c;
            c.name = stem + ".set";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = "Edit a ruler's position, name, or color.";
            c.tags = {"ruler"};
            c.args = rulerArgsSchema<RulerSetArgs, Region, true>((stem + "_set_args").c_str());
            c.result = emptySchema();
            c.examples = {{"Rename",
                           j(std::string("{\"id\":\"") + exampleId + "\",\"name\":\"Bridge\"}"),
                           j("{}"), fixture}};
            c.run = [](const RulerSetArgs& a, Context& ctx) -> Result<Empty> {
                const auto s = app(ctx).view();
                auto id = require<Object>(s, a.id, "/id");
                if (!id) return std::move(id).error();
                Object o = *model::Traits<Object>::map(s).find(*id);
                if constexpr (Region) {
                    if (a.start) o.start = *a.start;
                    if (a.end) o.end = *a.end;
                    if (!(o.end > o.start))
                        return Error("region.range", "end must follow start", "/end");
                } else {
                    if (a.pos) o.pos = *a.pos;
                }
                if (a.name) o.name = *a.name;
                if (a.color) o.color = *a.color;
                app(ctx).txn().put<Object>(*id, std::move(o));
                return Empty{};
            };
            registry.add(std::move(c));
        }
        {
            Command<RulerRef, Empty> c;
            c.name = stem + ".remove";
            c.kind = Kind::Edit;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = "Remove a ruler.";
            c.tags = {"ruler"};
            c.args = schema::object<RulerRef>("ruler_ref")
                         .field("id", &RulerRef::id, idSchema(Region ? "rg" : "mk"))
                         .build();
            c.result = emptySchema();
            c.examples = {
                {"Remove", j(std::string("{\"id\":\"") + exampleId + "\"}"), j("{}"), fixture}};
            c.run = [](const RulerRef& a, Context& ctx) -> Result<Empty> {
                auto id = require<Object>(app(ctx).view(), a.id, "/id");
                if (!id) return std::move(id).error();
                app(ctx).txn().remove<Object>(*id);
                return Empty{};
            };
            registry.add(std::move(c));
        }
        {
            Command<cmd::NoArgs, RulerList> c;
            c.name = stem + ".list";
            c.kind = Kind::Query;
            c.exposure = cmd::Exposure::McpTool;
            c.summary = "List rulers in this project.";
            c.tags = {"ruler"};
            c.args = schema::object<cmd::NoArgs>("no_args").build();
            c.result = rulerListSchema(Region);
            c.examples = {{"List empty", j("{}"),
                           Region ? j(R"({"regions":[]})") : j(R"({"markers":[]})"), "empty"}};
            c.run = [](const cmd::NoArgs&, Context& ctx) -> Result<RulerList> {
                RulerList out;
                const auto s = app(ctx).view();
                for (const auto& e : model::Traits<Object>::map(s)) {
                    RulerEntry row;
                    row.id = e.id.str();
                    row.name = e.value->name;
                    row.color = e.value->color;
                    if constexpr (Region) {
                        row.start = e.value->start;
                        row.end = e.value->end;
                    } else {
                        row.pos = e.value->pos;
                    }
                    out.entries.push_back(std::move(row));
                }
                return out;
            };
            registry.add(std::move(c));
        }
    };
    rulerCommands.template operator()<false>();
    rulerCommands.template operator()<true>();

    {
        Command<PluginArgs, PluginList> c;
        c.name = "plugin.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List scanned plugins; filter matches id, name, or vendor.";
        c.tags = {"plugin"};
        c.args = schema::object<PluginArgs>("plugin_list_args")
                     .field("filter", &PluginArgs::filter, schema::string())
                     .build();
        auto entry =
            schema::object<PluginEntry>("plugin_entry")
                .field("id", &PluginEntry::id, schema::string())
                .field("name", &PluginEntry::name, schema::string())
                .field("vendor", &PluginEntry::vendor, schema::string())
                .field("format", &PluginEntry::format, schema::string().doc("clap or vst3"))
                .field("instrument", &PluginEntry::instrument, schema::boolean())
                .field("semantic", &PluginEntry::semantic, schema::boolean())
                .build();
        c.result = schema::object<PluginList>("plugin_list")
                       .field("plugins", &PluginList::plugins, schema::arrayOf(entry))
                       .build();
        c.examples = {{"Find a test synth", j(R"({"filter":"test.sine"})"),
                       j(R"({"plugins":"<any>"})"), "empty"}};
        c.run = [](const PluginArgs& a, Context& ctx) -> Result<PluginList> {
            auto found = app(ctx).plugins();
            if (!found) return std::move(found).error();
            PluginList out;
            for (const auto& p : *found) {
                if (a.filter && p.id.find(*a.filter) == std::string::npos &&
                    p.name.find(*a.filter) == std::string::npos &&
                    p.vendor.find(*a.filter) == std::string::npos)
                    continue;
                out.plugins.push_back({p.id, p.name, p.vendor, p.format, p.instrument, p.semantic});
            }
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<cmd::NoArgs, PluginList> c;
        c.name = "plugin.scan";
        c.kind = Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Rescan configured plugin paths and return the available plugins.";
        c.tags = {"plugin"};
        c.args = schema::object<cmd::NoArgs>("no_args").build();
        auto entry =
            schema::object<PluginEntry>("plugin_entry")
                .field("id", &PluginEntry::id, schema::string())
                .field("name", &PluginEntry::name, schema::string())
                .field("vendor", &PluginEntry::vendor, schema::string())
                .field("format", &PluginEntry::format, schema::string().doc("clap or vst3"))
                .field("instrument", &PluginEntry::instrument, schema::boolean())
                .field("semantic", &PluginEntry::semantic, schema::boolean())
                .build();
        c.result = schema::object<PluginList>("plugin_list")
                       .field("plugins", &PluginList::plugins, schema::arrayOf(entry))
                       .build();
        c.examples = {{"Scan", j("{}"), j(R"({"plugins":"<any>"})"), "empty"}};
        c.run = [](const cmd::NoArgs&, Context& ctx) -> Result<PluginList> {
            auto found = app(ctx).plugins(true);
            if (!found) return std::move(found).error();
            PluginList out;
            for (const auto& p : *found)
                out.plugins.push_back({p.id, p.name, p.vendor, p.format, p.instrument, p.semantic});
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<TimeArgs, TimeResult> c;
        c.name = "time.convert";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Convert a seconds, beat, or bar position to all three time domains.";
        c.tags = {"time"};
        c.args = schema::object<TimeArgs>("time_convert_args")
                     .field("seconds", &TimeArgs::seconds, schema::number().min(0))
                     .field("beat", &TimeArgs::beat, schema::number().min(0))
                     .field("bar", &TimeArgs::bar, schema::integer().min(1))
                     .build();
        c.result = schema::object<TimeResult>("time_position")
                       .field("seconds", &TimeResult::seconds, schema::number())
                       .field("beat", &TimeResult::beat, schema::number())
                       .field("bar", &TimeResult::bar, schema::integer())
                       .field("beat_in_bar", &TimeResult::beatInBar, schema::integer())
                       .field("fraction", &TimeResult::fraction, schema::number())
                       .build();
        c.examples = {{"Bar two", j(R"({"bar":2})"),
                       j(R"({"seconds":2,"beat":4,"bar":2,"beat_in_bar":1,"fraction":0})"),
                       "empty"}};
        c.run = [](const TimeArgs& a, Context& ctx) -> Result<TimeResult> {
            if (a.seconds.has_value() + a.beat.has_value() + a.bar.has_value() != 1)
                return Error("time.input", "give exactly one of seconds, beat, or bar", "",
                             "remove the other time inputs");
            const auto map = app(ctx).view().timeMap;
            const double beat = a.seconds ? map->beatAt(*a.seconds)
                                : a.bar   ? map->beatOfBar(static_cast<int>(*a.bar))
                                          : *a.beat;
            const auto bb = map->barBeat(beat);
            return TimeResult{map->secondsAt(beat), beat, bb.bar, bb.beat, bb.fraction};
        };
        registry.add(std::move(c));
    }
    {
        Command<ItemSetArgs, Empty> c;
        c.name = "item.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set an item's position, length, playback, fades, name, or color.";
        c.description =
            "Positions and lengths are in beats; fades are in seconds. Name is the active take's "
            "name.";
        c.tags = {"item"};
        c.args = schema::object<ItemSetArgs>("item_set_args")
                     .field("item", &ItemSetArgs::item, idSchema("it"))
                     .field("position", &ItemSetArgs::position, schema::number().min(0))
                     .field("bar", &ItemSetArgs::bar, schema::integer().min(1))
                     .field("length", &ItemSetArgs::length, schema::number().min(0))
                     .field("mute", &ItemSetArgs::mute, schema::boolean())
                     .field("loop", &ItemSetArgs::loop, schema::boolean())
                     .field("gain_db", &ItemSetArgs::gainDb,
                            schema::number().range(model::kSilenceDb, 24).unit(units::Unit::dB))
                     .field("fade_in", &ItemSetArgs::fadeIn, schema::number().min(0))
                     .field("fade_out", &ItemSetArgs::fadeOut, schema::number().min(0))
                     .field("name", &ItemSetArgs::name, schema::string())
                     .field("color", &ItemSetArgs::color, schema::string())
                     .build();
        c.result = emptySchema();
        c.examples = {{"Lower an item", j(R"({"item":"it_1","gain_db":"-3 dB"})"), j("{}"),
                       "song_with_item"}};
        c.run = [](const ItemSetArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const auto& original = *s.items.find(*id);
            if (original.locked)
                return Error("item.locked", "item is locked", "/item", "unlock the item first");
            model::Item it = original;
            if (a.position) it.position = *a.position;
            if (a.bar) it.position = s.timeMap->beatOfBar(static_cast<int>(*a.bar));
            if (a.length) it.length = *a.length;
            if (a.mute) it.mute = *a.mute;
            if (a.loop) it.loop = *a.loop;
            if (a.gainDb) it.gainDb = *a.gainDb;
            if (a.fadeIn) it.fadeIn.length = *a.fadeIn;
            if (a.fadeOut) it.fadeOut.length = *a.fadeOut;
            if (a.color) it.color = *a.color;
            if (a.name && !it.activeTake)
                return Error("item.no_take", "item has no active take to name", "/name");
            auto& t = app(ctx).txn();
            if (a.name)
                t.modify<model::Take>(*it.activeTake, [&](model::Take& tk) { tk.name = *a.name; });
            t.put<model::Item>(*id, std::move(it));
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<ItemDuplicateArgs, IdResult> c;
        c.name = "item.duplicate";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary =
            "Copy an item and its takes to a new beat position (default: immediately after).";
        c.tags = {"item"};
        c.args = schema::object<ItemDuplicateArgs>("item_duplicate_args")
                     .field("item", &ItemDuplicateArgs::item, idSchema("it"))
                     .field("to", &ItemDuplicateArgs::to, schema::number().min(0))
                     .build();
        c.result = idResultSchema();
        c.examples = {{"Repeat", j(R"({"item":"it_1"})"), j(R"({"id":"it_2"})"), "song_with_item"}};
        c.run = [](const ItemDuplicateArgs& a, Context& ctx) -> Result<IdResult> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            model::Item copy = *s.items.find(*id);
            copy.position = a.to.value_or(copy.position + copy.length);
            copy.takes.clear();
            auto& txn = app(ctx).txn();
            for (Id oldId : s.items.find(*id)->takes) {
                model::Take tk = *s.takes.find(oldId);
                unpoolCopiedSource(tk, s, txn);
                tk.chain.clear();
                for (Id fx : s.takes.find(oldId)->chain)
                    tk.chain.push_back(txn.create(*s.fx.find(fx)));
                Id fresh = txn.create(std::move(tk));
                copy.takes.push_back(fresh);
                if (s.items.find(*id)->activeTake == oldId) copy.activeTake = fresh;
            }
            return IdResult{txn.create(std::move(copy)).str()};
        };
        registry.add(std::move(c));
    }
    {
        Command<ItemSplitArgs, SplitResult> c;
        c.name = "item.split";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Split an audio or MIDI item at a beat, keeping take source positions aligned.";
        c.tags = {"item"};
        c.args = schema::object<ItemSplitArgs>("item_split_args")
                     .field("item", &ItemSplitArgs::item, idSchema("it"))
                     .field("at", &ItemSplitArgs::at, schema::number().min(0))
                     .build();
        c.result = schema::object<SplitResult>("item_split_result")
                       .field("left", &SplitResult::left, schema::string())
                       .field("right", &SplitResult::right, schema::string())
                       .build();
        c.examples = {{"Split halfway", j(R"({"item":"it_1","at":2})"),
                       j(R"({"left":"it_1","right":"it_2"})"), "song_with_item"}};
        c.run = [](const ItemSplitArgs& a, Context& ctx) -> Result<SplitResult> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            const model::Item& old = *s.items.find(*id);
            if (old.locked)
                return Error("item.locked", "item is locked", "/item", "unlock it first");
            const auto* tr = s.tracks.find(old.track);
            model::Timebase tb = s.project->timebase;
            if (tr) {
                if (tr->timebase == model::TrackTimebase::Time) tb = model::Timebase::Time;
                if (tr->timebase == model::TrackTimebase::Beats) tb = model::Timebase::Beats;
                if (tr->timebase == model::TrackTimebase::BeatsPosition)
                    tb = model::Timebase::BeatsPosition;
            }
            if (old.timebase == model::ItemTimebase::Time) tb = model::Timebase::Time;
            if (old.timebase == model::ItemTimebase::Beats) tb = model::Timebase::Beats;
            if (old.timebase == model::ItemTimebase::BeatsPosition)
                tb = model::Timebase::BeatsPosition;
            const double splitSec = s.timeMap->secondsAt(a.at);
            const double startBeat =
                tb == model::Timebase::Time ? s.timeMap->beatAt(old.position) : old.position;
            const double startSec = s.timeMap->secondsAt(startBeat);
            const double endSec = tb == model::Timebase::Time ? old.position + old.length
                                  : tb == model::Timebase::Beats
                                      ? s.timeMap->secondsAt(old.position + old.length)
                                      : startSec + old.length;
            if (!(splitSec > startSec && splitSec < endSec))
                return Error("item.split_range", "split must be inside the item", "/at",
                             "choose a beat strictly after the start and before the end");
            model::Item left = old, right = old;
            if (tb == model::Timebase::Time) {
                left.length = splitSec - startSec;
                right.position = splitSec;
                right.length = endSec - splitSec;
            } else if (tb == model::Timebase::Beats) {
                left.length = a.at - old.position;
                right.position = a.at;
                right.length = old.position + old.length - a.at;
            } else {
                left.length = splitSec - startSec;
                right.position = a.at;
                right.length = endSec - splitSec;
            }
            left.fadeOut.length = 0;
            right.fadeIn.length = 0;
            right.takes.clear();
            auto& txn = app(ctx).txn();
            for (Id oldTake : old.takes) {
                model::Take tk = *s.takes.find(oldTake);
                const double delta = tk.source && tk.source->kind == model::Kind::Midi
                                         ? (a.at - startBeat) * tk.playrate
                                         : splitSec - startSec;
                tk.startOffset += delta;
                unpoolCopiedSource(tk, s, txn);
                tk.chain.clear();
                for (Id fx : s.takes.find(oldTake)->chain)
                    tk.chain.push_back(txn.create(*s.fx.find(fx)));
                Id fresh = txn.create(std::move(tk));
                right.takes.push_back(fresh);
                if (old.activeTake == oldTake) right.activeTake = fresh;
            }
            txn.put<model::Item>(*id, std::move(left));
            Id fresh = txn.create(std::move(right));
            return SplitResult{id->str(), fresh.str()};
        };
        registry.add(std::move(c));
    }
    {
        Command<TakeAddArgs, IdResult> c;
        c.name = "take.add";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add a take to an item, optionally copied from another take of that item.";
        c.tags = {"take"};
        c.args = schema::object<TakeAddArgs>("take_add_args")
                     .field("item", &TakeAddArgs::item, idSchema("it"))
                     .field("from", &TakeAddArgs::from, idSchema("tk"))
                     .build();
        c.result = idResultSchema();
        c.examples = {{"Copy a take", j(R"({"item":"it_1","from":"tk_1"})"), j(R"({"id":"tk_2"})"),
                       "song_with_item"}};
        c.run = [](const TakeAddArgs& a, Context& ctx) -> Result<IdResult> {
            const auto s = app(ctx).view();
            auto item = require<model::Item>(s, a.item, "/item");
            if (!item) return std::move(item).error();
            model::Take copy;
            if (a.from) {
                auto source = require<model::Take>(s, *a.from, "/from");
                if (!source) return std::move(source).error();
                const auto& takes = s.items.find(*item)->takes;
                if (std::find(takes.begin(), takes.end(), *source) == takes.end())
                    return Error("take.not_in_item", *a.from + " does not belong to " + a.item,
                                 "/from");
                copy = *s.takes.find(*source);
                unpoolCopiedSource(copy, s, app(ctx).txn());
                copy.chain.clear();
                for (Id fx : s.takes.find(*source)->chain)
                    copy.chain.push_back(app(ctx).txn().create(*s.fx.find(fx)));
            }
            auto& txn = app(ctx).txn();
            Id fresh = txn.create(std::move(copy));
            txn.modify<model::Item>(*item, [&](model::Item& it) {
                it.takes.push_back(fresh);
                if (!it.activeTake) it.activeTake = fresh;
            });
            return IdResult{fresh.str()};
        };
        registry.add(std::move(c));
    }
    {
        Command<TakeRef, Empty> c;
        c.name = "take.activate";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Choose the take heard from an item.";
        c.tags = {"take"};
        c.args = schema::object<TakeRef>("take_activate_args")
                     .field("item", &TakeRef::item, idSchema("it"))
                     .field("take", &TakeRef::take, idSchema("tk"))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Activate", j(R"({"item":"it_1","take":"tk_1"})"), j("{}"), "song_with_item"}};
        c.run = [](const TakeRef& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto item = require<model::Item>(s, a.item, "/item");
            if (!item) return std::move(item).error();
            auto take = require<model::Take>(s, a.take, "/take");
            if (!take) return std::move(take).error();
            const auto& takes = s.items.find(*item)->takes;
            if (std::find(takes.begin(), takes.end(), *take) == takes.end())
                return Error("take.not_in_item", a.take + " does not belong to " + a.item, "/take",
                             "use take.list for this item");
            app(ctx).txn().modify<model::Item>(*item,
                                               [&](model::Item& it) { it.activeTake = *take; });
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<TakeSetArgs, Empty> c;
        c.name = "take.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Set take gain, pan, channel mode, name, source offset, or playrate and pitch.";
        c.tags = {"take"};
        c.args =
            schema::object<TakeSetArgs>("take_set_args")
                .field("take", &TakeSetArgs::take, idSchema("tk"))
                .field("gain_db", &TakeSetArgs::gainDb,
                       schema::number().range(model::kSilenceDb, 24).unit(units::Unit::dB))
                .field("pan", &TakeSetArgs::pan,
                       schema::number().range(-100, 100).unit(units::Unit::percent))
                .field("channel_mode", &TakeSetArgs::channelMode,
                       schema::enumeration<model::ChannelMode>(
                           {{model::ChannelMode::Normal, "normal"},
                            {model::ChannelMode::ReverseStereo, "reverse_stereo"},
                            {model::ChannelMode::MonoMix, "mono_mix"},
                            {model::ChannelMode::Left, "left"},
                            {model::ChannelMode::Right, "right"}}))
                .field("name", &TakeSetArgs::name, schema::string())
                .field("start_offset", &TakeSetArgs::startOffset,
                       schema::number().doc(
                           "Source position at the item start; negative for a late first pass."))
                .field("playrate", &TakeSetArgs::playrate,
                       schema::number()
                           .range(0.01, 100)
                           .unit(units::Unit::x)
                           .doc("Playback speed: 2 plays the source twice as fast. Its length in "
                                "seconds is the item's, so the take runs out sooner or loops."))
                .field("preserve_pitch", &TakeSetArgs::preservePitch,
                       schema::boolean().doc(
                           "Keep the source's pitch when the playrate changes. Needs "
                           "pitch_mode \"signalsmith\"; with \"resample\" the pitch follows the "
                           "speed."))
                .field("pitch", &TakeSetArgs::pitch,
                       schema::number()
                           .range(-48, 48)
                           .unit(units::Unit::st)
                           .doc("Pitch offset in semitones. Stretched when the pitch is preserved, "
                                "otherwise a further speed factor of 2^(pitch/12)."))
                .field("pitch_mode", &TakeSetArgs::pitchMode,
                       schema::enumeration<std::string>(
                           {{"resample", "resample"}, {"signalsmith", "signalsmith"}})
                           .doc("Time-stretch algorithm: \"resample\" (varispeed) or "
                                "\"signalsmith\" (pitch-preserving)."))
                .build();
        c.result = emptySchema();
        c.examples = {
            {"Lower a take", j(R"({"take":"tk_1","gain_db":"-3 dB"})"), j("{}"), "song_with_item"},
            {"Twice as fast, pitch and all",
             j(R"({"take":"tk_1","playrate":2,"preserve_pitch":false})"), j("{}"),
             "song_with_item"},
            {"Half speed, pitch kept",
             j(R"({"take":"tk_1","playrate":0.5,"preserve_pitch":true,"pitch_mode":"signalsmith"})"),
             j("{}"), "song_with_item"},
            {"An octave up, same tempo",
             j(R"({"take":"tk_1","preserve_pitch":true,"pitch_mode":"signalsmith","pitch":12})"),
             j("{}"), "song_with_item"}};
        c.run = [](const TakeSetArgs& a, Context& ctx) -> Result<Empty> {
            auto id = require<model::Take>(app(ctx).view(), a.take, "/take");
            if (!id) return std::move(id).error();
            app(ctx).txn().modify<model::Take>(*id, [&](model::Take& tk) {
                if (a.gainDb) tk.gainDb = *a.gainDb;
                if (a.pan) tk.pan = *a.pan;
                if (a.channelMode) tk.channelMode = *a.channelMode;
                if (a.name) tk.name = *a.name;
                if (a.startOffset) tk.startOffset = *a.startOffset;
                if (a.playrate) tk.playrate = *a.playrate;
                if (a.preservePitch) tk.preservePitch = *a.preservePitch;
                if (a.pitch) tk.pitch = *a.pitch;
                if (a.pitchMode) tk.pitchMode = *a.pitchMode;
            });
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<ItemQuery, TakeList> c;
        c.name = "take.list";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List the takes of an item and identify its active take.";
        c.tags = {"take"};
        c.args = schema::object<ItemQuery>("take_list_args")
                     .field("item", &ItemQuery::item, idSchema("it"))
                     .build();
        auto entry =
            schema::object<TakeEntry>("take_entry")
                .field("id", &TakeEntry::id, schema::string())
                .field("name", &TakeEntry::name, schema::string())
                .field("source", &TakeEntry::source, schema::string())
                .field("active", &TakeEntry::active, schema::boolean())
                .field("gain_db", &TakeEntry::gainDb, schema::number())
                .field("pan", &TakeEntry::pan, schema::number())
                .field("start_offset", &TakeEntry::startOffset, schema::number())
                .field("playrate", &TakeEntry::playrate, schema::number().unit(units::Unit::x))
                .field("preserve_pitch", &TakeEntry::preservePitch, schema::boolean())
                .field("pitch", &TakeEntry::pitch, schema::number().unit(units::Unit::st))
                .field("pitch_mode", &TakeEntry::pitchMode, schema::string())
                .build();
        c.result = schema::object<TakeList>("take_list")
                       .field("takes", &TakeList::takes, schema::arrayOf(entry))
                       .build();
        c.examples = {
            {"List", j(R"({"item":"it_1"})"), j(R"({"takes":"<any>"})"), "song_with_item"}};
        c.run = [](const ItemQuery& a, Context& ctx) -> Result<TakeList> {
            const auto s = app(ctx).view();
            auto id = require<model::Item>(s, a.item, "/item");
            if (!id) return std::move(id).error();
            TakeList out;
            const auto& it = *s.items.find(*id);
            for (Id takeId : it.takes) {
                const auto& tk = *s.takes.find(takeId);
                out.takes.push_back({takeId.str(), tk.name, tk.source ? tk.source->str() : "",
                                     it.activeTake == takeId, tk.gainDb, tk.pan, tk.startOffset,
                                     tk.playrate, tk.pitch, tk.preservePitch, tk.pitchMode});
            }
            return out;
        };
        registry.add(std::move(c));
    }
    {
        Command<FxMoveArgs, Empty> c;
        c.name = "fx.move";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Move a plugin within a track chain or to another track.";
        c.tags = {"fx"};
        c.args = schema::object<FxMoveArgs>("fx_move_args")
                     .field("fx", &FxMoveArgs::fx, idSchema("fx"))
                     .field("track", &FxMoveArgs::track, idSchema("tr"))
                     .field("index", &FxMoveArgs::index, schema::integer().min(0))
                     .build();
        c.result = emptySchema();
        c.examples = {
            {"Move a gain first", j(R"({"fx":"fx_2","index":0})"), j("{}"), "song_with_fx"}};
        c.run = [](const FxMoveArgs& a, Context& ctx) -> Result<Empty> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            std::optional<Id> source;
            for (const auto& e : s.tracks)
                if (std::find(e.value->chain.begin(), e.value->chain.end(), *id) !=
                    e.value->chain.end())
                    source = e.id;
            if (!source)
                return Error("fx.chain", "this plugin is not in a track chain", "/fx",
                             "move track FX with fx.move; take FX require a take-chain command");
            Id target = *source;
            if (a.track) {
                auto tr = require<model::Track>(s, *a.track, "/track");
                if (!tr) return std::move(tr).error();
                target = *tr;
            }
            auto chain = s.tracks.find(*source)->chain;
            chain.erase(std::remove(chain.begin(), chain.end(), *id), chain.end());
            auto targetChain = target == *source ? chain : s.tracks.find(target)->chain;
            if (static_cast<std::size_t>(a.index) > targetChain.size())
                return Error("fx.index", "index is past the end of the destination chain", "/index",
                             "use an index from 0 to " + std::to_string(targetChain.size()));
            targetChain.insert(targetChain.begin() + a.index, *id);
            auto& txn = app(ctx).txn();
            if (target != *source)
                txn.modify<model::Track>(*source, [&](model::Track& tr) { tr.chain = chain; });
            txn.modify<model::Track>(target, [&](model::Track& tr) { tr.chain = targetChain; });
            return Empty{};
        };
        registry.add(std::move(c));
    }
    {
        Command<FxPatchGetArgs, FxPatchResult> c;
        c.name = "fx.patch.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read a semantic plugin's saved patch.";
        c.tags = {"fx", "patch"};
        c.args = schema::object<FxPatchGetArgs>("fx_patch_get_args")
                     .field("fx", &FxPatchGetArgs::fx, idSchema("fx"))
                     .field("changed_only", &FxPatchGetArgs::changedOnly, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::object<FxPatchResult>("fx_patch_result")
                       .field("patch", &FxPatchResult::patch, schema::any())
                       .build();
        c.examples = {
            {"Read gain", j(R"({"fx":"fx_2"})"), j(R"({"patch":"<any>"})"), "song_with_fx"}};
        c.run = [](const FxPatchGetArgs& a, Context& ctx) -> Result<FxPatchResult> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            const auto& fx = *s.fx.find(*id);
            auto effective = app(ctx).mergedPatch(fx, json::Value::object(), false);
            if (!effective) return std::move(effective).error();
            if (a.changedOnly) {
                model::FxInstance defaults = fx;
                defaults.patch.reset();
                auto baseline = app(ctx).mergedPatch(defaults, json::Value::object(), false);
                if (!baseline) return std::move(baseline).error();
                auto diff = json::createMergePatch(*baseline, *effective);
                if (!diff) return std::move(diff).error();
                return FxPatchResult{std::move(*diff)};
            }
            return FxPatchResult{std::move(*effective)};
        };
        registry.add(std::move(c));
    }
    {
        Command<FxPatchSetArgs, FxPatchResult> c;
        c.name = "fx.patch.set";
        c.kind = Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Replace a semantic plugin's complete patch.";
        c.tags = {"fx", "patch"};
        c.args = schema::object<FxPatchSetArgs>("fx_patch_set_args")
                     .field("fx", &FxPatchSetArgs::fx, idSchema("fx"))
                     .field("patch", &FxPatchSetArgs::patch, schema::any())
                     .build();
        c.result = schema::object<FxPatchResult>("fx_patch_result")
                       .field("patch", &FxPatchResult::patch, schema::any())
                       .build();
        c.examples = {
            {"Replace gain",
             j(R"({"fx":"fx_2","patch":{"schema":"supple.test-gain-patch/1.0","params":{"gain":-6}}})"),
             j(R"({"patch":"<any>"})"), "song_with_fx"}};
        c.run = [](const FxPatchSetArgs& a, Context& ctx) -> Result<FxPatchResult> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            auto patch = app(ctx).mergedPatch(*s.fx.find(*id), a.patch, true);
            if (!patch) {
                Error e = std::move(patch).error();
                e.path = "/patch" + e.path;
                return e;
            }
            app(ctx).txn().modify<model::FxInstance>(
                *id, [&](model::FxInstance& fx) { fx.patch = *patch; });
            return FxPatchResult{std::move(*patch)};
        };
        registry.add(std::move(c));
    }
    {
        Command<FxParamGetArgs, FxParamResult> c;
        c.name = "fx.param.get";
        c.kind = Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Read one saved plugin parameter by semantic key or numeric CLAP id.";
        c.tags = {"fx", "params"};
        c.args = schema::object<FxParamGetArgs>("fx_param_get_args")
                     .field("fx", &FxParamGetArgs::fx, idSchema("fx"))
                     .field("param", &FxParamGetArgs::param, schema::string().minLength(1))
                     .build();
        c.result = schema::object<FxParamResult>("fx_param_result")
                       .field("value", &FxParamResult::value, schema::any())
                       .build();
        c.examples = {{"Read gain", j(R"({"fx":"fx_2","param":"gain"})"), j(R"({"value":"<any>"})"),
                       "song_with_fx"}};
        c.run = [](const FxParamGetArgs& a, Context& ctx) -> Result<FxParamResult> {
            const auto s = app(ctx).view();
            auto id = require<model::FxInstance>(s, a.fx, "/fx");
            if (!id) return std::move(id).error();
            const auto& fx = *s.fx.find(*id);
            auto effective = app(ctx).mergedPatch(fx, json::Value::object(), false);
            if (effective) {
                const auto* params = effective->find("params");
                if (params && params->isObject())
                    if (const auto* value = params->find(a.param)) return FxParamResult{*value};
            }
            for (const auto& [key, value] : fx.params)
                if (std::to_string(key) == a.param) return FxParamResult{json::Value(value)};
            return Error("fx.param", "no saved parameter " + a.param, "/param",
                         "use fx.describe {full:true} for parameter names");
        };
        registry.add(std::move(c));
    }
}

}  // namespace supple::engine
