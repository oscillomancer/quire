#include "supple/patch/patch.h"

#include <cmath>
#include <map>

namespace supple::patch {

namespace {

constexpr std::string_view kEnvelopeKeys[] = {"name", "author", "tags", "notes", "seed"};

bool isEnvelopeKey(std::string_view key) {
    for (auto k : kEnvelopeKeys)
        if (k == key) return true;
    return false;
}

Error collect(std::vector<Error> errors, std::string_view what) {
    if (errors.size() == 1) return std::move(errors.front());
    Error e("schema.invalid", std::to_string(errors.size()) + " problems in " + std::string(what));
    e.details = std::move(errors);
    return e;
}

bool validUtf8Length(const std::string& s, std::size_t maxChars) {
    std::size_t chars = 0;
    for (unsigned char c : s)
        if ((c & 0xc0) != 0x80) ++chars;
    return chars <= maxChars;
}

bool validUtf8(std::span<const std::uint8_t> bytes) {
    // The JSON parser validates UTF-8 fully; this is only the cheap framing check.
    for (std::size_t i = 0; i < bytes.size();) {
        const std::uint8_t c = bytes[i];
        std::size_t n = c < 0x80           ? 1
                        : (c >> 5) == 0x6  ? 2
                        : (c >> 4) == 0xe  ? 3
                        : (c >> 3) == 0x1e ? 4
                                           : 0;
        if (n == 0 || i + n > bytes.size()) return false;
        for (std::size_t j = 1; j < n; ++j)
            if ((bytes[i + j] & 0xc0) != 0x80) return false;
        i += n;
    }
    return true;
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

std::uint32_t get32(std::span<const std::uint8_t> b, std::size_t at) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i)
        v |= static_cast<std::uint32_t>(b[at + static_cast<std::size_t>(i)]) << (8 * i);
    return v;
}

json::Value paramJsonSchema(const params::ParamSpec& p) {
    json::Value s = json::Value::object();
    if (!p.description.empty()) s["description"] = std::string(p.description);
    switch (p.type) {
        case params::Type::Bool:
            s["type"] = "boolean";
            break;
        case params::Type::Enum: {
            s["type"] = "string";
            json::Value e = json::Value::array();
            for (const auto& c : p.choices) e.push_back(std::string(c.key));
            s["enum"] = std::move(e);
            break;
        }
        case params::Type::Int:
        case params::Type::Float: {
            json::Value number = json::Value::object();
            number["type"] = p.type == params::Type::Int ? "integer" : "number";
            number["minimum"] = p.min;
            number["maximum"] = p.max;
            if (p.unit != units::Unit::none) {
                json::Value text = json::Value::object();
                text["type"] = "string";
                text["description"] =
                    "a quantity with a unit, e.g. \"" + params::format(p, p.def) + "\"";
                json::Value any = json::Value::array();
                any.push_back(std::move(number));
                any.push_back(std::move(text));
                s["anyOf"] = std::move(any);
                s["x-unit"] = std::string(units::unitName(p.unit));
            } else {
                for (const auto& m : number.asObject()) s[m.key] = m.value;
            }
            break;
        }
    }
    s["default"] = params::toJson(p, p.def);
    return s;
}

}  // namespace

// --- Codec ---------------------------------------------------------------------

Codec::Codec(std::string plugin, int major, int minor, bool hasSeed,
             std::vector<Migration> migrations)
    : id_{"supple", std::move(plugin) + "-patch", major, minor},
      schemaId_(schema::formatDocumentId(id_)),
      hasSeed_(hasSeed),
      migrations_(std::move(migrations)) {}

Result<Codec::Split> Codec::split(const json::Value& document,
                                  std::vector<Warning>* warnings) const {
    if (!document.isObject())
        return Error("patch.not_object", "a patch document must be a JSON object", "",
                     "start with {\"schema\": \"" + schemaId_ + "\", …}");
    const json::Value* sid = document.find("schema");
    if (!sid || !sid->isString())
        return Error("patch.missing_schema", "the patch has no \"schema\" member", "/schema",
                     "add \"schema\": \"" + schemaId_ + "\" as the first member");
    auto parsed = schema::parseDocumentId(sid->asString());
    if (!parsed)
        return Error("patch.bad_schema_id", "\"" + sid->asString() + "\" is not a document ID",
                     "/schema", "use \"" + schemaId_ + "\"");
    if (parsed->ns != id_.ns || parsed->type != id_.type)
        return Error("patch.wrong_type",
                     "this is a \"" + sid->asString() + "\" document, not a " + id_.type, "/schema",
                     "this plugin reads \"" + schemaId_ + "\"");
    if (parsed->major > id_.major)
        return Error(
            "patch.too_new",
            "the patch is from a newer major version (" + std::to_string(parsed->major) + ")",
            "/schema", "update the plugin; it reads major version " + std::to_string(id_.major));

    json::Value doc = document;
    for (int major = parsed->major; major < id_.major; ++major) {
        const auto index = static_cast<std::size_t>(major - 1);
        if (major < 1 || index >= migrations_.size() || !migrations_[index])
            return Error("patch.migration_failed",
                         "no migration from major " + std::to_string(major), "/schema",
                         "this is a bug in the plugin");
        auto next = migrations_[index](std::move(doc));
        if (!next) {
            Error e("patch.migration_failed",
                    "migrating from major " + std::to_string(major) + " failed", "/schema");
            e.details.push_back(std::move(next).error());
            return e;
        }
        doc = std::move(*next);
    }

    Split out;
    const bool newerMinor = parsed->major == id_.major && parsed->minor > id_.minor;
    if (newerMinor) out.options.unknownMembers = schema::UnknownMembers::Ignore;

    std::vector<Error> errors;
    auto str = [&](std::string_view key, std::size_t maxChars) -> std::optional<std::string> {
        const json::Value* v = doc.find(key);
        if (!v) return std::nullopt;
        if (!v->isString()) {
            errors.emplace_back("schema.type", "\"" + std::string(key) + "\" must be a string",
                                "/" + std::string(key),
                                "e.g. \"" + std::string(key) + "\": \"text\"");
            return std::nullopt;
        }
        if (maxChars && !validUtf8Length(v->asString(), maxChars))
            errors.emplace_back("schema.length", "\"" + std::string(key) + "\" is too long",
                                "/" + std::string(key),
                                "at most " + std::to_string(maxChars) + " characters");
        return v->asString();
    };
    out.meta.name = str("name", 128);
    out.meta.author = str("author", 128);
    out.meta.notes = str("notes", 0);
    if (const json::Value* tags = doc.find("tags")) {
        if (!tags->isArray()) {
            errors.emplace_back("schema.type", "\"tags\" must be an array of strings", "/tags",
                                "e.g. \"tags\": [\"pad\", \"warm\"]");
        } else {
            for (std::size_t i = 0; i < tags->size(); ++i) {
                const json::Value& t = tags->at(i);
                if (t.isString())
                    out.meta.tags.push_back(t.asString());
                else
                    errors.emplace_back("schema.type", "tags must be strings",
                                        "/tags/" + std::to_string(i), "e.g. \"pad\"");
            }
        }
    }
    if (const json::Value* seed = doc.find("seed")) {
        if (!hasSeed_) {
            if (!newerMinor)
                errors.emplace_back("schema.unknown_member",
                                    "this plugin has no randomness, so no \"seed\"", "/seed",
                                    "remove \"seed\"");
        } else if (!seed->isInt() || seed->asInt() < 0 || seed->asInt() > 0xffffffffLL) {
            errors.emplace_back("schema.out_of_range", "\"seed\" must be an integer 0..4294967295",
                                "/seed", "e.g. \"seed\": 1234");
        } else {
            out.meta.seed = static_cast<std::uint32_t>(seed->asInt());
        }
    }
    if (!errors.empty()) return collect(std::move(errors), "the patch envelope");
    // A plugin with randomness always has a seed (absent means 0), so that
    // documents with and without an explicit "seed": 0 are the same patch.
    if (hasSeed_ && !out.meta.seed) out.meta.seed = 0;

    out.body = json::Value::object();
    for (const auto& m : doc.asObject())
        if (m.key != "schema" && !isEnvelopeKey(m.key)) out.body[m.key] = m.value;
    if (newerMinor && warnings)
        warnings->push_back(
            {"patch.newer_minor",
             "the patch was written by a newer minor version; unknown members are ignored",
             "/schema"});
    return out;
}

json::Value Codec::join(const Envelope& meta, const json::Value& body) const {
    json::Value doc = json::Value::object();
    doc["schema"] = schemaId_;
    if (meta.name) doc["name"] = *meta.name;
    if (meta.author) doc["author"] = *meta.author;
    if (!meta.tags.empty()) {
        json::Value tags = json::Value::array();
        for (const auto& t : meta.tags) tags.push_back(t);
        doc["tags"] = std::move(tags);
    }
    if (meta.notes) doc["notes"] = *meta.notes;
    if (hasSeed_) doc["seed"] = static_cast<long long>(meta.seed.value_or(0));
    if (body.isObject())
        for (const auto& m : body.asObject())
            if (m.key != "schema" && !isEnvelopeKey(m.key)) doc[m.key] = m.value;
    return doc;
}

json::Value Codec::envelopeProperties() const {
    json::Value p = json::Value::object();
    p["schema"]["const"] = schemaId_;
    p["name"]["type"] = "string";
    p["name"]["maxLength"] = 128;
    p["author"]["type"] = "string";
    p["tags"]["type"] = "array";
    p["tags"]["items"]["type"] = "string";
    p["notes"]["type"] = "string";
    p["notes"]["description"] = "Free text; JSON has no comments.";
    if (hasSeed_) {
        p["seed"]["type"] = "integer";
        p["seed"]["minimum"] = 0;
        p["seed"]["maximum"] = 4294967295LL;
        p["seed"]["description"] = "All randomness derives from this seed.";
    }
    return p;
}

Status Codec::checkMergePatch(const json::Value& mergePatch) const {
    if (!mergePatch.isObject())
        return Error("patch.merge_not_object", "a merge patch must be a JSON object", "",
                     "send only the members to change, e.g. {\"params\": {\"mix\": 50}}");
    if (const json::Value* s = mergePatch.find("schema");
        s && !(s->isString() && s->asString() == schemaId_))
        return Error("patch.wrong_type", "the merge patch names another schema", "/schema",
                     "omit \"schema\" or use \"" + schemaId_ + "\"");
    return okStatus();
}

// --- FlatPatchType -------------------------------------------------------------

FlatPatchType::FlatPatchType(std::string plugin, int major, int minor, params::Table table,
                             bool hasSeed, std::vector<Migration> migrations)
    : codec_(std::move(plugin), major, minor, hasSeed, std::move(migrations)), table_(table) {}

Patch<FlatBody> FlatPatchType::defaults() const {
    Patch<FlatBody> p;
    p.body.values.reserve(table_.size());
    for (const auto& spec : table_) p.body.values.push_back(spec.def);
    if (codec_.hasSeed()) p.meta.seed = 0;
    return p;
}

Result<Patch<FlatBody>> FlatPatchType::fromJson(const json::Value& document,
                                                std::vector<Warning>* warnings) const {
    auto split = codec_.split(document, warnings);
    if (!split) return std::move(split).error();
    Patch<FlatBody> out = defaults();
    out.meta = std::move(split->meta);
    if (codec_.hasSeed() && !out.meta.seed) out.meta.seed = 0;

    std::vector<Error> errors;
    for (const auto& m : split->body.asObject()) {
        if (m.key == "params") continue;
        if (split->options.unknownMembers == schema::UnknownMembers::Ignore) {
            if (warnings)
                warnings->push_back(
                    {"schema.unknown_member", "ignored member \"" + m.key + "\"", "/" + m.key});
            continue;
        }
        errors.emplace_back(
            "schema.unknown_member", "unknown member \"" + m.key + "\"", "/" + m.key,
            "a flat patch has \"params\" and the envelope (name, author, tags, notes" +
                std::string(codec_.hasSeed() ? ", seed" : "") + ")");
    }
    if (const json::Value* ps = split->body.find("params")) {
        if (!ps->isObject()) {
            errors.emplace_back("schema.type", "\"params\" must be an object of key: value",
                                "/params",
                                "e.g. \"params\": {\"" +
                                    std::string(table_.empty() ? "key" : table_[0].key) + "\": …}");
        } else {
            for (const auto& m : ps->asObject()) {
                const std::string path = "/params/" + json::pointerEscape(m.key);
                const params::ParamSpec* spec = params::findByKey(table_, m.key);
                if (!spec) {
                    if (split->options.unknownMembers == schema::UnknownMembers::Ignore) {
                        if (warnings)
                            warnings->push_back({"params.unknown_key",
                                                 "ignored parameter \"" + m.key + "\"", path});
                        continue;
                    }
                    errors.push_back(params::unknownKey(table_, m.key, path));
                    continue;
                }
                if (spec->flags & params::kReadonly) {
                    errors.emplace_back("params.readonly", "\"" + m.key + "\" is a readonly meter",
                                        path, "remove it; meters are not part of a patch");
                    continue;
                }
                auto v = params::fromJson(*spec, m.value);
                if (!v) {
                    Error e = std::move(v).error();
                    e.path = path;
                    errors.push_back(std::move(e));
                    continue;
                }
                out.body.values[params::indexOf(table_, *spec)] = *v;
            }
        }
    }
    if (!errors.empty()) return collect(std::move(errors), "the patch");
    return out;
}

Result<Patch<FlatBody>> FlatPatchType::parse(std::string_view text,
                                             std::vector<Warning>* warnings) const {
    auto value = json::parse(text);
    if (!value) return std::move(value).error();
    return fromJson(*value, warnings);
}

json::Value FlatPatchType::toJson(const Patch<FlatBody>& p, Detail detail) const {
    json::Value body = json::Value::object();
    json::Value ps = json::Value::object();
    for (std::size_t i = 0; i < table_.size(); ++i) {
        const auto& spec = table_[i];
        if (spec.flags & params::kReadonly) continue;
        const double v = i < p.body.values.size() ? p.body.values[i] : spec.def;
        if (detail == Detail::Changed &&
            params::clampPhysical(spec, v) == params::clampPhysical(spec, spec.def))
            continue;
        ps[spec.key] = params::toJson(spec, v);
    }
    body["params"] = std::move(ps);
    return codec_.join(p.meta, body);
}

std::string FlatPatchType::serialize(const Patch<FlatBody>& p, Detail detail) const {
    return json::writeCanonical(toJson(p, detail));
}

Result<Patch<FlatBody>> FlatPatchType::merge(const Patch<FlatBody>& current,
                                             const json::Value& mergePatch) const {
    SUPPLE_TRY(codec_.checkMergePatch(mergePatch));
    return fromJson(json::mergePatch(toJson(current), mergePatch));
}

json::Value FlatPatchType::diff(const Patch<FlatBody>& a, const Patch<FlatBody>& b) const {
    auto p = json::createMergePatch(toJson(a), toJson(b));
    return p ? std::move(*p) : json::Value::object();
}

json::Value FlatPatchType::jsonSchema() const {
    json::Value s = json::Value::object();
    s["type"] = "object";
    s["title"] = codec_.schemaId();
    json::Value props = codec_.envelopeProperties();
    json::Value params = json::Value::object();
    params["type"] = "object";
    params["additionalProperties"] = false;
    json::Value pp = json::Value::object();
    for (const auto& spec : table_)
        if (!(spec.flags & params::kReadonly)) pp[spec.key] = paramJsonSchema(spec);
    params["properties"] = std::move(pp);
    props["params"] = std::move(params);
    s["properties"] = std::move(props);
    json::Value required = json::Value::array();
    required.push_back("schema");
    s["required"] = std::move(required);
    s["additionalProperties"] = false;
    return s;
}

// --- state framing -------------------------------------------------------------

Result<std::vector<std::uint8_t>> encodeState(std::string_view canonicalJson, std::uint32_t magic) {
    if (canonicalJson.size() > kMaxStateBytes)
        return Error("patch.state.too_large",
                     "state document is " + std::to_string(canonicalJson.size()) + " bytes", "",
                     "states are limited to 16 MiB");
    std::vector<std::uint8_t> out;
    out.reserve(12 + canonicalJson.size());
    put32(out, magic);
    put32(out, kStateFormatVersion);
    put32(out, static_cast<std::uint32_t>(canonicalJson.size()));
    out.insert(out.end(), canonicalJson.begin(), canonicalJson.end());
    return out;
}

bool isFramedState(std::span<const std::uint8_t> bytes, std::uint32_t magic) {
    return bytes.size() >= 8 && get32(bytes, 0) == magic && get32(bytes, 4) == kStateFormatVersion;
}

Result<std::string> decodeState(std::span<const std::uint8_t> bytes, std::uint32_t magic) {
    if (bytes.size() < 12)
        return Error("patch.state.truncated", "state is shorter than its 12-byte header", "",
                     "the session file may be damaged");
    if (get32(bytes, 0) != magic)
        return Error("patch.state.magic", "state belongs to another plugin (magic mismatch)", "",
                     "load this state into the plugin that saved it");
    const std::uint32_t version = get32(bytes, 4);
    if (version != kStateFormatVersion)
        return Error("patch.state.version",
                     "unsupported state format version " + std::to_string(version), "",
                     version == 1 ? "version 1 is the binary form; the plugin must migrate it"
                                  : "update the plugin");
    const std::uint32_t length = get32(bytes, 8);
    if (length > kMaxStateBytes)
        return Error("patch.state.too_large", "state declares " + std::to_string(length) + " bytes",
                     "", "states are limited to 16 MiB");
    if (bytes.size() - 12 < length)
        return Error("patch.state.truncated", "state is truncated", "",
                     "the session file may be damaged");
    if (bytes.size() - 12 > length)
        return Error("patch.state.trailing", "state has trailing bytes after the document", "",
                     "the session file may be damaged");
    auto body = bytes.subspan(12, length);
    if (!validUtf8(body)) return Error("patch.state.utf8", "state document is not valid UTF-8");
    return std::string(reinterpret_cast<const char*>(body.data()), body.size());
}

// --- descriptions --------------------------------------------------------------

std::string quantity(double v, units::Unit u, int digits) {
    return units::formatQuantity(v, u, digits);
}

std::string describeFlat(const FlatPatchType& type, const Patch<FlatBody>& p, Detail detail) {
    // Group by the first key segment ("band2.gain" -> "band2"), in table order.
    std::vector<std::pair<std::string, std::string>> groups;
    const auto table = type.table();
    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto& spec = table[i];
        if (spec.flags & (params::kReadonly | params::kHidden)) continue;
        const double v = i < p.body.values.size() ? p.body.values[i] : spec.def;
        if (detail == Detail::Changed &&
            params::clampPhysical(spec, v) == params::clampPhysical(spec, spec.def))
            continue;
        const auto dot = spec.key.find('.');
        const std::string group =
            dot == std::string_view::npos ? std::string() : std::string(spec.key.substr(0, dot));
        const std::string item = std::string(spec.name) + " " + params::format(spec, v);
        if (!groups.empty() && groups.back().first == group && !group.empty())
            groups.back().second += ", " + item;
        else
            groups.emplace_back(group, item);
    }
    if (groups.empty()) return "default settings";
    std::string out;
    for (const auto& [group, items] : groups) {
        if (!out.empty()) out += "; ";
        out += group.empty() ? items : group + ": " + items;
    }
    return out;
}

}  // namespace supple::patch
