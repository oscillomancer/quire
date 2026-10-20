// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/suite/semantics.h"
#include "supple/base/chars.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <set>

namespace supple::suite {

// --- names ------------------------------------------------------------------------------

std::string snakeCase(std::string_view text) {
    std::string out;
    bool gap = false;
    for (char ch : text) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (std::isalnum(c)) {
            if (gap && !out.empty()) out += '_';
            gap = false;
            out += static_cast<char>(std::tolower(c));
        } else {
            gap = true;
        }
    }
    if (!out.empty() && std::isdigit(static_cast<unsigned char>(out[0]))) out = "p" + out;
    return out;
}

namespace {

bool keyValid(std::string_view key) {
    bool start = true;
    for (char c : key) {
        if (start) {
            if (c < 'a' || c > 'z') return false;
            start = false;
        } else if (c == '.') {
            start = true;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return !key.empty() && !start;
}

// "name", "name_2", ... whichever is not taken yet.
std::string unique(std::string key, std::set<std::string>& taken) {
    if (key.empty()) key = "value";
    std::string candidate = key;
    for (int n = 2; !taken.insert(candidate).second; ++n) candidate = key + "_" + std::to_string(n);
    return candidate;
}

Meaning derive(const Definition& d) {
    Meaning m;
    std::string slug = snakeCase(d.info().name);
    std::replace(slug.begin(), slug.end(), '_', '-');
    m.patchType = "supple." + (slug.empty() ? std::string("plugin") : slug) + "-patch/1.0";
    m.docs = d.info().description;
    std::set<std::string> keys;
    for (size_t i = 0; i < d.params().count(); ++i) {
        const ParamDef& p = d.params().at(i);
        ParamMeaning pm;
        std::string key = snakeCase(p.name);
        if (p.group && *p.group) {
            std::string group;
            for (const char* g = p.group; *g; ++g) group += *g == '/' ? '.' : *g;
            std::string path;
            size_t start = 0;
            while (start <= group.size()) {
                const size_t dot = std::min(group.find('.', start), group.size());
                const std::string part = snakeCase(group.substr(start, dot - start));
                if (!part.empty()) path += part + ".";
                start = dot + 1;
            }
            key = path + key;
        }
        pm.key = unique(key, keys);
        if (p.kind == ParamKind::Choice || p.kind == ParamKind::Toggle) {
            std::set<std::string> choices;
            for (int k = 0; k < p.labelCount && p.kind == ParamKind::Choice; ++k)
                pm.choiceKeys.push_back(unique(snakeCase(p.labels[k]), choices));
        }
        if (p.is(pflag::kBypass)) pm.tags.push_back("role:enable");
        m.params.push_back(std::move(pm));
    }
    std::set<std::string> propertyKeys;
    for (size_t i = 0; i < d.properties().count(); ++i)
        m.properties.push_back({unique(snakeCase(d.properties().at(i).name), propertyKeys)});
    return m;
}

}  // namespace

Meaning meaningOf(const Definition& definition) {
    if (const auto* own = dynamic_cast<const SemanticDefinition*>(&definition))
        return own->meaning();
    return derive(definition);
}

std::string meaningProblems(const Definition& d, const Meaning& m) {
    std::string out;
    auto add = [&](const std::string& what) {
        if (!out.empty()) out += "; ";
        out += what;
    };
    if (m.params.size() != d.params().count()) add("the meaning does not cover every parameter");
    if (m.properties.size() != d.properties().count())
        add("the meaning does not cover every property");
    if (!out.empty()) return out;
    std::set<std::string> keys;
    for (size_t i = 0; i < m.params.size(); ++i) {
        const ParamDef& p = d.params().at(i);
        const ParamMeaning& pm = m.params[i];
        if (!keyValid(pm.key) || !keys.insert(pm.key).second)
            add("parameter '" + std::string(p.name) + "' has a malformed or repeated key '" +
                pm.key + "'");
        if (p.kind == ParamKind::Choice &&
            pm.choiceKeys.size() != static_cast<size_t>(p.labelCount))
            add("parameter '" + std::string(p.name) + "' needs a key for each choice");
        std::set<std::string> choices;
        for (const std::string& c : pm.choiceKeys)
            if (!keyValid(c) || c.find('.') != std::string::npos || !choices.insert(c).second)
                add("parameter '" + std::string(p.name) +
                    "' has a malformed or repeated choice key '" + c + "'");
        // A semantic host keeps a continuous value as its 0..1 position.
        if (p.kind == ParamKind::Continuous && !p.hostTapered())
            add("parameter '" + std::string(p.name) +
                "' reaches hosts as a plain value: mark it normalizedHostValue()");
    }
    std::set<std::string> propertyKeys;
    for (const PropertyMeaning& pm : m.properties)
        if (!keyValid(pm.key) || pm.key.find('.') != std::string::npos ||
            !propertyKeys.insert(pm.key).second)
            add("property key '" + pm.key + "' is malformed or repeated");
    const auto slash = m.patchType.rfind('/');
    if (m.patchType.rfind("supple.", 0) != 0 || slash == std::string::npos ||
        m.patchType.compare(slash - 6, 6, "-patch") != 0)
        add("the patch type must read supple.<plugin>-patch/<major>.<minor>");
    return out;
}

// --- units --------------------------------------------------------------------------------

namespace {

// A framework unit as core-params names it, and how many of those one plain
// unit is.
struct PhysicalUnit {
    const char* name;  // nullptr: none
    double scale;
};

PhysicalUnit physicalUnit(const ParamDef& p) {
    switch (p.unit) {
        case Unit::Decibels:
            return {"dB", 1.0};
        case Unit::Hertz:
            return {"Hz", 1.0};
        case Unit::Milliseconds:
            return {"ms", 1.0};
        case Unit::Seconds:
            return {"ms", 1000.0};
        case Unit::Percent:
            return {"%", 100.0};
        case Unit::Ratio:
            return {"ratio", 1.0};
        case Unit::Semitones:
            return {"st", 1.0};
        case Unit::Cents:
            return {"ct", 1.0};
        case Unit::Degrees:
            return {"deg", 1.0};
        case Unit::Samples:
            return {"samples", 1.0};
        case Unit::Octaves:
            return {"oct", 1.0};
        case Unit::Bpm:
            return {"bpm", 1.0};
        case Unit::Meters:
            return {"m", 1.0};
        default:
            return {nullptr, 1.0};
    }
}

// What a person or model may write after a number, in lower case, and what
// it means in a physical unit.
struct Alias {
    const char* text;
    const char* unit;
    double scale;
};
constexpr Alias kAliases[] = {
    {"hz", "Hz", 1},        {"khz", "Hz", 1000},    {"k", "Hz", 1000},
    {"ms", "ms", 1},        {"s", "ms", 1000},      {"sec", "ms", 1000},
    {"us", "ms", 0.001},    {"db", "dB", 1},        {"st", "st", 1},
    {"semi", "st", 1},      {"semitones", "st", 1}, {"ct", "ct", 1},
    {"cent", "ct", 1},      {"cents", "ct", 1},     {"oct", "oct", 1},
    {"octave", "oct", 1},   {"octaves", "oct", 1},  {"%", "%", 1},
    {":1", "ratio", 1},     {"ratio", "ratio", 1},  {"deg", "deg", 1},
    {"\xC2\xB0", "deg", 1}, {"bpm", "bpm", 1},      {"samples", "samples", 1},
    {"smp", "samples", 1},  {"m", "m", 1},          {"meters", "m", 1},
    {"metres", "m", 1},     {"cm", "m", 0.01},      {"ft", "m", 0.3048},
};

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return std::string(s);
}

size_t editDistance(std::string_view a, std::string_view b) {
    std::vector<size_t> row(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) row[j] = j;
    for (size_t i = 1; i <= a.size(); ++i) {
        size_t diagonal = row[0];
        row[0] = i;
        for (size_t j = 1; j <= b.size(); ++j) {
            const size_t up = row[j];
            row[j] =
                std::min({row[j] + 1, row[j - 1] + 1, diagonal + (a[i - 1] == b[j - 1] ? 0u : 1u)});
            diagonal = up;
        }
    }
    return row[b.size()];
}

std::string closest(std::string_view word, const std::vector<std::string>& candidates) {
    std::string best;
    size_t bestDistance = std::string::npos;
    for (const std::string& c : candidates) {
        const size_t d = editDistance(lower(word), lower(c));
        if (d < bestDistance) bestDistance = d, best = c;
    }
    return best;
}

std::string curveText(const ParamDef& p) {
    if (p.taper == Taper::Log) return "log";
    if (p.taper != Taper::Power || p.curve == 1.0) return "linear";
    char fixed[32];
    std::snprintf(fixed, sizeof(fixed), "%.3f", p.curve);
    std::string s = fixed;
    while (s.size() > 1 && s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    double back = 0.0;
    const auto r = ::supple::fromChars(s.data(), s.data() + s.size(), back);
    if (r.ec != std::errc() || back != p.curve) {
        char shortest[32];
        const auto t = std::to_chars(shortest, shortest + sizeof(shortest), p.curve);
        s.assign(shortest, t.ptr);
    }
    return "pow:" + s;
}

json::Error error(std::string code, std::string message, std::string path, std::string hint) {
    json::Error e;
    e.code = std::move(code);
    e.message = std::move(message);
    e.path = std::move(path);
    e.hint = std::move(hint);
    return e;
}

std::string quantity(double physical, const ParamDef& p) {
    const PhysicalUnit u = physicalUnit(p);
    std::string text = json::formatDouble(physical);
    if (p.kind == ParamKind::Integer && text.size() > 2 &&
        text.compare(text.size() - 2, 2, ".0") == 0)
        text.resize(text.size() - 2);
    return u.name ? text + " " + u.name : text;
}

// "supple.<plugin>-patch/<major>.<minor>": the plugin part and the version.
bool splitType(const std::string& type, std::string& plugin, int& major, int& minor) {
    const auto slash = type.rfind('/');
    if (type.rfind("supple.", 0) != 0 || slash == std::string::npos || slash < 13 ||
        type.compare(slash - 6, 6, "-patch") != 0)
        return false;
    plugin = type.substr(7, slash - 13);
    const auto dot = type.find('.', slash);
    if (dot == std::string::npos) return false;
    const auto a = std::from_chars(type.data() + slash + 1, type.data() + dot, major);
    const auto b = std::from_chars(type.data() + dot + 1, type.data() + type.size(), minor);
    return a.ec == std::errc() && a.ptr == type.data() + dot && b.ec == std::errc() &&
           b.ptr == type.data() + type.size() && !plugin.empty();
}

}  // namespace

// --- the codec ------------------------------------------------------------------------------

PatchCodec::PatchCodec(const Definition& definition)
    : definition_(definition), meaning_(meaningOf(definition)) {}

double PatchCodec::physical(size_t index, double plain) const {
    return plain * physicalUnit(definition_.params().at(index)).scale;
}

double PatchCodec::plain(size_t index, double physical) const {
    return physical / physicalUnit(definition_.params().at(index)).scale;
}

namespace {
// A value as the patch reports it: to 12 significant digits. Parameters are
// kept as host positions, and a value through a log or power taper and back
// lands an ulp or so off (507.49999999999994 for 507.5): reported rounded,
// a value set comes back as it was given, and a patch read and set again is
// the same patch.
double reported(double v) {
    if (v == 0.0 || !std::isfinite(v)) return v;
    const double scale = std::pow(10.0, 11.0 - std::floor(std::log10(std::fabs(v))));
    return std::isfinite(scale) && std::fabs(v * scale) < 9.0e15 ? std::round(v * scale) / scale
                                                                 : v;
}
}  // namespace

json::Value PatchCodec::valueJson(size_t index, double host) const {
    const ParamDef& p = definition_.params().at(index);
    const double v = toPlain(p, sanitizeHost(p, host));
    switch (p.kind) {
        case ParamKind::Toggle:
            return json::Value(v > 0.5);
        case ParamKind::Choice: {
            const auto& keys = meaning_.params[index].choiceKeys;
            const size_t k = static_cast<size_t>(
                std::clamp(std::lround(v), 0L, static_cast<long>(keys.size()) - 1));
            return json::Value(keys.empty() ? std::string() : keys[k]);
        }
        case ParamKind::Integer:
            return json::Value(static_cast<long long>(std::llround(physical(index, v))));
        default:
            return json::Value(reported(physical(index, v)));
    }
}

json::Value PatchCodec::paramsDescription() const {
    json::Value params = json::Value::object();
    const ParamTable& table = definition_.params();
    for (size_t i = 0; i < table.count(); ++i) {
        const ParamDef& p = table.at(i);
        const ParamMeaning& pm = meaning_.params[i];
        json::Value e = json::Value::object();
        e["id"] = static_cast<long long>(p.id);
        e["name"] = std::string(p.name);
        if (p.group && *p.group) e["group"] = std::string(p.group);
        const char* type = p.kind == ParamKind::Toggle    ? "bool"
                           : p.kind == ParamKind::Choice  ? "enum"
                           : p.kind == ParamKind::Integer ? "int"
                                                          : "float";
        e["type"] = type;
        if (p.kind == ParamKind::Continuous || p.kind == ParamKind::Integer) {
            if (const char* unit = physicalUnit(p).name) e["unit"] = unit;
            if (p.kind == ParamKind::Integer) {
                e["min"] = static_cast<long long>(std::llround(physical(i, p.min)));
                e["max"] = static_cast<long long>(std::llround(physical(i, p.max)));
            } else {
                e["min"] = physical(i, p.min);
                e["max"] = physical(i, p.max);
            }
        }
        if (p.kind == ParamKind::Choice) {
            json::Value choices = json::Value::array();
            for (const std::string& k : pm.choiceKeys) choices.push_back(k);
            e["choices"] = std::move(choices);
        }
        if (!p.isOutput()) e["default"] = valueJson(i, hostDefault(p));
        if (p.kind == ParamKind::Continuous) e["curve"] = curveText(p);
        if (!pm.tags.empty()) {
            json::Value tags = json::Value::array();
            for (const std::string& t : pm.tags) tags.push_back(t);
            e["tags"] = std::move(tags);
        }
        if (p.description && *p.description) e["description"] = std::string(p.description);
        json::Value flags = json::Value::array();
        if (p.isOutput()) flags.push_back("readonly");
        if (!p.isOutput() && p.is(pflag::kAutomatable)) flags.push_back("automatable");
        if (!p.isOutput() && p.is(pflag::kModulatable)) flags.push_back("modulatable");
        if (p.is(pflag::kHidden)) flags.push_back("hidden");
        if (p.is(pflag::kOffAtMin)) flags.push_back("off_at_min");
        if (p.is(pflag::kBypass)) flags.push_back("bypass");
        e["flags"] = std::move(flags);
        params[pm.key] = std::move(e);
    }
    return params;
}

json::Value PatchCodec::jsonSchema() const {
    json::Value s = json::Value::object();
    s["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    s["title"] = definition_.info().name + " patch";
    s["type"] = "object";
    json::Value required = json::Value::array();
    required.push_back("schema");
    s["required"] = std::move(required);
    json::Value& props = s["properties"];
    props["schema"]["const"] = meaning_.patchType;
    props["name"]["type"] = "string";
    props["name"]["maxLength"] = 128;
    props["author"]["type"] = "string";
    props["tags"]["type"] = "array";
    props["tags"]["items"]["type"] = "string";
    props["notes"]["type"] = "string";
    json::Value& params = props["params"];
    params["type"] = "object";
    params["additionalProperties"] = false;
    json::Value& each = params["properties"];
    each = json::Value::object();
    const ParamTable& table = definition_.params();
    for (size_t i = 0; i < table.count(); ++i) {
        const ParamDef& p = table.at(i);
        if (p.isOutput()) continue;
        json::Value v = json::Value::object();
        if (p.kind == ParamKind::Toggle) {
            v["type"] = "boolean";
        } else if (p.kind == ParamKind::Choice) {
            json::Value keys = json::Value::array();
            for (const std::string& k : meaning_.params[i].choiceKeys) keys.push_back(k);
            v["enum"] = std::move(keys);
        } else {
            v["type"] = p.kind == ParamKind::Integer ? "integer" : "number";
            v["minimum"] = physical(i, p.min);
            v["maximum"] = physical(i, p.max);
            if (const char* unit = physicalUnit(p).name) v["unit"] = unit;
        }
        if (p.description && *p.description) v["description"] = std::string(p.description);
        each[meaning_.params[i].key] = std::move(v);
    }
    if (definition_.properties().count() > 0) {
        json::Value& properties = props["properties"];
        properties["type"] = "object";
        properties["additionalProperties"] = false;
        json::Value& byKey = properties["properties"];
        byKey = json::Value::object();
        for (size_t i = 0; i < definition_.properties().count(); ++i) {
            const PropertyDef& def = definition_.properties().at(i);
            json::Value v = json::Value::object();
            v["type"] = "string";
            v["maxLength"] = static_cast<long long>(def.limit);
            std::string about = def.description ? def.description : "";
            if (def.isFile()) {
                // What a caller needs to know to fill it in.
                if (!about.empty()) about += " ";
                about += "The path of a file";
                if (def.extensions && *def.extensions)
                    about += " (" + std::string(def.extensions) + ")";
                about += "; \"\" for none.";
            }
            if (!about.empty()) v["description"] = about;
            byKey[meaning_.properties[i].key] = std::move(v);
        }
    }
    s["additionalProperties"] = false;
    return s;
}

json::Value PatchCodec::schemaDocument() const {
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.semantic-schema/1.0";
    doc["plugin"]["id"] = definition_.info().id;
    doc["plugin"]["name"] = definition_.info().name;
    doc["plugin"]["version"] = definition_.info().version;
    doc["patch_type"] = meaning_.patchType;
    doc["params"] = paramsDescription();
    doc["patch"] = jsonSchema();
    doc["docs"] = meaning_.docs;
    return doc;
}

json::Value PatchCodec::patch(const state::Snapshot& s, const Envelope& envelope,
                              bool changedOnly) const {
    json::Value doc = json::Value::object();
    doc["schema"] = meaning_.patchType;
    if (envelope.name) doc["name"] = *envelope.name;
    if (envelope.author) doc["author"] = *envelope.author;
    if (!envelope.tags.empty()) {
        json::Value tags = json::Value::array();
        for (const std::string& t : envelope.tags) tags.push_back(t);
        doc["tags"] = std::move(tags);
    }
    if (envelope.notes) doc["notes"] = *envelope.notes;
    json::Value params = json::Value::object();
    const ParamTable& table = definition_.params();
    for (size_t i = 0; i < table.count(); ++i) {
        const ParamDef& p = table.at(i);
        if (p.isOutput()) continue;
        const json::Value v = valueJson(i, s.host[i]);
        if (changedOnly && v == valueJson(i, hostDefault(p))) continue;
        params[meaning_.params[i].key] = v;
    }
    doc["params"] = std::move(params);
    const PropertyList& properties = definition_.properties();
    if (properties.count() > 0) {
        json::Value texts = json::Value::object();
        for (size_t i = 0; i < properties.count(); ++i) {
            const std::string value = s.hasProperty[i] ? s.property[i] : properties.at(i).def;
            if (changedOnly && value == properties.at(i).def) continue;
            texts[meaning_.properties[i].key] = value;
        }
        doc["properties"] = std::move(texts);
    }
    return doc;
}

bool PatchCodec::readValue(size_t index, const json::Value& v, const std::string& path,
                           double& host, std::vector<json::Error>& errors) const {
    const ParamDef& p = definition_.params().at(index);
    const ParamMeaning& pm = meaning_.params[index];
    auto typeError = [&](const std::string& example) {
        errors.push_back(error(
            "params.type", "'" + pm.key + "' cannot be " + std::string(json::typeName(v.type())),
            path, "write it as " + example));
        return false;
    };
    if (p.kind == ParamKind::Toggle) {
        bool on = false;
        if (v.isBool())
            on = v.asBool();
        else if (v.isNumber() && (v.asDouble() == 0.0 || v.asDouble() == 1.0))
            on = v.asDouble() == 1.0;
        else if (v.isString() && (lower(v.asString()) == "on" || lower(v.asString()) == "off"))
            on = lower(v.asString()) == "on";
        else
            return typeError("true or false");
        host = toHost(p, on ? 1.0 : 0.0);
        return true;
    }
    if (p.kind == ParamKind::Choice) {
        int chosen = -1;
        if (v.isString()) {
            const std::string want = lower(trim(v.asString()));
            for (size_t k = 0; k < pm.choiceKeys.size(); ++k)
                if (pm.choiceKeys[k] == want || (p.labels && lower(p.labels[k]) == want))
                    chosen = static_cast<int>(k);
        } else if (v.isInt() && v.asInt() >= 0 &&
                   v.asInt() < static_cast<long long>(pm.choiceKeys.size())) {
            chosen = static_cast<int>(v.asInt());
        } else {
            return typeError("one of its choices, e.g. \"" + pm.choiceKeys.front() + "\"");
        }
        if (chosen < 0) {
            std::string all;
            for (const std::string& k : pm.choiceKeys) all += (all.empty() ? "" : ", ") + k;
            errors.push_back(error(
                "params.enum", "'" + v.asString() + "' is not a choice of '" + pm.key + "'", path,
                "did you mean \"" + closest(v.asString(), pm.choiceKeys) + "\"? Choices: " + all));
            return false;
        }
        host = toHost(p, chosen);
        return true;
    }
    // A number in the physical unit, or text with a unit or "off".
    const PhysicalUnit unit = physicalUnit(p);
    double physical = 0.0;
    if (v.isNumber()) {
        physical = v.asDouble();
    } else if (v.isString()) {
        const std::string text = lower(trim(v.asString()));
        if (p.is(pflag::kOffAtMin) && (text == "off" || text == "-inf" || text == "-inf db")) {
            host = toHost(p, p.min);
            return true;
        }
        const char* first = text.data();
        const char* last = text.data() + text.size();
        if (first != last && *first == '+') ++first;
        const auto r = ::supple::fromChars(first, last, physical);
        if (r.ec != std::errc() || !std::isfinite(physical))
            return typeError(unit.name
                                 ? "a number of " + std::string(unit.name) + ", e.g. " +
                                       quantity(this->physical(index, p.def), p)
                                 : "a number, e.g. " + quantity(this->physical(index, p.def), p));
        const std::string suffix = trim(std::string_view(r.ptr, static_cast<size_t>(last - r.ptr)));
        if (!suffix.empty()) {
            const Alias* alias = nullptr;
            for (const Alias& a : kAliases)
                if (suffix == a.text) alias = &a;
            if (!alias || !unit.name || std::strcmp(alias->unit, unit.name) != 0) {
                errors.push_back(
                    error("params.unit",
                          "'" + v.asString() + "' is not in a unit '" + pm.key + "' takes", path,
                          unit.name ? "use a number of " + std::string(unit.name)
                                    : "use a plain number"));
                return false;
            }
            physical *= alias->scale;
        }
    } else {
        return typeError("a number, e.g. " + quantity(this->physical(index, p.def), p));
    }
    const double lo = this->physical(index, p.min), hi = this->physical(index, p.max);
    if (physical < lo - 1e-9 * std::max(1.0, std::fabs(lo)) ||
        physical > hi + 1e-9 * std::max(1.0, std::fabs(hi))) {
        errors.push_back(
            error("params.out_of_range",
                  "'" + pm.key + "' is " + quantity(physical, p) + ", outside its range", path,
                  "use a value from " + quantity(lo, p) + " to " + quantity(hi, p)));
        return false;
    }
    physical = std::clamp(physical, lo, hi);
    if (p.kind == ParamKind::Integer && std::fabs(physical - std::round(physical)) > 1e-9) {
        errors.push_back(error("params.type", "'" + pm.key + "' takes whole numbers", path,
                               "use " + std::to_string(std::llround(physical))));
        return false;
    }
    host = sanitizeHost(p, toHost(p, plain(index, physical)));
    return true;
}

namespace {

// A path from UTF-8 text, on any system.
std::filesystem::path pathOf(const std::string& utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

// Whether `path` ends in one of `extensions` (space-separated, without dots;
// empty takes anything), ignoring case.
bool hasExtension(const std::string& path, std::string_view extensions) {
    if (extensions.empty()) return true;
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of("/\\");
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) return false;
    std::string ext;
    for (char c : path.substr(dot + 1))
        ext += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    size_t at = 0;
    while (at < extensions.size()) {
        size_t end = extensions.find(' ', at);
        if (end == std::string_view::npos) end = extensions.size();
        if (extensions.substr(at, end - at) == ext) return true;
        at = end + 1;
    }
    return false;
}

}  // namespace

// A path a file property may take: "" (none), the one it holds (a session's
// reference stays, found or not), or an existing file of its types that the
// file check can read.
bool PatchCodec::fileUsable(size_t index, const std::string& path, const state::Snapshot& current,
                            std::string& why) const {
    const PropertyDef& def = definition_.properties().at(index);
    if (!def.isFile() || path.empty()) return true;
    const std::string held = current.hasProperty[index] ? current.property[index] : def.def;
    if (path == held) return true;
    const std::filesystem::path p = pathOf(path);
    std::error_code ec;
    if (!p.is_absolute())
        why = "the path is not absolute";
    else if (!std::filesystem::is_regular_file(p, ec))
        why = "there is no file there";
    else if (!hasExtension(path, def.extensions ? def.extensions : ""))
        why = "it takes " + std::string(def.extensions) + " files";
    else if (fileCheck_ && !fileCheck_(index, path, why)) {
        if (why.empty()) why = "the file cannot be read";
    } else
        return true;
    return false;
}

bool PatchCodec::apply(const json::Value& document, bool merge, state::Snapshot& snapshot,
                       Envelope& envelope, std::vector<json::Error>& errors) const {
    errors.clear();
    if (!document.isObject()) {
        errors.push_back(
            error(merge ? "patch.merge_not_object" : "patch.not_object", "a patch is a JSON object",
                  "", "send {\"schema\": \"" + meaning_.patchType + "\", \"params\": {...}}"));
        return false;
    }
    const json::Value doc =
        merge ? json::mergePatch(patch(snapshot, envelope, false), document) : document;

    // The envelope and its version.
    const json::Value* type = doc.find("schema");
    bool strict = true;
    if (!type || !type->isString()) {
        errors.push_back(error("patch.missing_schema", "the patch does not say what it is",
                               "/schema", "add \"schema\": \"" + meaning_.patchType + "\""));
        return false;
    }
    std::string plugin, ours;
    int major = 0, minor = 0, ourMajor = 0, ourMinor = 0;
    splitType(meaning_.patchType, ours, ourMajor, ourMinor);
    if (!splitType(type->asString(), plugin, major, minor)) {
        errors.push_back(error("patch.bad_schema_id",
                               "'" + type->asString() + "' is not a patch type", "/schema",
                               "use \"" + meaning_.patchType + "\""));
        return false;
    }
    if (plugin != ours) {
        errors.push_back(error("patch.wrong_type",
                               "this is a patch for '" + plugin + "', not '" + ours + "'",
                               "/schema", "use \"" + meaning_.patchType + "\""));
        return false;
    }
    if (major != ourMajor) {
        errors.push_back(error("patch.too_new",
                               "patch version " + std::to_string(major) + " is not version " +
                                   std::to_string(ourMajor),
                               "/schema", "use \"" + meaning_.patchType + "\""));
        return false;
    }
    strict = minor <= ourMinor;  // a newer minor's additions are ignored

    Envelope nextEnvelope;
    auto text = [&](const char* member, std::optional<std::string>& out, size_t limit) {
        const json::Value* v = doc.find(member);
        if (!v) return;
        if (!v->isString() || v->asString().size() > limit)
            errors.push_back(error("patch.type",
                                   std::string("\"") + member + "\" must be a string of at most " +
                                       std::to_string(limit) + " bytes",
                                   std::string("/") + member, "write it as text"));
        else
            out = v->asString();
    };
    text("name", nextEnvelope.name, 128);
    text("author", nextEnvelope.author, 256);
    text("notes", nextEnvelope.notes, 65536);
    if (const json::Value* tags = doc.find("tags")) {
        if (!tags->isArray()) {
            errors.push_back(error("patch.type", "\"tags\" must be an array of strings", "/tags",
                                   "write [\"warm\", \"pad\"]"));
        } else {
            for (size_t k = 0; k < tags->size(); ++k) {
                const json::Value& t = tags->asArray()[k];
                if (t.isString())
                    nextEnvelope.tags.push_back(t.asString());
                else
                    errors.push_back(error("patch.type", "a tag must be a string",
                                           "/tags/" + std::to_string(k), "write it as text"));
            }
        }
    }
    if (strict) {
        static const std::array<const char*, 7> kMembers = {"schema", "name",   "author",    "tags",
                                                            "notes",  "params", "properties"};
        for (const json::Member& m : doc.asObject()) {
            if (m.first == "properties" && definition_.properties().count() == 0) {
                errors.push_back(error("patch.unknown_member", "this plugin has no properties",
                                       "/properties", "remove the properties member"));
                continue;
            }
            if (std::find_if(kMembers.begin(), kMembers.end(),
                             [&](const char* k) { return m.first == k; }) != kMembers.end())
                continue;
            errors.push_back(
                error("patch.unknown_member", "\"" + m.first + "\" is not part of this patch",
                      json::pointerJoin("", m.first),
                      "a patch has schema, name, author, tags, notes, params and properties"));
        }
    }

    // Values: everything starts from its default, then what the document says.
    std::vector<double> host(snapshot.host.size());
    const ParamTable& table = definition_.params();
    for (size_t i = 0; i < table.count(); ++i)
        host[i] = table.at(i).isOutput() ? snapshot.host[i] : hostDefault(table.at(i));
    if (const json::Value* params = doc.find("params")) {
        if (!params->isObject()) {
            errors.push_back(
                error("patch.type", "\"params\" must be an object of values by key", "/params",
                      "write \"params\": {\"" +
                          (meaning_.params.empty() ? std::string("key") : meaning_.params[0].key) +
                          "\": ...}"));
        } else {
            std::vector<std::string> keys;
            for (const ParamMeaning& pm : meaning_.params) keys.push_back(pm.key);
            for (const json::Member& m : params->asObject()) {
                const std::string path = json::pointerJoin("/params", m.first);
                size_t index = table.count();
                for (size_t i = 0; i < meaning_.params.size(); ++i)
                    if (meaning_.params[i].key == m.first) index = i;
                if (index == table.count()) {
                    errors.push_back(error("params.unknown_key",
                                           "this plugin has no parameter '" + m.first + "'", path,
                                           "did you mean '" + closest(m.first, keys) + "'?"));
                    continue;
                }
                if (table.at(index).isOutput()) {
                    errors.push_back(error("params.readonly",
                                           "'" + m.first + "' is a meter: the plugin reports it",
                                           path, "leave it out of the patch"));
                    continue;
                }
                // What it reports now, written back: kept exactly, where the
                // value would go through its unit and back (get, then set, is
                // the identity).
                if (valueJson(index, snapshot.host[index]) == m.second) {
                    host[index] = snapshot.host[index];
                    continue;
                }
                double value = 0.0;
                if (readValue(index, m.second, path, value, errors)) host[index] = value;
            }
        }
    }
    const PropertyList& properties = definition_.properties();
    std::vector<std::string> texts(properties.count());
    for (size_t i = 0; i < properties.count(); ++i) texts[i] = properties.at(i).def;
    if (const json::Value* given = doc.find("properties"); given && properties.count() > 0) {
        if (!given->isObject()) {
            errors.push_back(error(
                "patch.type", "\"properties\" must be an object of text by key", "/properties",
                "write \"properties\": {\"" + meaning_.properties[0].key + "\": \"...\"}"));
        } else {
            std::vector<std::string> keys;
            for (const PropertyMeaning& pm : meaning_.properties) keys.push_back(pm.key);
            for (const json::Member& m : given->asObject()) {
                const std::string path = json::pointerJoin("/properties", m.first);
                size_t index = properties.count();
                for (size_t i = 0; i < meaning_.properties.size(); ++i)
                    if (meaning_.properties[i].key == m.first) index = i;
                if (index == properties.count()) {
                    errors.push_back(error("properties.unknown_key",
                                           "this plugin has no property '" + m.first + "'", path,
                                           "did you mean '" + closest(m.first, keys) + "'?"));
                } else if (!m.second.isString()) {
                    errors.push_back(error("properties.type", "'" + m.first + "' is text", path,
                                           "write it as a string"));
                } else if (!properties.accepts(index, m.second.asString())) {
                    errors.push_back(error(
                        "properties.too_long", "'" + m.first + "' is longer than its limit", path,
                        "keep it to " + std::to_string(properties.at(index).limit) + " bytes"));
                } else if (std::string why;
                           !fileUsable(index, m.second.asString(), snapshot, why)) {
                    errors.push_back(error(
                        "properties.file", "'" + m.first + "': " + why, path,
                        "give the absolute path of an existing " +
                            (properties.at(index).extensions && *properties.at(index).extensions
                                 ? std::string(properties.at(index).extensions) + " "
                                 : std::string()) +
                            "file, or \"\" for none"));
                } else {
                    texts[index] = m.second.asString();
                }
            }
        }
    }
    if (!errors.empty()) return false;
    snapshot.host = std::move(host);
    for (size_t i = 0; i < properties.count(); ++i) {
        snapshot.property[i] = std::move(texts[i]);
        snapshot.hasProperty[i] = true;
    }
    envelope = std::move(nextEnvelope);
    return true;
}

void PatchCodec::restore(const json::Value& doc, state::Snapshot& snapshot,
                         Envelope& envelope) const {
    if (!doc.isObject()) return;
    Envelope next;
    if (const json::Value* v = doc.find("name"); v && v->isString()) next.name = v->asString();
    if (const json::Value* v = doc.find("author"); v && v->isString()) next.author = v->asString();
    if (const json::Value* v = doc.find("notes"); v && v->isString()) next.notes = v->asString();
    if (const json::Value* v = doc.find("tags"); v && v->isArray())
        for (const json::Value& t : v->asArray())
            if (t.isString()) next.tags.push_back(t.asString());
    envelope = std::move(next);
    std::vector<json::Error> ignored;
    const ParamTable& table = definition_.params();
    if (const json::Value* params = doc.find("params"); params && params->isObject()) {
        for (const json::Member& m : params->asObject()) {
            for (size_t i = 0; i < meaning_.params.size(); ++i) {
                if (meaning_.params[i].key != m.first || table.at(i).isOutput()) continue;
                // The snapshot's own value when it writes this one: exact, where
                // the patch's has been through a unit and back.
                if (valueJson(i, snapshot.host[i]) == m.second) continue;
                double host = 0.0;
                if (readValue(i, m.second, "", host, ignored)) snapshot.host[i] = host;
            }
        }
    }
    const PropertyList& properties = definition_.properties();
    if (const json::Value* given = doc.find("properties"); given && given->isObject()) {
        for (const json::Member& m : given->asObject()) {
            for (size_t i = 0; i < meaning_.properties.size(); ++i) {
                if (meaning_.properties[i].key != m.first || !m.second.isString() ||
                    !properties.accepts(i, m.second.asString()))
                    continue;
                snapshot.property[i] = m.second.asString();
                snapshot.hasProperty[i] = true;
            }
        }
    }
}

std::string PatchCodec::describe(const state::Snapshot& s, bool changedOnly) const {
    // Groups in the order their first parameter appears; within each, its
    // parameters in table order.
    std::vector<std::pair<std::string, std::string>> groups;  // group, text
    const ParamTable& table = definition_.params();
    for (size_t i = 0; i < table.count(); ++i) {
        const ParamDef& p = table.at(i);
        if (p.isOutput() || p.is(pflag::kHidden)) continue;
        if (changedOnly && valueJson(i, s.host[i]) == valueJson(i, hostDefault(p))) continue;
        char value[64];
        formatValue(p, s.host[i], value, sizeof(value));
        const std::string group = p.group ? p.group : "";
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&](const auto& g) { return g.first == group; });
        if (it == groups.end()) {
            groups.emplace_back(group, "");
            it = groups.end() - 1;
        }
        if (!it->second.empty()) it->second += ", ";
        // Under its group's name, a name that repeats it says the rest
        // ("Band 1: Gain -3.0 dB", not "Band 1: Band 1 Gain -3.0 dB"); a
        // switch that is on says its name alone.
        std::string label = p.displayLabel();
        if (!group.empty() && label.size() > group.size() + 1 && label.starts_with(group) &&
            label[group.size()] == ' ')
            label = label.substr(group.size() + 1);
        if (p.kind == ParamKind::Toggle)
            it->second += toPlain(p, s.host[i]) >= 0.5 ? label : label + " off";
        else
            it->second += label + " " + value;
    }
    std::string out;
    for (const auto& [group, text] : groups) {
        if (!out.empty()) out += "; ";
        out += group.empty() ? text : group + ": " + text;
    }
    const PropertyList& properties = definition_.properties();
    for (size_t i = 0; i < properties.count(); ++i) {
        const std::string value = s.hasProperty[i] ? s.property[i] : properties.at(i).def;
        if (changedOnly && value == properties.at(i).def) continue;
        if (!out.empty()) out += "; ";
        out += std::string(properties.at(i).name) + " \"" +
               (value.size() > 40 ? value.substr(0, 37) + "..." : value) + "\"";
    }
    if (out.empty()) out = "default settings";
    out = definition_.info().name + ": " + out;
    if (changedOnly && out.size() > 600) out = out.substr(0, 597) + "...";
    return out;
}

// --- state framing --------------------------------------------------------------------------

std::vector<uint8_t> frame(const std::string& canonicalJson, uint32_t magic) {
    std::vector<uint8_t> out;
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
    };
    u32(magic);
    u32(2);
    u32(static_cast<uint32_t>(canonicalJson.size()));
    out.insert(out.end(), canonicalJson.begin(), canonicalJson.end());
    return out;
}

bool unframe(const uint8_t* data, size_t size, uint32_t magic, std::string& json) {
    if (size < 12) return false;
    auto u32 = [&](size_t at) {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= static_cast<uint32_t>(data[at + static_cast<size_t>(i)]) << (8 * i);
        return v;
    };
    const uint32_t length = u32(8);
    if (u32(0) != magic || u32(4) != 2 || length > (16u << 20) || length != size - 12) return false;
    json.assign(reinterpret_cast<const char*>(data + 12), length);
    return true;
}

}  // namespace supple::suite
