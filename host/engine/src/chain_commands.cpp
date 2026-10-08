// FX chain presets: ordered, canonical documents independent of session ids.
// File actions never create history; a load validates first and commits once.
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <set>
#include <tuple>

#include "supple/base/files.h"
#include "supple/base/sha256.h"
#include "supple/engine/app.h"
#include "supple/model/schemas.h"
#include "supple/product.h"

namespace supple::engine {
namespace {
namespace fs = std::filesystem;
using model::Id;
constexpr std::string_view kSchema = "supple.fx-chain/1.0";
constexpr std::size_t kMaxFile = 64 * 1024 * 1024;

struct SaveArgs {
    std::string track, name, chain = "main";
    bool replace = false;
};
struct LoadArgs {
    std::string track;
    std::optional<std::string> name, path;
    std::string chain = "main", mode = "append";
};
struct NoArgs {};
struct CopyArgs {
    std::string fx, track, chain = "main";
    std::optional<std::int64_t> index;
};
struct CopyResult {
    std::string fx;
    std::vector<std::string> chain;
};
struct SaveResult {
    std::string path;
    std::int64_t effects = 0;
};
struct LoadResult {
    std::vector<std::string> effects;
};
struct ChainEntry {
    std::string name, path;
    std::int64_t effects = 0;
};
struct ListResult {
    std::vector<ChainEntry> chains;
};
struct Document {
    std::string schema, name;
    std::vector<json::Value> effects;
};

json::Value j(std::string_view text) { return json::parse(text).value(); }

const schema::Schema<Document>& documentSchema() {
    static const auto value =
        schema::object<Document>("fx_chain")
            .field("schema", &Document::schema,
                   schema::string().pattern("^supple\\.fx-chain/1\\.0$", "supple.fx-chain/1.0"))
            .field("name", &Document::name, schema::string().minLength(1))
            .field("effects", &Document::effects, schema::arrayOf(schema::any()))
            .build();
    return value;
}

Status validName(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.size() > 200 ||
        std::any_of(name.begin(), name.end(),
                    [](unsigned char c) { return c < 32 || c == '/' || c == '\\'; }))
        return Error("chain.name", "invalid chain name", "/name",
                     "use 1–200 characters without path separators or control characters");
    return {};
}

Result<fs::path> chainDir(const EngineApp& app) {
    if (!app.options().chainDir.empty()) return app.options().chainDir;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / product::kSlug / "chains";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".config" / product::kSlug / "chains";
    return Error("chain.directory", "no configuration directory", {},
                 "set XDG_CONFIG_HOME or AppOptions::chainDir");
}

Result<fs::path> namedPath(const EngineApp& app, const std::string& name) {
    SUPPLE_TRY(validName(name));
    auto dir = chainDir(app);
    if (!dir) return std::move(dir).error();
    return *dir / (name + ".json");
}

Result<Id> trackOf(const model::Session& s, const std::string& name) {
    auto id = Id::parse(name);
    if (!id || id->kind != model::Kind::Track || !s.tracks.find(*id))
        return Error("track.unknown", "no track " + name, "/track",
                     "use a live id from track.list");
    return *id;
}

Result<Document> readDocument(const fs::path& path) {
    auto text = readTextFile(path, kMaxFile);
    if (!text) return std::move(text).error();
    auto doc = json::parse(*text);
    if (!doc) return std::move(doc).error();
    auto decoded = documentSchema().decode(*doc);
    if (!decoded) return std::move(decoded).error();
    if (decoded->effects.empty())
        return Error("chain.empty", path.string() + " contains no effects", "/effects",
                     "save a nonempty FX chain");
    SUPPLE_TRY(validName(decoded->name));
    for (std::size_t i = 0; i < decoded->effects.size(); ++i) {
        auto fx = model::schemaOf<model::FxInstance>().decode(decoded->effects[i]);
        const std::string at = "/effects/" + std::to_string(i);
        if (!fx) return std::move(fx).error().at(at);
        if ((fx->oversampling & (fx->oversampling - 1)) != 0)
            return Error("chain.invalid", "oversampling must be 1, 2, 4 or 8", at + "/oversampling",
                         "set oversampling to one of 1, 2, 4 or 8");
        if (fx->patch.has_value() == fx->state.has_value())
            return Error("chain.invalid", "effect needs exactly one patch or state", at + "/state",
                         "provide patch for a semantic plugin or base64 state for a plain plugin; "
                         "omit the other field");
    }
    return decoded;
}

// The model's blob references are content hashes ("sha256:<hex>").
std::string stateHash(std::span<const std::uint8_t> bytes) { return "sha256:" + sha256Hex(bytes); }

Status writePreset(const fs::path& path, std::string_view text, bool replace) {
    if (replace) return writeFileAtomic(path, text);
    // Publishing with link is atomic and refuses an existing destination,
    // including when another process saves the same name concurrently.
    static std::atomic<unsigned> serial{0};
    const fs::path temp = path.string() + "." + std::to_string(::getpid()) + "." +
                          std::to_string(serial.fetch_add(1)) + ".preset-tmp";
    SUPPLE_TRY(writeFileAtomic(temp, text));
    const int result = ::link(temp.c_str(), path.c_str());
    const int error = errno;
    std::error_code ignored;
    fs::remove(temp, ignored);
    if (result == 0) return {};
    return Error(error == EEXIST ? "chain.exists" : "chain.write",
                 "cannot save " + path.string() + ": " + std::strerror(error), "/name",
                 error == EEXIST ? "choose another name or set replace: true"
                                 : "check directory permissions and free space");
}

// The effects of a chain as preset entries: the semantic patch when the
// plugin has one, else its state as base64 (fx.chain.save, track templates).
Result<std::vector<json::Value>> encodeEffects(EngineApp& app, const model::Session& s,
                                               const std::vector<Id>& chain) {
    std::vector<json::Value> effects;
    for (Id id : chain) {
        const auto& fx = *s.fx.find(id);
        auto info = app.findPlugin(fx.plugin.id);
        if (!info) return std::move(info).error();
        auto value = model::toJson(fx);
        value["plugin"]["name"] = fx.plugin.name;
        value["plugin"]["vendor"] = fx.plugin.vendor;
        value["plugin"]["version"] = fx.plugin.version;
        value["label"] = fx.label;
        value["bypass"] = fx.bypass;
        value["offline"] = fx.offline;
        value["wet"] = fx.wet;
        static constexpr const char* isolation[] = {"inherit", "in_process", "shared_bridge",
                                                    "dedicated"};
        value["isolation"] = isolation[static_cast<std::size_t>(fx.isolation)];
        value["oversampling"] = fx.oversampling;
        if (info->semantic) {
            auto patch = app.mergedPatch(fx, json::Value::object(), false);
            if (!patch) return std::move(patch).error();
            value["patch"] = std::move(*patch);
            value.asObject().erase("state");
        } else {
            std::vector<std::uint8_t> bytes;
            if (fx.state) {
                const auto blob = s.blobs.find(*fx.state);
                if (blob == s.blobs.end())
                    return Error("engine.blob_missing", "missing state for " + id.str(), "/effects",
                                 "capture the plugin state again");
                bytes = blob->second->data;
            } else {
                auto probe = host::Instance::create(*info, app.options().identity);
                if (!probe) return std::move(probe).error();
                for (const auto& [param, v] : fx.params) SUPPLE_TRY((*probe)->setParam(param, v));
                auto state = (*probe)->saveState();
                if (!state) return std::move(state).error();
                bytes = std::move(*state);
            }
            value.asObject().erase("patch");
            value["state"] = base64Encode(bytes);
        }
        effects.push_back(std::move(value));
    }
    return effects;
}

// A preset effect checked and ready to add: its plugin installed, its patch
// merged or its state loaded once (nothing changes the model yet).
struct Prepared {
    model::FxInstance fx;
    std::shared_ptr<const model::Blob> blob;
};

Result<std::vector<Prepared>> prepareEffects(EngineApp& app,
                                             const std::vector<json::Value>& entries,
                                             const std::string& base) {
    std::vector<Prepared> prepared;
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const std::string at = base + "/" + std::to_string(i);
        auto fx = model::schemaOf<model::FxInstance>().decode(entries[i]);
        if (!fx) return std::move(fx).error().at(at);
        if ((fx->oversampling & (fx->oversampling - 1)) != 0)
            return Error("chain.invalid", "oversampling must be 1, 2, 4 or 8", at + "/oversampling",
                         "set oversampling to one of 1, 2, 4 or 8");
        auto info = app.findPlugin(fx->plugin.id);
        if (!info || info->id != fx->plugin.id || info->format != fx->plugin.format)
            return Error("chain.plugin_missing",
                         "plugin " + fx->plugin.name + " (" + fx->plugin.format + ":" +
                             fx->plugin.id + ") is not installed",
                         at + "/plugin", "install this plugin or choose another preset");
        Prepared p{*fx, {}};
        if (info->semantic) {
            if (!fx->patch || fx->state)
                return Error("chain.invalid", "a semantic effect needs a patch", at + "/patch",
                             "provide patch containing this semantic plugin's patch document and "
                             "omit state");
            auto patch = app.mergedPatch(*fx, *fx->patch, true);
            if (!patch) return std::move(patch).error().at(at + "/patch");
            p.fx.patch = std::move(*patch);
        } else {
            if (!fx->state || fx->patch)
                return Error(
                    "chain.invalid", "a plain effect needs base64 state", at + "/state",
                    "provide base64 state from a saved instance of this plugin and omit patch");
            auto bytes = base64Decode(*fx->state);
            if (!bytes) return std::move(bytes).error().at(at + "/state");
            auto probe = host::Instance::create(*info, app.options().identity);
            if (!probe) return std::move(probe).error().at(at + "/plugin");
            SUPPLE_TRY((*probe)->loadState(*bytes));
            p.fx.state = stateHash(*bytes);
            p.blob = std::make_shared<const model::Blob>(model::Blob{std::move(*bytes)});
        }
        prepared.push_back(std::move(p));
    }
    return prepared;
}

// Adds prepared effects to the open transaction, appended to `chain`; their ids.
std::vector<std::string> installEffects(EngineApp& app, std::vector<Prepared>& prepared,
                                        std::vector<Id>& chain) {
    std::vector<std::string> ids;
    for (auto& p : prepared) {
        if (p.blob) app.txn().putBlob(*p.fx.state, p.blob);
        const Id id = app.txn().create(std::move(p.fx));
        chain.push_back(id);
        ids.push_back(id.str());
    }
    return ids;
}

// A plugin's settings as the running instance has them right now (what the
// user hears: an editor edit the app has not consumed yet is not in the model),
// else as the model has them: `fx` gets the state, `blob` its bytes when it is
// an opaque state.
Status captureLiveState(EngineApp& app, Id id, model::FxInstance& fx,
                        std::shared_ptr<const model::Blob>& blob) {
    host::Instance* live = app.instanceOf(id);
    if (!live || live->failed()) return {};
    if (live->info().semantic) {
        auto patch = live->getPatch(false);
        if (!patch) return std::move(patch).error();
        auto parsed = json::parse(*patch);
        if (!parsed) return std::move(parsed).error();
        fx.patch = std::move(*parsed);
        fx.state.reset();
        return {};
    }
    auto state = live->saveState();
    if (!state) return std::move(state).error();
    fx.state = stateHash(*state);
    fx.patch.reset();
    blob = std::make_shared<const model::Blob>(model::Blob{std::move(*state)});
    return {};
}

Result<json::Value> invoke(const char* name, json::Value args, cmd::Context& ctx) {
    return ctx.service<const cmd::Registry>().find(name)->invoke(args, ctx);
}
// --- track templates ----------------------------------------------------------------------

constexpr std::string_view kTemplateSchema = "supple.track-template/1.0";

Result<fs::path> templateDir(const EngineApp& app) {
    if (!app.options().templateDir.empty()) return app.options().templateDir;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / product::kSlug / "track-templates";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".config" / product::kSlug / "track-templates";
    return Error("template.directory", "no configuration directory", {},
                 "set XDG_CONFIG_HOME or AppOptions::templateDir");
}

Result<fs::path> templatePath(const EngineApp& app, const std::string& name) {
    SUPPLE_TRY(validName(name));
    auto dir = templateDir(app);
    if (!dir) return std::move(dir).error();
    return *dir / (name + ".json");
}

struct TemplateDoc {
    std::string schema, name;
    json::Value track = json::Value::object();
    std::vector<json::Value> chain, inputChain;
};

const schema::Schema<TemplateDoc>& templateSchema() {
    static const auto value =
        schema::object<TemplateDoc>("track_template")
            .field("schema", &TemplateDoc::schema,
                   schema::string().pattern("^supple\\.track-template/1\\.0$",
                                            "supple.track-template/1.0"))
            .field("name", &TemplateDoc::name, schema::string().minLength(1))
            .field("track", &TemplateDoc::track, schema::any())
            .field("chain", &TemplateDoc::chain, schema::arrayOf(schema::any()))
            .field("input_chain", &TemplateDoc::inputChain, schema::arrayOf(schema::any()))
            .build();
    return value;
}

struct TemplateSaveArgs {
    std::string track, name;
    bool replace = false;
};
struct TemplateLoadArgs {
    std::optional<std::string> name, path, parent;
};
struct TemplateLoadResult {
    std::string track;
    std::vector<std::string> effects;
};
struct TemplateListResult {
    std::vector<ChainEntry> templates;
};

void registerTemplateCommands(cmd::Registry& registry) {
    {
        cmd::Command<TemplateSaveArgs, SaveResult> c;
        c.name = "track.template.save";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save a strip (settings and FX chains) as a named track template.";
        c.description =
            "Writes a supple.track-template/1.0 file to the user's track-templates directory: "
            "the strip's type, name, colour, notes, level, pan and monitoring, and its main and "
            "input chains as fx.chain.save stores them (semantic patches or base64 state). Not "
            "routing or items. replace: true overwrites an existing name.";
        c.tags = {"track", "presets"};
        c.args = schema::object<TemplateSaveArgs>("track_template_save_args")
                     .field("track", &TemplateSaveArgs::track,
                            schema::string().pattern("^tr_[0-9]+$", "track id"))
                     .field("name", &TemplateSaveArgs::name, schema::string().minLength(1))
                     .field("replace", &TemplateSaveArgs::replace, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::object<SaveResult>("track_template_save_result")
                       .field("path", &SaveResult::path, schema::string())
                       .field("effects", &SaveResult::effects, schema::integer().min(0))
                       .build();
        c.examples = {{"Save the bass strip", j(R"({"track":"tr_1","name":"Bass"})"),
                       j(R"({"path":"<any>","effects":2})"), "song_with_chain"}};
        c.run = [](const TemplateSaveArgs& a, cmd::Context& ctx) -> Result<SaveResult> {
            auto& app = ctx.service<EngineApp>();
            auto path = templatePath(app, a.name);
            if (!path) return std::move(path).error();
            const auto s = app.view();
            auto track = trackOf(s, a.track);
            if (!track) return std::move(track).error();
            const model::Track& strip = *s.tracks.find(*track);
            if (strip.type == model::StripType::Master)
                return Error("track.strip_type", "a master is not a template", "/track",
                             "save a channel or a buss");
            TemplateDoc doc;
            doc.schema = std::string(kTemplateSchema);
            doc.name = a.name;
            doc.track["type"] = strip.type == model::StripType::Channel ? "channel" : "buss";
            doc.track["name"] = strip.name;
            doc.track["channels"] = strip.channels;
            doc.track["color"] = strip.color;
            doc.track["notes"] = strip.notes;
            doc.track["volume_db"] = strip.volumeDb;
            doc.track["pan"] = strip.pan;
            static constexpr const char* kMonitor[] = {"off", "on", "auto"};
            doc.track["monitor"] = kMonitor[static_cast<std::size_t>(strip.record.monitor) % 3];
            auto main = encodeEffects(app, s, strip.chain);
            if (!main) return std::move(main).error();
            auto input = encodeEffects(app, s, strip.inputChain);
            if (!input) return std::move(input).error();
            doc.chain = std::move(*main);
            doc.inputChain = std::move(*input);
            const auto text = json::writeCanonical(templateSchema().encode(doc)) + "\n";
            if (text.size() > kMaxFile)
                return Error("template.size", "template exceeds the 64 MiB file limit", "/track",
                             "save a strip with smaller plugin states");
            SUPPLE_TRY(writePreset(*path, text, a.replace));
            return SaveResult{path->string(),
                              static_cast<std::int64_t>(doc.chain.size() + doc.inputChain.size())};
        };
        registry.add(std::move(c));
    }
    {
        cmd::Command<TemplateLoadArgs, TemplateLoadResult> c;
        c.name = "track.template.load";
        c.kind = cmd::Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Add a strip from a track template (settings and FX) in one undo step.";
        c.description =
            "Give exactly one of name (in the user's track-templates directory) or path. parent "
            "defaults to the master. Every plugin and state is checked before the model changes.";
        c.tags = {"track", "presets"};
        c.args = schema::object<TemplateLoadArgs>("track_template_load_args")
                     .field("name", &TemplateLoadArgs::name, schema::string().minLength(1))
                     .field("path", &TemplateLoadArgs::path, schema::string().minLength(1))
                     .field("parent", &TemplateLoadArgs::parent,
                            schema::string().pattern("^tr_[0-9]+$", "track id"))
                     .build();
        c.result =
            schema::object<TemplateLoadResult>("track_template_load_result")
                .field("track", &TemplateLoadResult::track, schema::string())
                .field("effects", &TemplateLoadResult::effects, schema::arrayOf(schema::string()))
                .build();
        c.examples = {{"Add the saved bass strip", j(R"({"name":"Bass"})"),
                       j(R"({"track":"<any>","effects":["<any>","<any>"]})"),
                       "song_with_saved_template"}};
        c.run = [](const TemplateLoadArgs& a, cmd::Context& ctx) -> Result<TemplateLoadResult> {
            auto& app = ctx.service<EngineApp>();
            if (a.name.has_value() == a.path.has_value())
                return Error("template.source", "give exactly one of name or path", "/name",
                             "use track.template.list to find a saved name");
            auto path =
                a.name ? templatePath(app, *a.name) : Result<fs::path>(app.resolve(*a.path));
            if (!path) return std::move(path).error();
            auto text = readTextFile(*path);
            if (!text) return std::move(text).error();
            auto parsed = json::parse(*text);
            if (!parsed) return std::move(parsed).error();
            auto doc = templateSchema().decode(*parsed);
            if (!doc) return std::move(doc).error();
            auto main = prepareEffects(app, doc->chain, "/chain");
            if (!main) return std::move(main).error();
            auto input = prepareEffects(app, doc->inputChain, "/input_chain");
            if (!input) return std::move(input).error();
            auto field = [&](const char* key) { return doc->track.find(key); };
            json::Value add = json::Value::object();
            const json::Value* type = field("type");
            add["type"] = type && type->isString() ? type->asString() : std::string("channel");
            if (const json::Value* name = field("name"); name && name->isString())
                add["name"] = name->asString();
            if (const json::Value* channels = field("channels")) add["channels"] = *channels;
            if (a.parent) add["parent"] = *a.parent;
            auto added = invoke("track.add", std::move(add), ctx);
            if (!added) return std::move(added).error();
            const std::string track = (*added)["track"].asString();
            json::Value set = json::Value::object();
            set["track"] = track;
            for (const char* key : {"color", "notes", "volume_db", "pan"})
                if (const json::Value* v = field(key); v && !v->isNull()) set[key] = *v;
            const bool channel = !type || !type->isString() || type->asString() == "channel";
            if (const json::Value* v = field("monitor"); channel && v && v->isString())
                set["monitor"] = *v;
            auto changed = invoke("track.set", std::move(set), ctx);
            if (!changed) return std::move(changed).error();
            const Id id = *model::Id::parse(track);
            const model::Track& strip = *app.view().tracks.find(id);
            std::vector<Id> chain = strip.chain, inputChain = strip.inputChain;
            TemplateLoadResult result;
            result.track = track;
            for (auto& e : installEffects(app, *main, chain)) result.effects.push_back(e);
            for (auto& e : installEffects(app, *input, inputChain)) result.effects.push_back(e);
            app.txn().modify<model::Track>(id, [&](model::Track& t) {
                t.chain = chain;
                t.inputChain = inputChain;
            });
            return result;
        };
        registry.add(std::move(c));
    }
    {
        cmd::Command<NoArgs, TemplateListResult> c;
        c.name = "track.template.list";
        c.kind = cmd::Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List saved track templates by name, path and effect count.";
        c.tags = {"track", "presets"};
        c.args = schema::object<NoArgs>("track_template_list_args").build();
        auto entry = schema::object<ChainEntry>("track_template_entry")
                         .field("name", &ChainEntry::name, schema::string())
                         .field("path", &ChainEntry::path, schema::string())
                         .field("effects", &ChainEntry::effects, schema::integer().min(0))
                         .build();
        c.result = schema::object<TemplateListResult>("track_template_list_result")
                       .field("templates", &TemplateListResult::templates, schema::arrayOf(entry))
                       .build();
        c.examples = {{"None yet", j("{}"), j(R"({"templates":[]})"), "empty"}};
        c.run = [](const NoArgs&, cmd::Context& ctx) -> Result<TemplateListResult> {
            auto dir = templateDir(ctx.service<EngineApp>());
            if (!dir) return std::move(dir).error();
            TemplateListResult result;
            std::error_code ec;
            if (!fs::exists(*dir, ec)) return result;
            for (const auto& e : fs::directory_iterator(*dir, ec)) {
                if (e.path().extension() != ".json") continue;
                auto text = readTextFile(e.path());
                if (!text) continue;
                auto parsed = json::parse(*text);
                if (!parsed) continue;
                auto doc = templateSchema().decode(*parsed);
                if (!doc) continue;
                result.templates.push_back(
                    {doc->name, e.path().string(),
                     static_cast<std::int64_t>(doc->chain.size() + doc->inputChain.size())});
            }
            std::sort(result.templates.begin(), result.templates.end(),
                      [](const ChainEntry& x, const ChainEntry& y) {
                          return std::tie(x.name, x.path) < std::tie(y.name, y.path);
                      });
            return result;
        };
        registry.add(std::move(c));
    }
}

// Removes a saved preset or template file by name (fx.chain.delete,
// project.template.delete); the file's own directory is never removed.
Status removeNamed(const fs::path& path, const char* code, const std::string& what) {
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(path, ec)))
        return Error(code, "no saved " + what + " " + path.stem().string(), "/name",
                     "use a name the list command returns");
    if (!fs::remove(path, ec) || ec)
        return Error(code, "cannot delete " + path.string() + ": " + ec.message(), "/name",
                     "check the directory permissions");
    return {};
}

// --- project templates ----------------------------------------------------------------------

Result<fs::path> projectTemplateDir(const EngineApp& app) {
    if (!app.options().projectTemplateDir.empty()) return app.options().projectTemplateDir;
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return fs::path(xdg) / product::kSlug / "templates";
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".config" / product::kSlug / "templates";
    return Error("template.directory", "no configuration directory", {},
                 "set XDG_CONFIG_HOME or AppOptions::projectTemplateDir");
}

// A template is a folder, <dir>/<name>/project.json with media/; an older one
// is a single file, <dir>/<name>.json, with absolute media paths.
struct TemplateLocation {
    fs::path folder, legacy;
    bool folderExists() const {
        std::error_code ec;
        return fs::is_regular_file(folder / "project.json", ec);
    }
    bool legacyExists() const {
        std::error_code ec;
        return fs::is_regular_file(legacy, ec);
    }
};

Result<TemplateLocation> projectTemplateAt(const EngineApp& app, const std::string& name) {
    SUPPLE_TRY(validName(name));
    if (name.front() == '.')
        return Error("chain.name", "a template name cannot start with a dot", "/name",
                     "use a name that starts with a letter or digit");
    auto dir = projectTemplateDir(app);
    if (!dir) return std::move(dir).error();
    return TemplateLocation{*dir / name, *dir / (name + ".json")};
}

// Writes the template into a staging folder, then puts it in place (a
// replaced template is removed only once the new one is there).
Status publishProjectTemplate(EngineApp& app, const TemplateLocation& at, bool replace) {
    if ((at.folderExists() || at.legacyExists()) && !replace)
        return Error("template.exists",
                     "a project template named " + at.folder.filename().string() + " exists",
                     "/name", "choose another name or set replace: true");
    static std::atomic<unsigned> serial{0};
    const std::string tag = std::to_string(::getpid()) + "-" + std::to_string(serial.fetch_add(1));
    const fs::path staging = at.folder.parent_path() / ".staging";
    const fs::path fresh = staging / ("new-" + tag), old = staging / ("old-" + tag);
    std::error_code ec;
    fs::create_directories(staging, ec);
    if (auto written = app.writeTemplate(fresh); !written) {
        fs::remove_all(fresh, ec);
        return written;
    }
    const bool moved = fs::exists(at.folder, ec);
    if (moved) fs::rename(at.folder, old, ec);
    if (!ec) fs::rename(fresh, at.folder, ec);
    if (ec) {
        const std::string why = ec.message();
        std::error_code ignored;
        if (moved && !fs::exists(at.folder, ignored)) fs::rename(old, at.folder, ignored);
        fs::remove_all(fresh, ignored);
        return Error("template.write", "cannot save " + at.folder.string() + ": " + why, "/name",
                     "check the templates folder's permissions and free space");
    }
    fs::remove_all(old, ec);
    fs::remove(at.legacy, ec);
    fs::remove(staging, ec);  // only when empty
    return {};
}

struct NameArgs {
    std::string name;
};
struct ProjectTemplateSaveArgs {
    std::string name;
    bool replace = false;
};
struct PathResult {
    std::string path;
};
struct ProjectTemplateLoadResult {
    std::string name;
};
struct ProjectTemplateEntry {
    std::string name, path;
};
struct ProjectTemplateListResult {
    std::vector<ProjectTemplateEntry> templates;
};

void registerProjectTemplateCommands(cmd::Registry& registry) {
    {
        cmd::Command<ProjectTemplateSaveArgs, PathResult> c;
        c.name = "project.template.save";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save the whole project as a named project template.";
        c.description =
            "Writes the project as it is now (tracks, FX and their states, routing, items, "
            "settings; not the history) to a folder of its own in the user's templates "
            "directory, with a copy of every audio file it uses. The project keeps its own file. "
            "replace: true replaces a template of that name.";
        c.tags = {"project", "presets"};
        c.args = schema::object<ProjectTemplateSaveArgs>("project_template_save_args")
                     .field("name", &ProjectTemplateSaveArgs::name, schema::string().minLength(1))
                     .field("replace", &ProjectTemplateSaveArgs::replace, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::object<PathResult>("project_template_save_result")
                       .field("path", &PathResult::path, schema::string())
                       .build();
        c.errors = {{"template.exists", "the name already exists",
                     "choose another name or set replace: true"},
                    {"media.missing", "an audio file the project uses is gone",
                     "restore it, or remove the items that use it"}};
        c.examples = {
            {"Save the band setup", j(R"({"name":"Band"})"), j(R"({"path":"<any>"})"), "song"}};
        c.run = [](const ProjectTemplateSaveArgs& a, cmd::Context& ctx) -> Result<PathResult> {
            auto& app = ctx.service<EngineApp>();
            auto at = projectTemplateAt(app, a.name);
            if (!at) return std::move(at).error();
            SUPPLE_TRY(publishProjectTemplate(app, *at, a.replace));
            return PathResult{at->folder.string()};
        };
        registry.add(std::move(c));
    }
    {
        cmd::Command<NameArgs, ProjectTemplateLoadResult> c;
        c.name = "project.template.load";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Start a new, unsaved project from a project template.";
        c.description =
            "Replaces the current project and its history, like project.new: unsaved changes are "
            "lost, so save first. The new project has no file until project.save {path} and "
            "gets its own copy of the template's audio; the template itself is never changed.";
        c.tags = {"project", "presets"};
        c.args = schema::object<NameArgs>("project_template_load_args")
                     .field("name", &NameArgs::name, schema::string().minLength(1))
                     .build();
        c.result = schema::object<ProjectTemplateLoadResult>("project_template_load_result")
                       .field("name", &ProjectTemplateLoadResult::name, schema::string())
                       .build();
        c.examples = {{"Start from the band setup", j(R"({"name":"Band"})"),
                       j(R"({"name":"<any>"})"), "song_with_project_template"}};
        c.run = [](const NameArgs& a, cmd::Context& ctx) -> Result<ProjectTemplateLoadResult> {
            auto& app = ctx.service<EngineApp>();
            auto at = projectTemplateAt(app, a.name);
            if (!at) return std::move(at).error();
            if (!at->folderExists() && !at->legacyExists())
                return Error("template.unknown", "no project template " + a.name, "/name",
                             "use project.template.list to find a saved name");
            std::vector<schema::Warning> warnings;
            SUPPLE_TRY(app.openTemplate(at->folderExists() ? at->folder : at->legacy, &warnings));
            for (const auto& w : warnings) ctx.warn(w.code, w.message, w.path);
            return ProjectTemplateLoadResult{app.head().project->name};
        };
        registry.add(std::move(c));
    }
    {
        cmd::Command<NoArgs, ProjectTemplateListResult> c;
        c.name = "project.template.list";
        c.kind = cmd::Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List saved project templates by name and path.";
        c.tags = {"project", "presets"};
        c.args = schema::object<NoArgs>("project_template_list_args").build();
        auto entry = schema::object<ProjectTemplateEntry>("project_template_entry")
                         .field("name", &ProjectTemplateEntry::name, schema::string())
                         .field("path", &ProjectTemplateEntry::path, schema::string())
                         .build();
        c.result =
            schema::object<ProjectTemplateListResult>("project_template_list_result")
                .field("templates", &ProjectTemplateListResult::templates, schema::arrayOf(entry))
                .build();
        c.examples = {{"None yet", j("{}"), j(R"({"templates":[]})"), "empty"}};
        c.run = [](const NoArgs&, cmd::Context& ctx) -> Result<ProjectTemplateListResult> {
            auto dir = projectTemplateDir(ctx.service<EngineApp>());
            if (!dir) return std::move(dir).error();
            ProjectTemplateListResult result;
            std::error_code ec;
            if (!fs::exists(*dir, ec)) return result;
            std::set<std::string> folders;
            for (const auto& e : fs::directory_iterator(*dir, ec)) {
                const std::string name = e.path().filename().string();
                std::error_code kind;
                if (name.front() == '.') continue;  // .staging
                if (e.is_directory(kind) && fs::is_regular_file(e.path() / "project.json", kind)) {
                    folders.insert(name);
                    result.templates.push_back({name, e.path().string()});
                }
            }
            for (const auto& e : fs::directory_iterator(*dir, ec)) {  // older single files
                std::error_code kind;
                const std::string stem = e.path().stem().string();
                if (e.is_regular_file(kind) && e.path().extension() == ".json" &&
                    stem.front() != '.' && !folders.contains(stem))
                    result.templates.push_back({stem, e.path().string()});
            }
            std::sort(result.templates.begin(), result.templates.end(),
                      [](const auto& x, const auto& y) { return x.name < y.name; });
            return result;
        };
        registry.add(std::move(c));
    }
    {
        cmd::Command<NameArgs, PathResult> c;
        c.name = "project.template.delete";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Delete a saved project template by name.";
        c.tags = {"project", "presets"};
        c.args = schema::object<NameArgs>("project_template_delete_args")
                     .field("name", &NameArgs::name, schema::string().minLength(1))
                     .build();
        c.result = schema::object<PathResult>("project_template_delete_result")
                       .field("path", &PathResult::path, schema::string())
                       .build();
        c.errors = {{"template.unknown", "no template has that name",
                     "use a name from project.template.list"}};
        c.examples = {{"Delete the band setup", j(R"({"name":"Band"})"), j(R"({"path":"<any>"})"),
                       "song_with_project_template"}};
        c.run = [](const NameArgs& a, cmd::Context& ctx) -> Result<PathResult> {
            auto at = projectTemplateAt(ctx.service<EngineApp>(), a.name);
            if (!at) return std::move(at).error();
            if (!at->folderExists()) {
                SUPPLE_TRY(removeNamed(at->legacy, "template.unknown", "project template"));
                return PathResult{at->legacy.string()};
            }
            std::error_code ec;
            fs::remove_all(at->folder, ec);
            if (ec)
                return Error("template.unknown",
                             "cannot delete " + at->folder.string() + ": " + ec.message(), "/name",
                             "check the templates folder's permissions");
            fs::remove(at->legacy, ec);
            return PathResult{at->folder.string()};
        };
        registry.add(std::move(c));
    }
}

}  // namespace

void registerChainCommands(cmd::Registry& registry) {
    {
        cmd::Command<SaveArgs, SaveResult> c;
        c.name = "fx.chain.save";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Save an ordered FX chain as a named preset.";
        c.description =
            "Save main (default) or input FX to the user's chains directory. Preserves semantic "
            "patches or opaque base64 state. Refuses empty chains; replace: true explicitly "
            "overwrites an existing name.";
        c.tags = {"fx", "presets"};
        c.args = schema::object<SaveArgs>("fx_chain_save_args")
                     .field("track", &SaveArgs::track,
                            schema::string().pattern("^tr_[0-9]+$", "track id"))
                     .field("name", &SaveArgs::name, schema::string().minLength(1))
                     .field("chain", &SaveArgs::chain,
                            schema::string().pattern("^(main|input)$", "main or input"))
                     .defaultValue(std::string("main"))
                     .field("replace", &SaveArgs::replace, schema::boolean())
                     .defaultValue(false)
                     .build();
        c.result = schema::object<SaveResult>("fx_chain_save_result")
                       .field("path", &SaveResult::path, schema::string())
                       .field("effects", &SaveResult::effects, schema::integer().min(0))
                       .build();
        c.errors = {{"chain.name", "invalid preset name", "use a name without path separators"},
                    {"chain.empty", "the chain has no effects", "add effects before saving"},
                    {"chain.exists", "the name already exists",
                     "choose another name or set replace: true"}};
        c.examples = {{"Save the bass chain", j(R"({"track":"tr_1","name":"Bass"})"),
                       j(R"({"path":"<any>","effects":2})"), "song_with_chain"}};
        c.run = [](const SaveArgs& a, cmd::Context& ctx) -> Result<SaveResult> {
            auto& app = ctx.service<EngineApp>();
            auto path = namedPath(app, a.name);
            if (!path) return std::move(path).error();
            const auto s = app.view();
            auto track = trackOf(s, a.track);
            if (!track) return std::move(track).error();
            const auto& strip = *s.tracks.find(*track);
            const auto& chain = a.chain == "input" ? strip.inputChain : strip.chain;
            if (chain.empty())
                return Error("chain.empty", "the " + a.chain + " chain has no effects", "/chain",
                             "add effects before saving");
            Document doc{std::string(kSchema), a.name, {}};
            auto effects = encodeEffects(app, s, chain);
            if (!effects) return std::move(effects).error();
            doc.effects = std::move(*effects);
            const auto text = json::writeCanonical(documentSchema().encode(doc)) + "\n";
            if (text.size() > kMaxFile)
                return Error("chain.size", "preset exceeds the 64 MiB file limit", "/effects",
                             "save fewer effects or smaller plugin states");
            SUPPLE_TRY(writePreset(*path, text, a.replace));
            return SaveResult{path->string(), static_cast<std::int64_t>(chain.size())};
        };
        registry.add(std::move(c));
    }
    {
        cmd::Command<LoadArgs, LoadResult> c;
        c.name = "fx.chain.load";
        c.kind = cmd::Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Append or replace a track's FX chain from a preset in one undo step.";
        c.description =
            "Give exactly one of name (in the user's chains directory) or path (relative to the "
            "work directory). mode defaults to append; replace removes the target chain, its "
            "envelopes and key routes. Validates every plugin and state before changing the model.";
        c.tags = {"fx", "presets"};
        c.args = schema::object<LoadArgs>("fx_chain_load_args")
                     .field("track", &LoadArgs::track,
                            schema::string().pattern("^tr_[0-9]+$", "track id"))
                     .field("name", &LoadArgs::name, schema::string().minLength(1))
                     .field("path", &LoadArgs::path, schema::string().minLength(1))
                     .field("chain", &LoadArgs::chain,
                            schema::string().pattern("^(main|input)$", "main or input"))
                     .defaultValue(std::string("main"))
                     .field("mode", &LoadArgs::mode,
                            schema::string().pattern("^(append|replace)$", "append or replace"))
                     .defaultValue(std::string("append"))
                     .build();
        c.result = schema::object<LoadResult>("fx_chain_load_result")
                       .field("effects", &LoadResult::effects, schema::arrayOf(schema::string()))
                       .build();
        c.errors = {
            {"chain.source", "name/path missing or both given", "give exactly one of name or path"},
            {"chain.plugin_missing", "a saved plugin is not installed",
             "install the named plugin or choose another preset"}};
        c.examples = {{"Load a saved bass chain",
                       j(R"({"track":"tr_1","name":"Bass","mode":"replace"})"),
                       j(R"({"effects":["<any>","<any>"]})"), "song_with_saved_chain"}};
        c.run = [](const LoadArgs& a, cmd::Context& ctx) -> Result<LoadResult> {
            auto& app = ctx.service<EngineApp>();
            if (a.name.has_value() == a.path.has_value())
                return Error("chain.source", "give exactly one of name or path", "/name",
                             "use fx.chain.list to find a saved name");
            auto path = a.name ? namedPath(app, *a.name) : Result<fs::path>(app.resolve(*a.path));
            if (!path) return std::move(path).error();
            auto doc = readDocument(*path);
            if (!doc) return std::move(doc).error();
            const auto s = app.view();
            auto target = trackOf(s, a.track);
            if (!target) return std::move(target).error();
            auto ready = prepareEffects(app, doc->effects, "/effects");
            if (!ready) return std::move(ready).error();
            std::vector<Prepared>& prepared = *ready;
            const auto* strip = s.tracks.find(*target);
            auto chain = a.chain == "input" ? strip->inputChain : strip->chain;
            if (a.mode == "replace") {
                for (Id id : chain) {
                    auto args = json::Value::object();
                    args["fx"] = id.str();
                    auto removed = invoke("fx.remove", std::move(args), ctx);
                    if (!removed) return std::move(removed).error();
                }
                chain.clear();
            }
            LoadResult result;
            result.effects = installEffects(app, prepared, chain);
            app.txn().modify<model::Track>(*target, [&](model::Track& track) {
                (a.chain == "input" ? track.inputChain : track.chain) = chain;
            });
            return result;
        };
        registry.add(std::move(c));
    }
    {
        cmd::Command<NoArgs, ListResult> c;
        c.name = "fx.chain.list";
        c.kind = cmd::Kind::Query;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "List saved FX chain presets by name, path and effect count.";
        c.description =
            "Lists valid supple.fx-chain/1.0 files, sorted by name and path. Invalid documents are "
            "skipped with warnings. See fx.chain.save and fx.chain.load.";
        c.tags = {"fx", "presets"};
        c.args = schema::object<NoArgs>("fx_chain_list_args").build();
        const auto entrySchema =
            schema::object<ChainEntry>("fx_chain_entry")
                .field("name", &ChainEntry::name, schema::string())
                .field("path", &ChainEntry::path, schema::string())
                .field("effects", &ChainEntry::effects, schema::integer().min(0))
                .build();
        c.result = schema::object<ListResult>("fx_chain_list_result")
                       .field("chains", &ListResult::chains, schema::arrayOf(entrySchema))
                       .build();
        c.examples = {{"List presets", j("{}"), j(R"({"chains":[]})"), "empty"}};
        c.run = [](const NoArgs&, cmd::Context& ctx) -> Result<ListResult> {
            auto dir = chainDir(ctx.service<EngineApp>());
            if (!dir) return std::move(dir).error();
            ListResult result;
            std::error_code ec;
            if (fs::exists(*dir, ec)) {
                std::vector<ChainEntry> rows;
                fs::directory_iterator it(*dir, ec), end;
                while (!ec && it != end) {
                    const auto entry = *it;
                    it.increment(ec);
                    std::error_code kindError;
                    if (!entry.is_regular_file(kindError) || entry.path().extension() != ".json")
                        continue;
                    auto doc = readDocument(entry.path());
                    if (!doc) {
                        ctx.warn("chain.invalid",
                                 entry.path().string() + ": " + doc.error().message);
                        continue;
                    }
                    rows.push_back({doc->name, entry.path().string(),
                                    static_cast<std::int64_t>(doc->effects.size())});
                }
                std::sort(rows.begin(), rows.end(), [](const auto& a, const auto& b) {
                    return std::tie(a.name, a.path) < std::tie(b.name, b.path);
                });
                result.chains = std::move(rows);
            }
            if (ec)
                return Error("chain.directory",
                             "cannot list " + dir->string() + ": " + ec.message(), {},
                             "check the directory permissions");
            return result;
        };
        registry.add(std::move(c));
    }
    {
        cmd::Command<NameArgs, PathResult> c;
        c.name = "fx.chain.delete";
        c.kind = cmd::Kind::Action;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Delete a saved FX chain preset by name.";
        c.description =
            "Removes the preset file from the user's chains directory. Tracks that loaded it keep "
            "their effects.";
        c.tags = {"fx", "presets"};
        c.args = schema::object<NameArgs>("fx_chain_delete_args")
                     .field("name", &NameArgs::name, schema::string().minLength(1))
                     .build();
        c.result = schema::object<PathResult>("fx_chain_delete_result")
                       .field("path", &PathResult::path, schema::string())
                       .build();
        c.errors = {{"chain.unknown", "no preset has that name", "use a name from fx.chain.list"}};
        c.examples = {{"Delete the bass chain", j(R"({"name":"Bass"})"), j(R"({"path":"<any>"})"),
                       "song_with_saved_chain"}};
        c.run = [](const NameArgs& a, cmd::Context& ctx) -> Result<PathResult> {
            auto path = namedPath(ctx.service<EngineApp>(), a.name);
            if (!path) return std::move(path).error();
            SUPPLE_TRY(removeNamed(*path, "chain.unknown", "chain preset"));
            return PathResult{path->string()};
        };
        registry.add(std::move(c));
    }
    {
        cmd::Command<CopyArgs, CopyResult> c;
        c.name = "fx.copy";
        c.kind = cmd::Kind::Edit;
        c.exposure = cmd::Exposure::McpTool;
        c.summary = "Copy a plugin and its current settings into a track's FX chain.";
        c.description =
            "Adds a new instance of fx's plugin to track's main (default) or input chain, at index "
            "(default: the end), with the source's options (label, bypass, offline, wet, "
            "isolation, oversampling, parallel, pdc) and its settings as they are now: the "
            "running plugin's own state when the engine has one (editor edits included), else the "
            "model's patch or state. Not copied: envelopes, key routes into the source, and, "
            "when the track differs, channel pins. The source stays where it is; "
            "use fx.move to move it. One undo step.";
        c.tags = {"fx"};
        c.args = schema::object<CopyArgs>("fx_copy_args")
                     .field("fx", &CopyArgs::fx, schema::string().pattern("^fx_[0-9]+$", "fx id"))
                     .field("track", &CopyArgs::track,
                            schema::string().pattern("^tr_[0-9]+$", "track id"))
                     .field("index", &CopyArgs::index, schema::integer().min(0))
                     .field("chain", &CopyArgs::chain,
                            schema::string().pattern("^(main|input)$", "main or input"))
                     .defaultValue(std::string("main"))
                     .build();
        c.result = schema::object<CopyResult>("fx_copy_result")
                       .field("fx", &CopyResult::fx, schema::string())
                       .field("chain", &CopyResult::chain, schema::arrayOf(schema::string()))
                       .build();
        c.errors = {
            {"fx.unknown", "no such plugin instance",
             "use an fx id returned by fx.add, fx.copy or fx.chain.load"},
            {"fx.index", "index is past the end of the chain", "use 0 to the chain's size"}};
        c.examples = {{"Duplicate a plugin at the end of its own chain",
                       j(R"({"fx":"fx_1","track":"tr_1"})"),
                       j(R"({"fx":"<any>","chain":["fx_1","fx_2","<any>"]})"), "song_with_fx"}};
        c.run = [](const CopyArgs& a, cmd::Context& ctx) -> Result<CopyResult> {
            auto& app = ctx.service<EngineApp>();
            const auto s = app.view();
            auto source = Id::parse(a.fx);
            const model::FxInstance* original =
                source && source->kind == model::Kind::Fx ? s.fx.find(*source) : nullptr;
            if (!original)
                return Error("fx.unknown", "no plugin instance " + a.fx, "/fx",
                             "use an fx id returned by fx.add, fx.copy or fx.chain.load");
            auto target = trackOf(s, a.track);
            if (!target) return std::move(target).error();
            const model::Track* strip = s.tracks.find(*target);
            const bool input = a.chain == "input";
            std::vector<Id> chain = input ? strip->inputChain : strip->chain;
            if (a.index && static_cast<std::size_t>(*a.index) > chain.size())
                return Error("fx.index", "index is past the end of the destination chain", "/index",
                             "use an index from 0 to " + std::to_string(chain.size()));
            Prepared copy{*original, {}};
            // The pins name channels of the source's own track.
            const auto home = model::trackOfFx(s, *source);
            if (!home || *home != *target) copy.fx.pins = {};
            SUPPLE_TRY(captureLiveState(app, *source, copy.fx, copy.blob));
            if (copy.blob) app.txn().putBlob(*copy.fx.state, copy.blob);
            const Id id = app.txn().create(std::move(copy.fx));
            chain.insert(
                chain.begin() + static_cast<std::ptrdiff_t>(
                                    a.index ? *a.index : static_cast<std::int64_t>(chain.size())),
                id);
            app.txn().modify<model::Track>(*target, [&](model::Track& track) {
                (input ? track.inputChain : track.chain) = chain;
            });
            CopyResult result{id.str(), {}};
            for (Id f : chain) result.chain.push_back(f.str());
            return result;
        };
        registry.add(std::move(c));
    }
    registerTemplateCommands(registry);
    registerProjectTemplateCommands(registry);
}
}  // namespace supple::engine
