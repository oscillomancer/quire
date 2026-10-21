// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "supple/sandbox/session.h"

#include "../../dsp-commands/script_library.h"
#include "schema_params.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <set>

#include "supple/base/closest.h"
#include "supple/base/files.h"
#include "supple/clap/semantic_params.h"

namespace supple::sandbox {

namespace fs = std::filesystem;

struct Session::Loaded {
    std::string id;
    host::PluginInfo info;
    std::unique_ptr<host::Instance> instance;
    json::Value schema;  // semantic schema, or null
};

struct Session::Txn {
    SessionSettings settings;
    std::vector<std::string> chain;
    std::set<std::string> instanceIds, renderIds;
    std::map<std::string, host::PluginInfo> infos;
    std::map<std::string, std::vector<std::uint8_t>> states;  // saved at begin
    std::map<std::string, std::unique_ptr<Loaded>> removed;   // kept until commit
};

namespace {

// Whether a schema entry's parameter applies with the current values: unless
// it says "active": false.
bool applies(const json::Value* entry) {
    const json::Value* active = entry ? entry->find("active") : nullptr;
    return !(active && active->isBool() && !active->asBool());
}

bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// Moves error paths from under `from` to under `to` (recursively), e.g. the
// plugin's "/params/cutoff" becomes the command's "/values/cutoff".
Error rebase(Error e, std::string_view from, std::string_view to) {
    if (e.path.rfind(from, 0) == 0) e.path = std::string(to) + e.path.substr(from.size());
    for (auto& d : e.details) d = rebase(std::move(d), from, to);
    return e;
}

Error prefixed(Error e, std::string_view prefix) {
    if (!e.path.empty() || e.details.empty()) e.path = std::string(prefix) + e.path;
    for (auto& d : e.details) d = prefixed(std::move(d), prefix);
    return e;
}

std::string label(const std::string& id, const host::PluginInfo& info) {
    return id + " (" + info.name + ")";
}

bool validPresetName(std::string_view n) {
    if (n.empty() || n.size() > 64 || n.front() == '.' || n.front() == ' ') return false;
    return std::all_of(n.begin(), n.end(), [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) || c == ' ' || c == '_' || c == '.' ||
               c == '+' || c == '-';
    });
}

std::string safeDirName(std::string_view pluginId) {
    std::string s(pluginId);
    for (char& c : s)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '-' && c != '_')
            c = '_';
    if (s.empty() || s.front() == '.') s.insert(s.begin(), '_');
    return s;
}

std::string closestHint(std::string_view input, const std::vector<std::string>& names,
                        std::string_view listCommand) {
    std::vector<std::string_view> views(names.begin(), names.end());
    const std::string best = closestMatch(input, views);
    std::string hint;
    if (!best.empty()) hint = "did you mean '" + best + "'? ";
    hint += std::string("see ") + std::string(listCommand);
    return hint;
}

bool isPowerOfTwo(unsigned v) { return v && !(v & (v - 1)); }

}  // namespace

// --- settings ----------------------------------------------------------------------

Status validate(const SessionSettings& s) {
    Error all("sandbox.settings", "invalid session settings");
    auto bad = [&](const char* path, std::string message, std::string hint) {
        all.details.emplace_back("sandbox.settings", std::move(message), path, std::move(hint));
    };
    if (!(s.sampleRate >= 8000 && s.sampleRate <= 384000))
        bad("/sample_rate", "sample rate out of range", "8000 .. 384000 Hz");
    if (s.blockSize < 1 || s.blockSize > 8192)
        bad("/block_size", "block size out of range", "1 .. 8192 frames");
    if (!(s.tempo >= 1 && s.tempo <= 999)) bad("/tempo", "tempo out of range", "1 .. 999 BPM");
    if (s.numerator < 1 || s.numerator > 64 || !isPowerOfTwo(s.denominator) || s.denominator > 64)
        bad("/meter", "invalid meter", "N/D with N 1..64 and D one of 1, 2, 4, 8, 16, 32, 64");
    if (!(s.tailLimitSeconds >= 0 && s.tailLimitSeconds <= 60))
        bad("/tail_limit_s", "tail limit out of range", "0 .. 60 seconds");
    if (all.details.empty()) return okStatus();
    if (all.details.size() == 1) return all.details.front();
    return all;
}

json::Value toJson(const SessionSettings& s) {
    json::Value v = json::Value::object();
    v["sample_rate"] = s.sampleRate;
    v["block_size"] = s.blockSize;
    v["tempo"] = s.tempo;
    v["meter"] = std::to_string(s.numerator) + "/" + std::to_string(s.denominator);
    v["tail_limit_s"] = s.tailLimitSeconds;
    return v;
}

// --- timing ------------------------------------------------------------------------

std::int64_t ticksToSamples(std::int64_t ticks, int ppq, double tempo, double sampleRate) {
    return std::llround(static_cast<double>(ticks) * 60.0 * sampleRate /
                        (tempo * static_cast<double>(ppq)));
}

std::vector<TimedEvent> sequenceEvents(const midi::Sequence& seq, double tempo, double sampleRate) {
    // Order at one sample: note-offs, then controls, then note-ons (so a
    // controller set "at" a note start applies to it).
    struct Item {
        std::int64_t sample;
        int rank;  // 0 off, 1 control, 2 on
        TimedEvent ev;
    };
    std::vector<Item> items;
    items.reserve(seq.notes.size() * 2 + seq.controls.size());
    std::int32_t noteId = 0;
    for (const midi::Note& n : seq.notes) {
        const std::int64_t on = ticksToSamples(n.start, seq.ppq, tempo, sampleRate);
        // A zero-length note still sounds for one sample, so its off never
        // precedes its own on.
        const std::int64_t off =
            std::max(on + 1, ticksToSamples(n.start + n.length, seq.ppq, tempo, sampleRate));
        TimedEvent e;
        e.target = 0;
        e.event.channel = n.channel;
        e.event.key = n.pitch;
        e.event.noteId = noteId++;
        e.event.velocity = n.velocity / 127.0;
        e.sample = on;
        e.event.kind = host::Event::Kind::NoteOn;
        items.push_back({on, 2, e});
        e.sample = off;
        e.event.kind = host::Event::Kind::NoteOff;
        e.event.velocity = n.offVelocity / 127.0;
        items.push_back({off, 0, e});
    }
    auto b7 = [](std::int32_t v) { return static_cast<std::uint8_t>(std::clamp(v, 0, 127)); };
    for (const midi::Control& c : seq.controls) {
        TimedEvent e;
        e.target = 0;
        e.sample = ticksToSamples(c.tick, seq.ppq, tempo, sampleRate);
        e.event.kind = host::Event::Kind::Midi;
        const auto ch = static_cast<std::uint8_t>(c.channel & 0x0f);
        switch (c.kind) {
            case midi::ControlKind::CC:
                e.event.midi[0] = static_cast<std::uint8_t>(0xb0 | ch);
                e.event.midi[1] = b7(c.number);
                e.event.midi[2] = b7(c.value);
                break;
            case midi::ControlKind::ProgramChange:
                e.event.midi[0] = static_cast<std::uint8_t>(0xc0 | ch);
                e.event.midi[1] = b7(c.value);
                break;
            case midi::ControlKind::ChannelPressure:
                e.event.midi[0] = static_cast<std::uint8_t>(0xd0 | ch);
                e.event.midi[1] = b7(c.value);
                break;
            case midi::ControlKind::PolyPressure:
                e.event.midi[0] = static_cast<std::uint8_t>(0xa0 | ch);
                e.event.midi[1] = b7(c.number);
                e.event.midi[2] = b7(c.value);
                break;
            case midi::ControlKind::PitchBend: {
                const std::int32_t v = std::clamp(c.value, -8192, 8191) + 8192;
                e.event.midi[0] = static_cast<std::uint8_t>(0xe0 | ch);
                e.event.midi[1] = static_cast<std::uint8_t>(v & 0x7f);
                e.event.midi[2] = static_cast<std::uint8_t>((v >> 7) & 0x7f);
                break;
            }
        }
        items.push_back({e.sample, 1, e});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
        if (a.sample != b.sample) return a.sample < b.sample;
        return a.rank < b.rank;
    });
    std::vector<TimedEvent> out;
    out.reserve(items.size());
    for (auto& i : items) out.push_back(std::move(i.ev));
    return out;
}

// --- session -------------------------------------------------------------------------

Session::Session(SessionOptions options) : options_(std::move(options)) {}

Session::~Session() {
    stopJobs();
    // Instances deactivate and unload in their destructors; renders stay on disk.
    txn_.reset();
    instances_.clear();
}

Result<std::unique_ptr<Session>> Session::open(SessionOptions options) {
    if (options.workdir.empty())
        return Error("sandbox.workdir", "no work directory given", {}, "pass --workdir DIR");
    std::error_code ec;
    fs::create_directories(options.workdir / "renders", ec);
    if (ec)
        return Error("sandbox.workdir", "cannot create " + options.workdir.string(), {},
                     "choose a writable directory");
    options.workdir = fs::absolute(options.workdir, ec);
    if (options.presetDir.empty()) options.presetDir = options.workdir / "presets";
    auto session = std::unique_ptr<Session>(new Session(std::move(options)));
    dsp_commands::nameScriptLibrary(session->scriptDir());
    return session;
}

// --- plugins -------------------------------------------------------------------------

Result<std::span<const host::PluginInfo>> Session::plugins(bool rescan) {
    if (!catalogue_ || rescan) {
        const auto path =
            options_.pluginPath.empty() ? host::defaultSearchPath() : options_.pluginPath;
        auto list = host::scan(path, host::ScanOptions{}, &scanReport_);
        if (!list) return std::move(list).error();
        host::orderCatalogue(*list);  // the first location on the path wins an id
        catalogue_ = std::move(*list);
    }
    return std::span<const host::PluginInfo>(*catalogue_);
}

Result<host::PluginInfo> Session::findPlugin(std::string_view idOrPath) {
    const bool isPath = idOrPath.find('/') != std::string_view::npos ||
                        (idOrPath.size() > 5 && idOrPath.substr(idOrPath.size() - 5) == ".clap");
    if (isPath) {
        const fs::path p(idOrPath);
        if (!fs::exists(p))
            return Error("sandbox.unknown_plugin", "no such file: " + p.string(), {},
                         "pass a plugin id from plugin.list or an existing .clap path");
        auto list = host::scan(std::span<const fs::path>(&p, 1));
        if (!list) return std::move(list).error();
        if (list->empty())
            return Error("sandbox.unknown_plugin", p.string() + " contains no CLAP plugins");
        if (list->size() > 1) {
            std::string ids;
            for (const auto& i : *list) ids += (ids.empty() ? "" : ", ") + i.id;
            return Error("sandbox.ambiguous_plugin", p.string() + " contains several plugins", {},
                         "load one by id (add its directory to the plugin path): " + ids);
        }
        return list->front();
    }
    auto all = plugins();
    if (!all) return std::move(all).error();
    for (const auto& p : *all)
        if (p.id == idOrPath) return p;
    for (const auto& p : *all)
        if (iequals(p.id, idOrPath)) return p;
    std::vector<std::string> ids;
    for (const auto& p : *all) ids.push_back(p.id);
    return Error("sandbox.unknown_plugin", "no plugin with id '" + std::string(idOrPath) + "'", {},
                 closestHint(idOrPath, ids, "plugin.list"));
}

// --- instances and chain -------------------------------------------------------------

std::string Session::candidates() const {
    if (instances_.empty()) return "no instances are loaded; use plugin.load";
    std::string s = "loaded: ";
    bool first = true;
    for (const auto& [id, l] : instances_) {
        s += (first ? "" : ", ") + label(id, l->info);
        first = false;
    }
    return s;
}

const Session::Loaded* Session::findLoaded(std::string_view id) const {
    auto it = instances_.find(std::string(id));
    return it == instances_.end() ? nullptr : it->second.get();
}

Result<Session::Loaded*> Session::loaded(std::string_view id) {
    auto it = instances_.find(std::string(id));
    if (it == instances_.end())
        return Error("sandbox.unknown_instance", "no instance '" + std::string(id) + "'",
                     "/instance", candidates());
    return it->second.get();
}

Result<Session::Loaded*> Session::semantic(std::string_view id) {
    auto l = loaded(id);
    if (!l) return l;
    if (!(*l)->info.semantic)
        return Error("sandbox.not_semantic",
                     label((*l)->id, (*l)->info) + " has no semantic parameters or patches", {},
                     "use param.get/param.set with CLAP ids or names, and presets for state");
    return l;
}

Result<host::Instance*> Session::instance(std::string_view id) {
    auto l = loaded(id);
    if (!l) return std::move(l).error();
    return (*l)->instance.get();
}

const json::Value& Session::schema(std::string_view id) {
    static const json::Value kNull;
    auto it = instances_.find(std::string(id));
    if (it == instances_.end()) return kNull;
    Loaded& l = *it->second;
    if (!l.info.semantic) return l.schema;  // null: no semantic view
    // The plugin derives each parameter's "active" flag from its current
    // patch, so the schema captured at load goes stale the moment a patch adds
    // or drops a module (filter1.cutoff reads active: false until
    // filters.filter1 exists, while optimize.start already accepts it). Re-read
    // it here: a query-path call, and the plugin builds the document from its
    // own state.
    auto live = l.instance->semanticSchema();
    if (live)
        l.schema = std::move(*live);
    else if (!l.instance->info().semantic) {
        l.info.semantic = false;
        l.schema = {};
    }
    return l.schema;
}

Result<std::string> Session::load(std::string_view pluginIdOrPath,
                                  std::optional<std::size_t> position) {
    auto info = findPlugin(pluginIdOrPath);
    if (!info) return std::move(info).error();
    const bool instrument = info->instrument;
    if (instrument && !chain_.empty()) {
        const Loaded* first = findLoaded(chain_.front());
        if (first && first->info.instrument)
            return Error(
                "sandbox.chain_order",
                "the chain already starts with instrument " + label(first->id, first->info), {},
                "remove it first (plugin.remove), or load an effect");
    }
    std::size_t at = instrument ? 0 : chain_.size();
    if (position) {
        if (*position > chain_.size())
            return Error("sandbox.position",
                         "position " + std::to_string(*position) + " is past the end of the chain",
                         "/position", "0 .. " + std::to_string(chain_.size()));
        if (instrument && *position != 0)
            return Error("sandbox.chain_order", info->name + " is an instrument", "/position",
                         "instruments go first (position 0)");
        if (!instrument && *position == 0 && !chain_.empty()) {
            const Loaded* first = findLoaded(chain_.front());
            if (first && first->info.instrument)
                return Error("sandbox.chain_order",
                             "position 0 is taken by instrument " + label(first->id, first->info),
                             "/position", "effects go after the instrument (position >= 1)");
        }
        at = *position;
    }
    auto made = host::Instance::create(*info, options_.identity);
    if (!made) return std::move(made).error();
    auto l = std::make_unique<Loaded>();
    l->id = "in_" + std::to_string(nextInstance_++);
    l->instance = std::move(*made);
    l->info = l->instance->info();
    if (l->info.semantic) {
        auto s = l->instance->semanticSchema();
        if (!s) return std::move(s).error();
        l->schema = std::move(*s);
    }
    const std::string id = l->id;
    instances_.emplace(id, std::move(l));
    chain_.insert(chain_.begin() + static_cast<std::ptrdiff_t>(at), id);
    return id;
}

Status Session::remove(std::string_view id) {
    auto it = instances_.find(std::string(id));
    if (it == instances_.end())
        return Error("sandbox.unknown_instance", "no instance '" + std::string(id) + "'",
                     "/instance", candidates());
    chain_.erase(std::remove(chain_.begin(), chain_.end(), it->first), chain_.end());
    if (txn_ && txn_->instanceIds.count(it->first))
        txn_->removed.emplace(it->first, std::move(it->second));  // restorable until commit
    instances_.erase(it);
    return okStatus();
}

Status Session::move(std::string_view id, std::size_t position) {
    auto l = loaded(id);
    if (!l) return std::move(l).error();
    if (position >= chain_.size())
        return Error("sandbox.position",
                     "position " + std::to_string(position) + " is past the end", "/position",
                     "0 .. " + std::to_string(chain_.size() - 1));
    auto cur = std::find(chain_.begin(), chain_.end(), (*l)->id);
    const auto from = static_cast<std::size_t>(cur - chain_.begin());
    const bool instrument = (*l)->info.instrument;
    if (instrument && position != 0)
        return Error("sandbox.chain_order", (*l)->info.name + " is an instrument", "/position",
                     "instruments stay first");
    if (!instrument && position == 0 && from != 0) {
        const Loaded* first = findLoaded(chain_.front());
        if (first && first->info.instrument)
            return Error("sandbox.chain_order",
                         "position 0 is taken by instrument " + label(first->id, first->info),
                         "/position", "effects go after the instrument");
    }
    const std::string moving = *cur;
    chain_.erase(cur);
    chain_.insert(chain_.begin() + static_cast<std::ptrdiff_t>(position), moving);
    return okStatus();
}

// --- parameters ----------------------------------------------------------------------

Result<json::Value> Session::paramGet(std::string_view id, std::span<const std::string> keys) {
    auto lr = loaded(id);
    if (!lr) return std::move(lr).error();
    Loaded& l = **lr;
    host::Instance& in = *l.instance;
    json::Value out = json::Value::object();
    if (l.info.semantic) {
        // The keys, units and curves here do not depend on the patch; the only
        // part of the schema that follows it is each parameter's "active" flag,
        // which param.get does not report (plugin.describe does, and refreshes
        // the cache through Session::schema).
        const json::Value* params = l.schema.find("params");
        auto text = in.getPatch(false);
        if (!text) return std::move(text).error();
        auto doc = json::parse(*text);
        if (!doc) return std::move(doc).error();
        const json::Value* values = doc->find("params");
        std::vector<std::string> wanted(keys.begin(), keys.end());
        if (wanted.empty() && params && params->isObject())
            for (const auto& m : params->asObject()) wanted.push_back(m.key);
        for (std::size_t i = 0; i < wanted.size(); ++i) {
            const std::string& key = wanted[i];
            const json::Value* entry = params ? params->find(key) : nullptr;
            if (!entry) {
                std::vector<std::string> names;
                if (params && params->isObject())
                    for (const auto& m : params->asObject()) names.push_back(m.key);
                return Error("sandbox.unknown_param",
                             label(l.id, l.info) + " has no parameter '" + key + "'",
                             "/keys/" + std::to_string(i),
                             closestHint(key, names, "plugin.describe"));
            }
            json::Value item = json::Value::object();
            const auto pid = static_cast<std::uint32_t>(entry->find("id")->asInt());
            auto clap = in.paramValue(pid);
            if (values && values->find(key)) {
                item["value"] = *values->find(key);  // flat patch: the patch value
            } else if (clap) {
                // Structured patch: the physical value from the CLAP value.
                auto sp = schemaParam(*entry);
                if (!sp) return std::move(sp).error().at("/keys/" + std::to_string(i));
                item["value"] = params::toJson(sp->spec, physicalFromClap(*sp, *clap));
            }
            if (!item.find("value"))
                return Error(
                    "sandbox.plugin_contract",
                    label(l.id, l.info) + " did not provide a current value for '" + key + "'",
                    "/params/" + key + "/value",
                    "Restart or replace the plugin; its patch and CLAP value are both "
                    "unavailable.");
            if (clap) {
                auto t = in.paramText(pid, *clap);
                if (t) item["text"] = *t;
            }
            out[key] = std::move(item);
        }
        return out;
    }
    const auto infos = in.params();
    auto describeOne = [&](const host::ParamInfo& p) -> Result<json::Value> {
        json::Value item = json::Value::object();
        item["name"] = p.name;
        auto v = in.paramValue(p.id);
        if (!v)
            return Error(
                "sandbox.plugin_contract",
                label(l.id, l.info) + " did not provide a current value for '" + p.name + "'",
                "/params/" + std::to_string(p.id) + "/value",
                "Restart or replace the plugin; clap.params.get_value failed.");
        if (v) {
            item["value"] = *v;
            auto t = in.paramText(p.id, *v);
            if (t) item["text"] = *t;
        }
        item["min"] = p.min;
        item["max"] = p.max;
        return item;
    };
    if (keys.empty()) {
        for (const auto& p : infos) {
            auto item = describeOne(p);
            if (!item) return std::move(item).error();
            out[std::to_string(p.id)] = std::move(*item);
        }
        return out;
    }
    for (std::size_t i = 0; i < keys.size(); ++i) {
        const host::ParamInfo* found = nullptr;
        for (const auto& p : infos)
            if (std::to_string(p.id) == keys[i] || p.name == keys[i]) found = &p;
        if (!found) {
            std::vector<std::string> names;
            for (const auto& p : infos) names.push_back(p.name);
            return Error("sandbox.unknown_param",
                         label(l.id, l.info) + " has no parameter '" + keys[i] + "'",
                         "/keys/" + std::to_string(i), closestHint(keys[i], names, "param.get"));
        }
        auto item = describeOne(*found);
        if (!item) return std::move(item).error();
        out[keys[i]] = std::move(*item);
    }
    return out;
}

Result<json::Value> Session::paramSet(std::string_view id, const json::Value& values) {
    auto lr = loaded(id);
    if (!lr) return std::move(lr).error();
    Loaded& l = **lr;
    if (!values.isObject() || values.asObject().empty())
        return Error("sandbox.param_values", "values must be a non-empty object", "/values",
                     R"(e.g. {"cutoff": "800 Hz", "resonance": 0.3})");
    std::vector<std::string> keys;
    for (const auto& m : values.asObject()) keys.push_back(m.key);
    if (l.info.semantic) {
        json::Value merge = json::Value::object();
        merge["params"] = values;
        auto st = l.instance->setPatch(json::writeCompact(merge), CLAP_SUPPLE_SET_MERGE);
        if (!st) return rebase(std::move(st).error(), "/params", "/values");
        auto set = paramGet(id, keys);
        if (!set) return set;
        // A value that does nothing now says so.
        const json::Value* entries = schema(id).find("params");
        for (const std::string& key : keys)
            if (!applies(entries ? entries->find(key) : nullptr)) (*set)[key]["active"] = false;
        return set;
    }
    // Third-party: resolve and validate everything first, then apply.
    const auto infos = l.instance->params();
    std::vector<std::pair<std::uint32_t, double>> apply;
    for (const auto& m : values.asObject()) {
        const std::string path = "/values/" + json::pointerEscape(m.key);
        const host::ParamInfo* p = nullptr;
        for (const auto& c : infos)
            if (std::to_string(c.id) == m.key || c.name == m.key) p = &c;
        if (!p) {
            std::vector<std::string> names;
            for (const auto& c : infos) names.push_back(c.name);
            return Error("sandbox.unknown_param",
                         label(l.id, l.info) + " has no parameter '" + m.key + "'", path,
                         closestHint(m.key, names, "param.get"));
        }
        double v = 0;
        if (m.value.isNumber()) {
            v = m.value.asDouble();
        } else if (m.value.isObject() && m.value.find("text") && m.value.find("text")->isString()) {
            auto parsed = l.instance->paramFromText(p->id, m.value.find("text")->asString());
            if (!parsed)
                return Error("sandbox.param_text",
                             "'" + m.value.find("text")->asString() + "' is not a valid " + p->name,
                             path + "/text",
                             "the plugin formats values like '" +
                                 l.instance->paramText(p->id, p->def).valueOr("?") + "'");
            v = *parsed;
        } else {
            return Error("sandbox.param_value", "value must be a number or {\"text\": \"...\"}",
                         path,
                         "CLAP value in [" + json::formatDouble(p->min) + ", " +
                             json::formatDouble(p->max) + "]");
        }
        if (!(v >= p->min && v <= p->max))
            return Error(
                "sandbox.param_range", p->name + " value out of range", path,
                "[" + json::formatDouble(p->min) + ", " + json::formatDouble(p->max) + "]");
        apply.emplace_back(p->id, v);
    }
    for (const auto& [pid, v] : apply) SUPPLE_TRY(l.instance->setParam(pid, v));
    return paramGet(id, keys);
}

// --- patches -------------------------------------------------------------------------

Result<json::Value> Session::patchGet(std::string_view id, bool changedOnly) {
    auto l = semantic(id);
    if (!l) return std::move(l).error();
    auto text = (*l)->instance->getPatch(changedOnly);
    if (!text) return std::move(text).error();
    return json::parse(*text);
}

Result<std::string> Session::patchSet(std::string_view id, const json::Value& patch) {
    auto l = semantic(id);
    if (!l) return std::move(l).error();
    auto st = (*l)->instance->setPatch(json::writeCompact(patch), CLAP_SUPPLE_SET_REPLACE);
    if (!st) return prefixed(std::move(st).error(), "/patch");
    return (*l)->instance->describe(true);
}

Result<Session::MergeResult> Session::patchMerge(std::string_view id,
                                                 const json::Value& mergePatch) {
    auto l = semantic(id);
    if (!l) return std::move(l).error();
    host::Instance& in = *(*l)->instance;
    auto before = patchGet(id, false);
    if (!before) return std::move(before).error();
    auto st = in.setPatch(json::writeCompact(mergePatch), CLAP_SUPPLE_SET_MERGE);
    if (!st) return prefixed(std::move(st).error(), "/patch");
    auto after = patchGet(id, false);
    if (!after) return std::move(after).error();
    MergeResult r;
    auto changed = json::createMergePatch(*before, *after);
    r.changed = changed ? std::move(*changed) : json::Value::object();
    auto d = in.describe(true);
    if (!d) return std::move(d).error();
    r.description = std::move(*d);
    if (const json::Value* set = mergePatch.find("params"); set && set->isObject()) {
        const json::Value* entries = schema(id).find("params");
        for (const auto& m : set->asObject())
            if (!applies(entries ? entries->find(m.key) : nullptr)) r.inactive.push_back(m.key);
    }
    return r;
}

Result<std::string> Session::describe(std::string_view id, bool changedOnly) {
    auto l = semantic(id);
    if (!l) return std::move(l).error();
    return (*l)->instance->describe(changedOnly);
}

// --- presets -------------------------------------------------------------------------

Result<fs::path> Session::presetSave(std::string_view id, std::string_view name,
                                     std::span<const std::string> tags, std::string_view notes) {
    auto lr = loaded(id);
    if (!lr) return std::move(lr).error();
    Loaded& l = **lr;
    if (!validPresetName(name))
        return Error("sandbox.preset_name", "invalid preset name '" + std::string(name) + "'",
                     "/name",
                     "1-64 characters from A-Z a-z 0-9 space _ . + -, not starting "
                     "with '.' or a space");
    json::Value doc = json::Value::object();
    doc["schema"] = "supple.preset/1.0";
    doc["plugin"] = l.info.id;
    doc["plugin_name"] = l.info.name;
    doc["name"] = std::string(name);
    json::Value t = json::Value::array();
    for (const auto& tag : tags) t.push_back(tag);
    doc["tags"] = std::move(t);
    doc["notes"] = std::string(notes);
    if (l.info.semantic) {
        auto p = patchGet(id, false);
        if (!p) return std::move(p).error();
        doc["patch"] = std::move(*p);
    } else {
        auto s = l.instance->saveState();
        if (!s) return std::move(s).error();
        doc["state"] = base64Encode(*s);
    }
    const fs::path path =
        options_.presetDir / safeDirName(l.info.id) / (std::string(name) + ".json");
    SUPPLE_TRY(writeFileAtomic(path, json::writeCanonical(doc) + "\n"));
    return path;
}

Result<std::string> Session::presetLoad(std::string_view id, std::string_view name, bool factory) {
    auto lr = loaded(id);
    if (!lr) return std::move(lr).error();
    Loaded& l = **lr;
    if (factory) {
        auto patches = l.instance->factoryPatches();
        if (!patches) return std::move(patches).error();
        std::vector<std::string> names;
        for (const auto& patch : patches->asArray()) {
            const auto& candidate = patch.find("name")->asString();
            if (candidate == name) {
                SUPPLE_TRY(
                    l.instance->setPatch(json::writeCanonical(patch), CLAP_SUPPLE_SET_REPLACE));
                return l.instance->describe(true);
            }
            names.push_back(candidate);
        }
        return Error("sandbox.unknown_preset",
                     "no factory preset '" + std::string(name) + "' for " + l.info.name, "/name",
                     names.empty()
                         ? "this plugin has no factory patches; use source: user for saved presets"
                         : closestHint(name, names, "preset.list with source: factory"));
    }
    if (!validPresetName(name))
        return Error("sandbox.preset_name", "invalid preset name '" + std::string(name) + "'",
                     "/name", "see preset.list");
    const fs::path path =
        options_.presetDir / safeDirName(l.info.id) / (std::string(name) + ".json");
    auto text = readTextFile(path, 16u << 20);
    if (!text) {
        auto list = presetList(l.info.id);
        std::vector<std::string> names;
        if (list)
            for (const auto& p : *list) names.push_back(p.name);
        return Error("sandbox.unknown_preset",
                     "no preset '" + std::string(name) + "' for " + l.info.name, "/name",
                     names.empty() ? "no presets saved for this plugin yet"
                                   : closestHint(name, names, "preset.list"));
    }
    auto doc = json::parse(*text);
    if (!doc || !doc->isObject())
        return Error("sandbox.preset_corrupt", path.string() + " is not a valid preset file");
    if (const auto* schema = doc->find("schema");
        schema && (!schema->isString() || schema->asString() != "supple.preset/1.0"))
        return Error("sandbox.preset_schema", "unsupported preset schema", "/schema",
                     "use supple.preset/1.0 or a legacy preset without schema");
    if (const auto* version = doc->find("version");
        version && (!version->isNumber() || version->asDouble() != 1.0))
        return Error("sandbox.preset_version", "unsupported preset version", "/version",
                     "use version 1 or omit version for legacy presets");
    const json::Value* plugin = doc->find("plugin");
    if (!plugin || !plugin->isString() || plugin->asString() != l.info.id)
        return Error("sandbox.preset_plugin", "preset belongs to another plugin", "/name");
    if (const json::Value* patch = doc->find("patch")) {
        auto st = l.instance->setPatch(json::writeCompact(*patch), CLAP_SUPPLE_SET_REPLACE);
        if (!st) return std::move(st).error();
        return l.instance->describe(true);
    }
    const json::Value* state = doc->find("state");
    if (!state || !state->isString())
        return Error("sandbox.preset_corrupt", path.string() + " has neither patch nor state");
    auto bytes = base64Decode(state->asString());
    if (!bytes) return std::move(bytes).error();
    SUPPLE_TRY(l.instance->loadState(*bytes));
    return std::string("loaded preset ") + std::string(name);
}

Result<std::vector<PresetInfo>> Session::presetList(std::string_view plugin, bool factory) {
    std::vector<PresetInfo> out;
    if (factory) {
        if (plugin.empty())
            return Error("sandbox.preset_plugin", "factory preset listing needs a plugin id",
                         "/plugin", "provide plugin from plugin.list and source: factory");
        host::Instance* in = nullptr;
        for (const auto& [id, l] : instances_)
            if (l->info.id == plugin) {
                in = l->instance.get();
                break;
            }
        std::unique_ptr<host::Instance> probe;
        if (!in) {
            auto info = findPlugin(plugin);
            if (!info) return std::move(info).error();
            auto made = host::Instance::create(*info, options_.identity);
            if (!made) return std::move(made).error();
            probe = std::move(*made);
            in = probe.get();
        }
        auto patches = in->factoryPatches();
        if (!patches) return std::move(patches).error();
        for (const auto& patch : patches->asArray()) {
            PresetInfo p;
            p.plugin = in->info().id;
            p.name = patch.find("name")->asString();
            p.readOnly = true;
            if (const auto* tags = patch.find("tags"); tags && tags->isArray())
                for (const auto& t : tags->asArray())
                    if (t.isString()) p.tags.push_back(t.asString());
            out.push_back(std::move(p));
        }
        return out;
    }
    std::error_code ec;
    if (!fs::is_directory(options_.presetDir, ec)) return out;
    for (const auto& dir : fs::directory_iterator(options_.presetDir, ec)) {
        if (!dir.is_directory()) continue;
        if (!plugin.empty() && dir.path().filename() != safeDirName(plugin)) continue;
        for (const auto& f : fs::directory_iterator(dir.path(), ec)) {
            if (f.path().extension() != ".json") continue;
            auto text = readTextFile(f.path(), 16u << 20);
            if (!text) continue;
            auto doc = json::parse(*text);
            if (!doc || !doc->isObject()) continue;
            const json::Value* id = doc->find("plugin");
            const json::Value* name = doc->find("name");
            if (!id || !id->isString() || !name || !name->isString()) continue;
            if (!plugin.empty() && id->asString() != plugin) continue;
            PresetInfo p;
            p.plugin = id->asString();
            p.name = name->asString();
            p.path = f.path();
            if (const json::Value* tags = doc->find("tags"); tags && tags->isArray())
                for (const auto& t : tags->asArray())
                    if (t.isString()) p.tags.push_back(t.asString());
            out.push_back(std::move(p));
        }
    }
    std::sort(out.begin(), out.end(), [](const PresetInfo& a, const PresetInfo& b) {
        return a.plugin != b.plugin ? a.plugin < b.plugin : a.name < b.name;
    });
    return out;
}

// --- rendering -----------------------------------------------------------------------

Result<RenderResult> Session::renderThrough(std::span<host::Instance* const> chain,
                                            const RenderInput& input,
                                            const SessionSettings& settings) {
    if (chain.empty())
        return Error("render.empty_chain", "nothing to render through", {},
                     "load a plugin first (plugin.load)");
    if (!chain.front()) return Error("render.chain", "chain contains a null instance");
    if (!(settings.sampleRate >= 8000 && settings.sampleRate <= 384000))
        return Error("render.sample_rate", "sample rate out of range", {}, "use 8000 .. 384000 Hz");
    if (input.notes && input.audio)
        return Error("render.input", "give either notes or audio, not both");
    const double tempo = input.tempo.value_or(settings.tempo);
    if (!(tempo >= 1 && tempo <= 999))
        return Error("render.tempo", "tempo out of range", "/tempo", "1 .. 999 BPM");
    if (input.tailSeconds && !(*input.tailSeconds >= 0 && *input.tailSeconds <= 60))
        return Error("render.tail", "tail out of range", "/tail_s", "0 .. 60 seconds");
    if (input.seconds && !(*input.seconds > 0 && *input.seconds <= 600))
        return Error("render.length", "length out of range", "/seconds", "(0, 600] seconds");
    host::Instance& first = *chain.front();

    RenderRequest req;
    std::int64_t length = 0;
    if (input.notes) {
        if (!first.acceptsNotes())
            return Error("render.not_instrument", first.info().name + " does not accept notes", {},
                         "load an instrument first in the chain, or render audio_file input");
        const midi::Sequence& seq = *input.notes;
        if (seq.ppq < 1) return Error("render.input", "ppq must be positive");
        const long double maxTicks = (std::int64_t(1) << 25) * static_cast<long double>(tempo) *
                                     seq.ppq / (60 * static_cast<long double>(settings.sampleRate));
        if (seq.length < 0 || seq.length > maxTicks)
            return Error("render.too_long", "sequence length is negative or too long");
        for (const auto& n : seq.notes) {
            if (n.start < 0 || n.length < 0 || n.start > maxTicks || n.length > maxTicks - n.start)
                return Error("render.event_time", "note time is negative or too long");
        }
        for (const auto& c : seq.controls)
            if (c.tick < 0 || c.tick > maxTicks)
                return Error("render.event_time", "control time is negative or too long");
        req.events = sequenceEvents(seq, tempo, settings.sampleRate);
        std::int64_t endTicks = seq.length;
        for (const auto& n : seq.notes) endTicks = std::max(endTicks, n.start + n.length);
        for (const auto& c : seq.controls) endTicks = std::max(endTicks, c.tick);
        length = ticksToSamples(endTicks, seq.ppq, tempo, settings.sampleRate);
    }
    if (input.audio) {
        if (first.audioInputChannels() == 0)
            return Error("render.no_input",
                         first.info().name + " is an instrument and takes no audio", {},
                         "render notes through it, or remove it to process audio with effects");
        req.input = &*input.audio;
        length = static_cast<std::int64_t>(input.audio->frames());
    }
    if (input.seconds) length = std::llround(*input.seconds * settings.sampleRate);
    if (length <= 0 && !input.notes && !input.audio)
        return Error("render.length", "nothing to render", {},
                     "give notes, audio, or a length in seconds");
    req.lengthSamples = length;

    RenderSettings rs;
    rs.sampleRate = settings.sampleRate;
    rs.blockSize = settings.blockSize;
    rs.tempo = tempo;
    rs.numerator = settings.numerator;
    rs.denominator = settings.denominator;
    if (input.meter) {
        const auto& m = *input.meter;
        if (m.numerator < 1 || m.numerator > 64 || m.denominator < 1 || m.denominator > 64 ||
            !isPowerOfTwo(static_cast<unsigned>(m.denominator)))
            return Error("render.meter", "invalid meter", "/meter", "N/D, N 1..64, D a power of 2");
        rs.numerator = static_cast<std::uint16_t>(m.numerator);
        rs.denominator = static_cast<std::uint16_t>(m.denominator);
    }
    rs.tailLimitSeconds = input.tailSeconds.value_or(settings.tailLimitSeconds);
    return sandbox::render(chain, rs, req);
}

Result<const RenderRecord*> Session::render(const RenderInput& input) {
    if (chain_.empty())
        return Error("render.empty_chain", "nothing to render through", {},
                     "load a plugin first (plugin.load)");
    std::vector<host::Instance*> chain;
    for (const auto& id : chain_) chain.push_back(instances_.at(id)->instance.get());
    auto result = renderThrough(chain, input, settings_);
    if (!result) return std::move(result).error();

    RenderRecord rec;
    rec.id = "rd_" + std::to_string(nextRender_++);
    rec.name = input.name;
    rec.path = options_.workdir / "renders" / (rec.id + ".wav");
    rec.sampleRate = result->audio.sampleRate;
    rec.frames = static_cast<std::int64_t>(result->audio.frames());
    rec.seconds = result->audio.seconds();
    rec.peakDbfs = result->peakDbfs;
    rec.latency = result->latency;
    rec.tailSeconds = static_cast<double>(result->tailSamples) / settings_.sampleRate;
    rec.tailCapped = result->tailCapped;
    rec.chain = chain_;
    rec.log = std::move(result->log);
    audio::WavOptions wav;
    wav.format = audio::SampleFormat::Float32;
    SUPPLE_TRY(audio::writeWav(rec.path, result->audio, wav));
    if (options_.recordMidi && input.notes) {
        json::Value midi = json::Value::object();
        midi["schema"] = "supple.render-midi/1.0";
        midi["ppq"] = input.notes->ppq;
        midi["length_ticks"] = input.notes->length;
        const auto meter =
            input.meter.value_or(midi::Meter{settings_.numerator, settings_.denominator});
        midi["meter"] = std::to_string(meter.numerator) + "/" + std::to_string(meter.denominator);
        midi["bar_meters"] = json::Value::array();
        for (const auto& m : input.barMeters)
            midi["bar_meters"].push_back(std::to_string(m.numerator) + "/" +
                                         std::to_string(m.denominator));
        midi["notes"] = json::Value::array();
        for (const auto& note : input.notes->notes) {
            json::Value n = json::Value::object();
            n["start"] = note.start;
            n["length"] = note.length;
            n["pitch"] = static_cast<int>(note.pitch);
            n["velocity"] = static_cast<int>(note.velocity);
            n["channel"] = static_cast<int>(note.channel);
            midi["notes"].push_back(std::move(n));
        }
        auto sidecar = rec.path;
        sidecar.replace_extension(".midi.json");
        auto saved = writeFileAtomic(sidecar, json::writeCanonical(midi));
        if (!saved) {
            std::error_code ec;
            fs::remove(rec.path, ec);
            return std::move(saved).error();
        }
    }
    const std::string id = rec.id;
    auto [it, inserted] = renders_.emplace(id, std::move(rec));
    (void)inserted;
    return &it->second;
}

Result<const RenderRecord*> Session::findRender(std::string_view id) const {
    auto it = renders_.find(std::string(id));
    if (it != renders_.end()) return &it->second;
    std::vector<std::string> ids;
    for (const auto& [rid, r] : renders_) ids.push_back(rid);
    return Error("sandbox.unknown_render", "no render '" + std::string(id) + "'", {},
                 ids.empty() ? "nothing has been rendered yet; use render"
                             : closestHint(id, ids, "session.get"));
}

RenderRecord* Session::mutableRender(std::string_view id) {
    auto it = renders_.find(std::string(id));
    return it == renders_.end() ? nullptr : &it->second;
}

// --- settings / whole session ---------------------------------------------------------

Status Session::setSettings(const SessionSettings& s) {
    SUPPLE_TRY(validate(s));
    const bool reconfigure =
        s.sampleRate != settings_.sampleRate || s.blockSize != settings_.blockSize;
    settings_ = s;
    if (reconfigure)
        for (auto& [id, l] : instances_)
            if (l->instance->active()) l->instance->deactivate();
    return okStatus();
}

json::Value Session::summary() const {
    json::Value v = json::Value::object();
    v["settings"] = toJson(settings_);
    json::Value chain = json::Value::array();
    for (const auto& id : chain_) chain.push_back(id);
    v["chain"] = std::move(chain);
    json::Value ins = json::Value::object();
    for (const auto& [id, l] : instances_) {
        json::Value i = json::Value::object();
        i["plugin"] = l->info.id;
        i["name"] = l->info.name;
        i["instrument"] = l->info.instrument;
        i["semantic"] = l->info.semantic;
        ins[id] = std::move(i);
    }
    v["instances"] = std::move(ins);
    json::Value rds = json::Value::object();
    for (const auto& [id, r] : renders_) {
        json::Value o = json::Value::object();
        if (!r.name.empty()) o["name"] = r.name;
        o["path"] = r.path.string();
        o["seconds"] = r.seconds;
        o["peak_dbfs"] = r.peakDbfs;
        rds[id] = std::move(o);
    }
    v["renders"] = std::move(rds);
    return v;
}

void Session::reset() {
    stopJobs();
    adoptBackgroundRenders();
    txn_.reset();
    chain_.clear();
    instances_.clear();
    std::error_code ec;
    for (const auto& [id, r] : renders_) {
        fs::remove(r.path, ec);
        auto sidecar = r.path;
        sidecar.replace_extension(".midi.json");
        fs::remove(sidecar, ec);
    }
    renders_.clear();
    settings_ = {};
}

// --- transactions ---------------------------------------------------------------------

Status Session::begin() {
    if (txn_)
        return Error("sandbox.transaction_nested", "a transaction is already open", {},
                     "commit or roll back first");
    auto t = snapshot();
    if (!t) return t.error();
    txn_ = std::move(*t);
    return okStatus();
}

Result<std::unique_ptr<Session::Txn>> Session::snapshot() {
    auto t = std::make_unique<Txn>();
    t->settings = settings_;
    t->chain = chain_;
    for (const auto& [id, l] : instances_) {
        t->instanceIds.insert(id);
        t->infos.emplace(id, l->info);
        auto s = l->instance->saveState();
        if (!s) {
            Error error("sandbox.snapshot_failed", "cannot snapshot instance " + id,
                        "/instances/" + id, "repair or remove the instance and retry");
            error.details.push_back(s.error());
            return error;
        }
        t->states.emplace(id, std::move(*s));
    }
    for (const auto& [id, r] : renders_) t->renderIds.insert(id);
    return t;
}

void Session::commit() { txn_.reset(); }

Result<std::function<Status()>> Session::invocationSavepoint() {
    auto captured = snapshot();
    if (!captured) return captured.error();
    std::shared_ptr<Txn> saved(std::move(*captured));
    return std::function<Status()>([this, saved] { return restore(*saved); });
}

Status Session::rollback() {
    if (!txn_) return okStatus();
    auto t = std::move(txn_);
    return restore(*t);
}

Status Session::restore(Txn& snapshotState) {
    auto* t = &snapshotState;
    Error error("sandbox.rollback_failed", "some instances could not be restored", {},
                "reload the listed instances before continuing");
    // Drop instances loaded inside the transaction, bring back removed ones.
    for (auto it = instances_.begin(); it != instances_.end();)
        it = t->instanceIds.count(it->first) ? std::next(it) : instances_.erase(it);
    for (auto& [id, l] : t->removed) instances_.emplace(id, std::move(l));
    for (const auto& [id, info] : t->infos) {
        if (instances_.contains(id)) continue;
        if (txn_) {
            auto removed = txn_->removed.find(id);
            if (removed != txn_->removed.end()) {
                instances_.emplace(id, std::move(removed->second));
                txn_->removed.erase(removed);
                continue;
            }
        }
        auto created = host::Instance::create(info, options_.identity);
        if (!created) {
            Error detail("sandbox.restore_failed", "cannot recreate instance " + id,
                         "/instances/" + id);
            detail.details.push_back(created.error());
            error.details.push_back(std::move(detail));
            continue;
        }
        auto loadedInstance = std::make_unique<Loaded>();
        loadedInstance->id = id;
        loadedInstance->info = (*created)->info();
        loadedInstance->instance = std::move(*created);
        instances_.emplace(id, std::move(loadedInstance));
    }
    for (auto& [id, bytes] : t->states)
        if (auto it = instances_.find(id); it != instances_.end())
            if (auto restored = it->second->instance->loadState(bytes); !restored) {
                Error detail("sandbox.restore_failed", "cannot restore instance " + id,
                             "/instances/" + id);
                detail.details.push_back(restored.error());
                error.details.push_back(std::move(detail));
            }
    std::error_code ec;
    for (auto it = renders_.begin(); it != renders_.end();) {
        if (t->renderIds.count(it->first)) {
            ++it;
        } else {
            fs::remove(it->second.path, ec);
            auto sidecar = it->second.path;
            sidecar.replace_extension(".midi.json");
            fs::remove(sidecar, ec);
            it = renders_.erase(it);
        }
    }
    chain_ = t->chain;
    const bool reconfigure = t->settings.sampleRate != settings_.sampleRate ||
                             t->settings.blockSize != settings_.blockSize;
    settings_ = t->settings;
    if (reconfigure)
        for (auto& [id, l] : instances_)
            if (l->instance->active()) l->instance->deactivate();
    if (!error.details.empty()) return error;
    return okStatus();
}

Result<Session::CloneState> Session::snapshotInstance(std::string_view id) {
    auto lr = loaded(id);
    if (!lr) return std::move(lr).error();
    Loaded& l = **lr;
    auto state = l.instance->saveState();
    if (!state) return std::move(state).error();
    return CloneState{l.info, options_.identity, std::move(*state)};
}

RenderRecord Session::reserveRender(std::string name) {
    RenderRecord rec;
    rec.id = "rd_" + std::to_string(nextRender_++);
    rec.name = std::move(name);
    rec.path = options_.workdir / "renders" / (rec.id + ".wav");
    return rec;
}

void Session::submitRender(RenderRecord record) {
    std::lock_guard lock(backgroundMutex_);
    backgroundRenders_.push_back(std::move(record));
}

void Session::adoptBackgroundRenders() {
    std::vector<RenderRecord> ready;
    {
        std::lock_guard lock(backgroundMutex_);
        ready.swap(backgroundRenders_);
    }
    for (auto& r : ready) {
        const std::string id = r.id;
        renders_[id] = std::move(r);
    }
}

void Session::startJob(std::function<void(supple::StopToken)> work) {
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->finished->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) it->thread.join();
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
    auto finished = std::make_shared<std::atomic<bool>>(false);
    jobs_.push_back(
        JobThread{supple::JoiningThread([w = std::move(work), finished](supple::StopToken st) {
                      w(st);
                      finished->store(true, std::memory_order_release);
                  }),
                  std::move(finished)});
}

void Session::stopJobs() {
    for (auto& t : jobs_) t.thread.request_stop();
    for (auto& t : jobs_)
        if (t.thread.joinable()) t.thread.join();
    jobs_.clear();
}

void Session::tick() {
    adoptBackgroundRenders();
    // Finished job threads are joined and dropped (startJob also prunes).
    for (auto it = jobs_.begin(); it != jobs_.end();) {
        if (it->finished->load(std::memory_order_acquire)) {
            if (it->thread.joinable()) it->thread.join();
            it = jobs_.erase(it);
        } else {
            ++it;
        }
    }
}

}  // namespace supple::sandbox
