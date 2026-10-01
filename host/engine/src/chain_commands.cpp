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
#include <tuple>

#include "supple/base/files.h"
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
        if ((fx->oversampling & (fx->oversampling - 1)) != 0 ||
            fx->patch.has_value() == fx->state.has_value())
            return Error("chain.invalid", "effect needs one state and valid oversampling", at,
                         "use a semantic patch or base64 state; oversampling is 1, 2, 4 or 8");
    }
    return decoded;
}

// The model's blob references are content hashes. SHA-256 here hashes an
// opaque state snapshot; it never runs on the audio path.
std::string stateHash(std::span<const std::uint8_t> bytes) {
    static constexpr std::uint32_t constants[] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const std::size_t blocks = (bytes.size() + 9 + 63) / 64;
    for (std::size_t block = 0; block < blocks; ++block) {
        std::array<std::uint32_t, 64> w{};
        for (std::size_t i = 0; i < 64; ++i) {
            const std::size_t at = block * 64 + i;
            std::uint8_t b = 0;
            if (at < bytes.size())
                b = bytes[at];
            else if (at == bytes.size())
                b = 0x80;
            else if (at >= blocks * 64 - 8)
                b = static_cast<std::uint8_t>((static_cast<std::uint64_t>(bytes.size()) * 8) >>
                                              ((blocks * 64 - 1 - at) * 8));
            w[i / 4] |= static_cast<std::uint32_t>(b) << (24 - (i % 4) * 8);
        }
        for (std::size_t i = 16; i < 64; ++i) {
            const auto x = w[i - 15], y = w[i - 2];
            w[i] = w[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + w[i - 7] +
                   (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
        }
        auto v = h;
        for (std::size_t i = 0; i < 64; ++i) {
            const auto t1 = v[7] +
                            (std::rotr(v[4], 6) ^ std::rotr(v[4], 11) ^ std::rotr(v[4], 25)) +
                            ((v[4] & v[5]) ^ (~v[4] & v[6])) + constants[i] + w[i];
            const auto t2 = (std::rotr(v[0], 2) ^ std::rotr(v[0], 13) ^ std::rotr(v[0], 22)) +
                            ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            for (std::size_t k = 7; k > 0; --k) v[k] = v[k - 1];
            v[4] += t1;
            v[0] = t1 + t2;
        }
        for (std::size_t i = 0; i < 8; ++i) h[i] += v[i];
    }
    std::string out = "sha256:";
    constexpr char hex[] = "0123456789abcdef";
    for (auto word : h)
        for (int shift = 28; shift >= 0; shift -= 4) out += hex[(word >> shift) & 15];
    return out;
}

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

Result<json::Value> invoke(const char* name, json::Value args, cmd::Context& ctx) {
    return ctx.service<const cmd::Registry>().find(name)->invoke(args, ctx);
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
                static constexpr const char* isolation[] = {"inherit", "in_process",
                                                            "shared_bridge", "dedicated"};
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
                            return Error("engine.blob_missing", "missing state for " + id.str(),
                                         "/effects", "capture the plugin state again");
                        bytes = blob->second->data;
                    } else {
                        auto probe = host::Instance::create(*info, app.options().identity);
                        if (!probe) return std::move(probe).error();
                        for (const auto& [param, v] : fx.params)
                            SUPPLE_TRY((*probe)->setParam(param, v));
                        auto state = (*probe)->saveState();
                        if (!state) return std::move(state).error();
                        bytes = std::move(*state);
                    }
                    value.asObject().erase("patch");
                    value["state"] = base64Encode(bytes);
                }
                doc.effects.push_back(std::move(value));
            }
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
            struct Prepared {
                model::FxInstance fx;
                std::shared_ptr<const model::Blob> blob;
            };
            std::vector<Prepared> prepared;
            for (std::size_t i = 0; i < doc->effects.size(); ++i) {
                const std::string at = "/effects/" + std::to_string(i);
                auto fx = model::schemaOf<model::FxInstance>().decode(doc->effects[i]);
                if (!fx) return std::move(fx).error().at(at);
                if ((fx->oversampling & (fx->oversampling - 1)) != 0)
                    return Error("chain.invalid", "oversampling must be 1, 2, 4 or 8",
                                 at + "/oversampling");
                auto info = app.findPlugin(fx->plugin.id);
                if (!info || info->id != fx->plugin.id || info->format != fx->plugin.format)
                    return Error("chain.plugin_missing",
                                 "plugin " + fx->plugin.name + " (" + fx->plugin.format + ":" +
                                     fx->plugin.id + ") is not installed",
                                 at + "/plugin", "install this plugin or choose another preset");
                Prepared p{*fx, {}};
                if (info->semantic) {
                    if (!fx->patch || fx->state)
                        return Error("chain.invalid", "a semantic effect needs a patch",
                                     at + "/patch");
                    auto patch = app.mergedPatch(*fx, *fx->patch, true);
                    if (!patch) return std::move(patch).error().at(at + "/patch");
                    p.fx.patch = std::move(*patch);
                } else {
                    if (!fx->state || fx->patch)
                        return Error("chain.invalid", "a plain effect needs base64 state",
                                     at + "/state");
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
            for (auto& p : prepared) {
                if (p.blob) app.txn().putBlob(*p.fx.state, p.blob);
                const Id id = app.txn().create(std::move(p.fx));
                chain.push_back(id);
                result.effects.push_back(id.str());
            }
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
}
}  // namespace supple::engine
