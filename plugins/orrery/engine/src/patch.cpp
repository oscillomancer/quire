#include "orrery/patch.h"

#include <algorithm>
#include <cmath>
#include <set>

#include "supple/base/closest.h"

namespace supple::orrery {

using json::Value;
using params::ParamSpec;
using units::Unit;

// --- names -------------------------------------------------------------------------------

namespace {

constexpr std::array<std::string_view, static_cast<std::size_t>(Source::Count)> kSourceNames = {
    "amp",        "env2",   "env3",      "env4",       "lfo1",     "lfo2",
    "lfo3",       "lfo4",   "step1",     "step2",      "velocity", "release_velocity",
    "key",        "random", "gate",      "pressure",   "slide",    "modwheel",
    "expression", "breath", "pitchbend", "aftertouch", "macro1",   "macro2",
    "macro3",     "macro4", "macro5",    "macro6",     "macro7",   "macro8",
    "osc1",       "osc2",   "osc3",      "osc4"};

constexpr std::string_view kOscTypes[] = {"va", "wavetable", "fm", "noise", "sub"};
constexpr std::string_view kFilterTypes[] = {"ladder", "svf", "comb", "formant"};
constexpr std::string_view kTargets[] = {"filter1", "filter2", "both", "direct", "none"};
constexpr std::string_view kRoutings[] = {"serial", "parallel"};
constexpr std::string_view kDriveTypes[] = {"off", "soft", "tape", "fold", "hard"};
constexpr std::string_view kRetriggers[] = {"voice", "free", "mono"};
constexpr std::string_view kQualities[] = {"eco", "normal", "high"};
constexpr std::string_view kFxNames[] = {"chorus", "phaser", "delay", "reverb", "eq"};
constexpr std::string_view kBlocks[] = {"voice",  "oscillators", "filters", "drive",
                                        "amp",    "envelopes",   "lfos",    "steps",
                                        "routes", "macros",      "fx",      "output"};

template <std::size_t N>
std::optional<std::size_t> find(const std::string_view (&names)[N], std::string_view s) {
    for (std::size_t i = 0; i < N; ++i)
        if (names[i] == s) return i;
    return std::nullopt;
}

std::string list(std::span<const std::string_view> names) {
    std::string s;
    for (auto n : names) {
        if (!s.empty()) s += ", ";
        s += n;
    }
    return s;
}

std::string didYouMean(std::string_view input, std::span<const std::string_view> names) {
    std::string best = closestMatch(input, names);
    std::string hint = best.empty() ? std::string() : "did you mean \"" + best + "\"? ";
    return hint + "valid: " + list(names);
}

std::string pointer(std::initializer_list<std::string_view> parts) {
    std::string p;
    for (auto part : parts) {
        p += '/';
        for (char c : part) {
            if (c == '~')
                p += "~0";
            else if (c == '/')
                p += "~1";
            else
                p += c;
        }
    }
    return p;
}

const ParamSpec& spec(std::size_t i) { return table()[i]; }

Slot oscSlotOfSource(Source s) { return osc(static_cast<int>(s) - static_cast<int>(Source::Osc1)); }

// Every destination name a route may use (for closest-match hints).
std::vector<std::string> allDestNames() {
    std::vector<std::string> names;
    for (std::size_t i = 0; i < paramCount(); ++i)
        if (spec(i).flags & params::kModulatable) names.emplace_back(spec(i).key);
    for (int o = 0; o < kOscCount; ++o)
        for (auto suffix : {".pitch", ".fm", ".am"})
            names.push_back(std::string(slotInfo(osc(o)).name) + suffix);
    return names;
}

std::optional<Dest> findDest(std::string_view name) {
    for (int o = 0; o < kOscCount; ++o) {
        const std::string prefix = std::string(slotInfo(osc(o)).name);
        if (name == prefix + ".pitch")
            return Dest{Dest::Kind::Pitch, static_cast<std::uint16_t>(o)};
        if (name == prefix + ".fm") return Dest{Dest::Kind::Fm, static_cast<std::uint16_t>(o)};
        if (name == prefix + ".am") return Dest{Dest::Kind::Am, static_cast<std::uint16_t>(o)};
    }
    if (const ParamSpec* p = params::findByKey(table(), name))
        return Dest{Dest::Kind::Param, static_cast<std::uint16_t>(params::indexOf(table(), *p))};
    return std::nullopt;
}

Slot destSlot(const Dest& d) {
    return d.kind == Dest::Kind::Param ? locate(d.index).slot : osc(d.index);
}

// Source → the slot that must exist for it (kSlotCount when none).
Slot sourceSlot(Source s) {
    const int i = static_cast<int>(s);
    if (s <= Source::Env4) return env(i);
    if (s <= Source::Lfo4) return lfo(i - static_cast<int>(Source::Lfo1));
    if (s <= Source::Step2) return step(i - static_cast<int>(Source::Step1));
    if (s >= Source::Osc1) return oscSlotOfSource(s);
    return kSlotCount;
}

int destOrder(const Dest& d) {
    if (d.kind == Dest::Kind::Param) return static_cast<int>(d.index) * 4;
    return static_cast<int>(index(osc(d.index), OscF::level)) * 4 - 4 + static_cast<int>(d.kind);
}

bool routeLess(const Route& a, const Route& b) {
    if (a.source != b.source) return a.source < b.source;
    return destOrder(a.dest) < destOrder(b.dest);
}

}  // namespace

std::string_view sourceName(Source s) { return kSourceNames[static_cast<std::size_t>(s)]; }

std::optional<Source> findSource(std::string_view name) {
    for (std::size_t i = 0; i < kSourceNames.size(); ++i)
        if (kSourceNames[i] == name) return static_cast<Source>(i);
    return std::nullopt;
}

bool isAudioSource(Source s) { return s >= Source::Osc1 && s <= Source::Osc4; }

std::string destName(const Dest& d) {
    switch (d.kind) {
        case Dest::Kind::Param:
            return std::string(spec(d.index).key);
        case Dest::Kind::Pitch:
            return std::string(slotInfo(osc(d.index)).name) + ".pitch";
        case Dest::Kind::Fm:
            return std::string(slotInfo(osc(d.index)).name) + ".fm";
        case Dest::Kind::Am:
            return std::string(slotInfo(osc(d.index)).name) + ".am";
    }
    return {};
}

Unit modUnit(const Dest& d) {
    switch (d.kind) {
        case Dest::Kind::Pitch:
            return Unit::st;
        case Dest::Kind::Fm:
            return Unit::x;
        case Dest::Kind::Am:
            return Unit::percent;
        case Dest::Kind::Param:
            break;
    }
    const ParamSpec& p = spec(d.index);
    switch (p.unit) {
        case Unit::Hz:
            return Unit::oct;
        case Unit::st:
        case Unit::ct:
            return Unit::st;
        case Unit::dB:
            return Unit::dB;
        default:
            return Unit::percent;
    }
}

std::string_view fxName(FxKind k) { return kFxNames[static_cast<std::size_t>(k)]; }

int oversampling(Quality q) { return q == Quality::Eco ? 1 : q == Quality::Normal ? 2 : 4; }

bool Structure::has(Slot s) const {
    const SlotInfo& info = slotInfo(s);
    switch (info.block) {
        case Block::Osc:
            return osc[s - kOsc1].has_value();
        case Block::Filter:
            return filter[s - kFilter1].has_value();
        case Block::Drive:
            return drive != DriveType::Off;
        case Block::Env:
            return env[s - kEnvAmp];
        case Block::Lfo:
            return lfo[s - kLfo1];
        case Block::Step:
            return step[s - kStep1];
        case Block::Chorus:
            return std::ranges::count(chain, FxKind::Chorus) > 0;
        case Block::Phaser:
            return std::ranges::count(chain, FxKind::Phaser) > 0;
        case Block::Delay:
            return std::ranges::count(chain, FxKind::Delay) > 0;
        case Block::Reverb:
            return std::ranges::count(chain, FxKind::Reverb) > 0;
        case Block::Eq:
            return std::ranges::count(chain, FxKind::Eq) > 0;
        default:
            return true;  // voice, filters, amp, macros, output
    }
}

bool isGlobalSource(Source s, const Structure& st) {
    if (s >= Source::Modwheel && s <= Source::Macro8) return true;
    if (s >= Source::Lfo1 && s <= Source::Lfo4)
        return st.lfoRetrigger[static_cast<int>(s) - static_cast<int>(Source::Lfo1)] !=
               Retrigger::Voice;
    if (s >= Source::Step1 && s <= Source::Step2)
        return st.stepRetrigger[static_cast<int>(s) - static_cast<int>(Source::Step1)] !=
               Retrigger::Voice;
    return false;
}

bool isGlobalDest(const Dest& d, const Structure& st) {
    if (d.kind != Dest::Kind::Param) return false;
    const Slot s = locate(d.index).slot;
    switch (slotInfo(s).block) {
        case Block::Voice:
        case Block::Macro:
        case Block::Chorus:
        case Block::Phaser:
        case Block::Delay:
        case Block::Reverb:
        case Block::Eq:
        case Block::Output:
            return true;
        case Block::Lfo:
            return st.lfoRetrigger[s - kLfo1] != Retrigger::Voice;
        case Block::Step:
            return st.stepRetrigger[s - kStep1] != Retrigger::Voice;
        default:
            return false;
    }
}

double defaultValue(const Structure& st, std::size_t i) {
    const ParamLocation loc = locate(i);
    if (slotInfo(loc.slot).block == Block::Osc &&
        loc.field == static_cast<std::uint16_t>(OscF::wave)) {
        if (const auto& t = st.osc[loc.slot - kOsc1]) return static_cast<double>(defaultWave(*t));
    }
    return spec(i).def;
}

// Does the (existing) slot have this field in its patch block?
static bool fieldApplies(const Structure& st, Slot s, std::uint16_t field) {
    switch (slotInfo(s).block) {
        case Block::Osc: {
            const auto& t = st.osc[s - kOsc1];
            return t && oscFieldApplies(*t, static_cast<OscF>(field)) &&
                   !(s == kOsc1 && field == static_cast<std::uint16_t>(OscF::sync));
        }
        case Block::Filter: {
            const auto& t = st.filter[s - kFilter1];
            return t && filterFieldApplies(*t, static_cast<FilterF>(field));
        }
        default:
            return st.has(s);
    }
}

bool paramActive(const Structure& st, std::size_t index) {
    const ParamLocation loc = locate(index);
    if (!fieldApplies(st, loc.slot, loc.field)) return false;
    // A macro does something only when a route uses it.
    if (slotInfo(loc.slot).block == Block::Macro) {
        const auto src =
            static_cast<Source>(static_cast<int>(Source::Macro1) + (loc.slot - kMacro1));
        return std::ranges::any_of(st.routes,
                                   [&](const Route& r) { return r.source == src || r.via == src; });
    }
    // The filter balance applies to parallel routing.
    if (loc.slot == kFilters) return st.routing == FilterRouting::Parallel;
    return true;
}

std::vector<std::string> patchPath(std::size_t index) {
    const ParamLocation loc = locate(index);
    const SlotInfo& info = slotInfo(loc.slot);
    const std::string field(fieldName(info.block, loc.field));
    const std::string slot(info.name);
    switch (info.block) {
        case Block::Voice:
            return {"voice", field};
        case Block::Osc:
            return {"oscillators", slot, field};
        case Block::Filters:
            return {"filters", field};
        case Block::Filter:
            return {"filters", slot, field};
        case Block::Drive:
            return {"drive", field};
        case Block::Amp:
            return {"amp", field};
        case Block::Env:
            return {"envelopes", slot, field};
        case Block::Lfo:
            return {"lfos", slot, field};
        case Block::Step:
            return {"steps", slot, field};
        case Block::Macro:
            return {"macros", slot, "value"};
        case Block::Output:
            return {"output", field};
        default:
            return {"fx", slot.substr(3), field};  // "fx.chorus" → fx/chorus
    }
}

// Normalizes values so equal patches have equal bodies: absent slots and
// fields a slot's type does not have hold their defaults.
static void normalize(Body& b) {
    for (std::size_t i = 0; i < b.values.size(); ++i) {
        const ParamLocation loc = locate(i);
        if (!fieldApplies(b.structure, loc.slot, loc.field))
            b.values[i] = defaultValue(b.structure, i);
    }
    std::ranges::sort(b.structure.routes, routeLess);
}

// --- decoding ----------------------------------------------------------------------------

namespace {

std::vector<std::string_view> validFieldNames(const Structure& st, Slot s) {
    std::vector<std::string_view> names;
    const Block b = slotInfo(s).block;
    for (std::uint16_t f = 0; f < fieldCount(b); ++f)
        if (fieldApplies(st, s, f)) names.push_back(fieldName(b, f));
    return names;
}

class Decoder {
  public:
    explicit Decoder(Body& body) : b_(body), st_(body.structure) {}

    void fail(Error e) { errors_.push_back(std::move(e)); }
    std::vector<Error>& errors() { return errors_; }

    bool expectObject(const Value& v, const std::string& path, std::string_view what) {
        if (v.isObject()) return true;
        fail(Error("orrery.type", std::string(what) + " must be an object", path,
                   "e.g. {\"level\": -6}"));
        return false;
    }

    template <std::size_t N>
    std::optional<std::size_t> name(const Value& v, const std::string_view (&names)[N],
                                    const std::string& path, std::string_view what) {
        if (!v.isString()) {
            fail(Error("orrery.type", std::string(what) + " must be a string", path,
                       "valid: " + list(names)));
            return std::nullopt;
        }
        if (auto i = find(names, v.asString())) return i;
        fail(Error("orrery.unknown_choice",
                   "\"" + v.asString() + "\" is not a valid " + std::string(what), path,
                   didYouMean(v.asString(), names)));
        return std::nullopt;
    }

    // Fields of an existing slot; `structural` names are skipped (the caller
    // handles them).
    void fields(const Value& obj, Slot s, const std::string& path,
                std::initializer_list<std::string_view> structural) {
        const Block block = slotInfo(s).block;
        for (const auto& m : obj.asObject()) {
            if (std::ranges::find(structural, m.key) != structural.end()) continue;
            const std::string fpath = path + pointer({m.key});
            const auto f = findField(block, m.key);
            if (!f || !fieldApplies(st_, s, *f)) {
                auto names = validFieldNames(st_, s);
                for (auto n : structural) names.push_back(n);
                std::string what = std::string(slotInfo(s).name);
                if (block == Block::Osc && st_.osc[s - kOsc1])
                    what += " (" + std::string(oscTypeName(*st_.osc[s - kOsc1])) + ")";
                if (block == Block::Filter && st_.filter[s - kFilter1])
                    what += " (" + std::string(filterTypeName(*st_.filter[s - kFilter1])) + ")";
                const bool known = f.has_value();
                fail(Error(known ? "orrery.field_not_for_type" : "orrery.unknown_field",
                           "\"" + m.key + "\" is not a field of " + what, fpath,
                           didYouMean(m.key, names)));
                continue;
            }
            const std::size_t i = index(s, *f);
            auto v = params::fromJson(spec(i), m.value);
            if (!v) {
                Error e = std::move(v).error();
                e.path = fpath;
                fail(std::move(e));
                continue;
            }
            b_.values[i] = *v;
            written_.insert(i);
        }
    }

    bool written(std::size_t i) const { return written_.count(i) > 0; }

    void voice(const Value& v) {
        if (!expectObject(v, "/voice", "voice")) return;
        if (const Value* q = v.find("quality"))
            if (auto i = name(*q, kQualities, "/voice/quality", "quality"))
                st_.quality = static_cast<Quality>(*i);
        fields(v, kVoice, "/voice", {"quality"});
    }

    void oscillators(const Value& v) {
        if (!expectObject(v, "/oscillators", "oscillators")) return;
        std::vector<std::pair<const Value*, int>> slots;
        for (const auto& m : v.asObject()) {
            const std::string path = pointer({"oscillators", m.key});
            int o = -1;
            for (int k = 0; k < kOscCount; ++k)
                if (m.key == slotInfo(osc(k)).name) o = k;
            if (o < 0) {
                constexpr std::string_view names[] = {"osc1", "osc2", "osc3", "osc4"};
                fail(Error("orrery.unknown_slot", "\"" + m.key + "\" is not an oscillator slot",
                           path, didYouMean(m.key, names)));
                continue;
            }
            if (!expectObject(m.value, path, m.key)) continue;
            const Value* type = m.value.find("type");
            if (!type) {
                fail(
                    Error("orrery.missing_type", m.key + " needs a type", path + "/type",
                          "valid: " + list(kOscTypes) + R"(; e.g. {"type": "va", "wave": "saw"})"));
                continue;
            }
            if (auto t = name(*type, kOscTypes, path + "/type", "oscillator type")) {
                st_.osc[o] = static_cast<OscType>(*t);
                slots.emplace_back(&m.value, o);
            }
        }
        for (auto [obj, o] : slots) {
            const std::string path = pointer({"oscillators", slotInfo(osc(o)).name});
            if (const Value* to = obj->find("to")) {
                if (auto t = name(*to, kTargets, path + "/to", "oscillator target"))
                    st_.to[o] = static_cast<OscTarget>(*t);
                explicitTo_[o] = true;
            }
            fields(*obj, osc(o), path, {"type", "to"});
            const std::size_t wi = index(osc(o), OscF::wave);
            const OscType t = *st_.osc[o];
            if (oscFieldApplies(t, OscF::wave)) {
                if (!written(wi)) {
                    b_.values[wi] = static_cast<double>(defaultWave(t));
                } else if (!waveApplies(t, static_cast<Wave>(b_.values[wi]))) {
                    std::vector<std::string_view> ok;
                    for (int w = 0; w <= static_cast<int>(Wave::SquareSine); ++w)
                        if (waveApplies(t, static_cast<Wave>(w)))
                            ok.push_back(spec(wi).choices[static_cast<std::size_t>(w)].key);
                    fail(Error(
                        "orrery.wave_not_for_type",
                        std::string(oscTypeName(t)) + " oscillators have no \"" +
                            std::string(
                                spec(wi).choices[static_cast<std::size_t>(b_.values[wi])].key) +
                            "\" wave",
                        path + "/wave", "valid: " + list(ok)));
                }
            }
        }
    }

    void filters(const Value& v) {
        if (!expectObject(v, "/filters", "filters")) return;
        std::vector<std::pair<const Value*, int>> slots;
        for (const auto& m : v.asObject()) {
            const std::string path = pointer({"filters", m.key});
            if (m.key == "routing" || m.key == "balance") continue;
            int f = -1;
            for (int k = 0; k < kFilterCount; ++k)
                if (m.key == slotInfo(filter(k)).name) f = k;
            if (f < 0) {
                constexpr std::string_view names[] = {"routing", "balance", "filter1", "filter2"};
                fail(Error("orrery.unknown_slot", "\"" + m.key + "\" is not a member of filters",
                           path, didYouMean(m.key, names)));
                continue;
            }
            if (!expectObject(m.value, path, m.key)) continue;
            const Value* type = m.value.find("type");
            if (!type) {
                fail(Error("orrery.missing_type", m.key + " needs a type", path + "/type",
                           "valid: " + list(kFilterTypes) +
                               R"(; e.g. {"type": "ladder", "cutoff": "1.2 kHz"})"));
                continue;
            }
            if (auto t = name(*type, kFilterTypes, path + "/type", "filter type")) {
                st_.filter[f] = static_cast<FilterType>(*t);
                slots.emplace_back(&m.value, f);
            }
        }
        if (const Value* r = v.find("routing"))
            if (auto i = name(*r, kRoutings, "/filters/routing", "filter routing"))
                st_.routing = static_cast<FilterRouting>(*i);
        if (const Value* bal = v.find("balance")) {
            const std::size_t i = index(kFilters, FiltersF::balance);
            if (auto x = params::fromJson(spec(i), *bal))
                b_.values[i] = *x;
            else
                fail(std::move(x).error().at("/filters/balance"));
        }
        for (auto [obj, f] : slots)
            fields(*obj, filter(f), pointer({"filters", slotInfo(filter(f)).name}), {"type"});
    }

    void drive(const Value& v) {
        if (!expectObject(v, "/drive", "drive")) return;
        const Value* type = v.find("type");
        if (!type) {
            fail(Error("orrery.missing_type", "drive needs a type", "/drive/type",
                       "valid: " + list(kDriveTypes) + R"(; e.g. {"type": "tape", "amount": 6})"));
            return;
        }
        if (auto t = name(*type, kDriveTypes, "/drive/type", "drive type"))
            st_.drive = static_cast<DriveType>(*t);
        if (st_.drive != DriveType::Off) fields(v, kDrive, "/drive", {"type"});
    }

    void simple(const Value& v, Slot s, std::string_view block) {
        const std::string path = pointer({block});
        if (!expectObject(v, path, block)) return;
        fields(v, s, path, {});
    }

    void envelopes(const Value& v) {
        if (!expectObject(v, "/envelopes", "envelopes")) return;
        for (const auto& m : v.asObject()) {
            const std::string path = pointer({"envelopes", m.key});
            int e = -1;
            for (int k = 0; k < kEnvCount; ++k)
                if (m.key == slotInfo(env(k)).name) e = k;
            if (e < 0) {
                constexpr std::string_view names[] = {"amp", "env2", "env3", "env4"};
                fail(Error("orrery.unknown_slot", "\"" + m.key + "\" is not an envelope slot", path,
                           didYouMean(m.key, names)));
                continue;
            }
            if (!expectObject(m.value, path, m.key)) continue;
            st_.env[e] = true;
            fields(m.value, env(e), path, {});
        }
    }

    template <std::size_t N>
    int slotIndex(std::string_view key, const std::string_view (&names)[N]) {
        for (std::size_t k = 0; k < N; ++k)
            if (key == names[k]) return static_cast<int>(k);
        return -1;
    }

    void lfos(const Value& v) {
        if (!expectObject(v, "/lfos", "lfos")) return;
        constexpr std::string_view names[] = {"lfo1", "lfo2", "lfo3", "lfo4"};
        for (const auto& m : v.asObject()) {
            const std::string path = pointer({"lfos", m.key});
            const int l = slotIndex(m.key, names);
            if (l < 0) {
                fail(Error("orrery.unknown_slot", "\"" + m.key + "\" is not an LFO slot", path,
                           didYouMean(m.key, names)));
                continue;
            }
            if (!expectObject(m.value, path, m.key)) continue;
            st_.lfo[l] = true;
            if (const Value* r = m.value.find("retrigger"))
                if (auto i = name(*r, kRetriggers, path + "/retrigger", "LFO retrigger"))
                    st_.lfoRetrigger[l] = static_cast<Retrigger>(*i);
            fields(m.value, lfo(l), path, {"retrigger"});
        }
    }

    void steps(const Value& v) {
        if (!expectObject(v, "/steps", "steps")) return;
        constexpr std::string_view names[] = {"step1", "step2"};
        for (const auto& m : v.asObject()) {
            const std::string path = pointer({"steps", m.key});
            const int s = slotIndex(m.key, names);
            if (s < 0) {
                fail(Error("orrery.unknown_slot", "\"" + m.key + "\" is not a step slot", path,
                           didYouMean(m.key, names)));
                continue;
            }
            if (!expectObject(m.value, path, m.key)) continue;
            st_.step[s] = true;
            if (const Value* r = m.value.find("retrigger"))
                if (auto i = name(*r, kRetriggers, path + "/retrigger", "step retrigger"))
                    st_.stepRetrigger[s] = static_cast<Retrigger>(*i);
            if (const Value* values = m.value.find("values")) {
                if (!values->isArray()) {
                    fail(Error("orrery.type", "values must be an array of percentages",
                               path + "/values", "e.g. [100, 0, -50, 25]"));
                } else if (values->size() > static_cast<std::size_t>(kMaxSteps)) {
                    fail(Error("orrery.steps.too_many",
                               "a step sequence has at most 16 values, got " +
                                   std::to_string(values->size()),
                               path + "/values", "use `length` to play fewer steps"));
                } else {
                    for (std::size_t k = 0; k < values->size(); ++k) {
                        const Value& x = values->at(k);
                        Result<double> pct =
                            x.isNumber() ? Result<double>(x.asDouble())
                            : x.isString()
                                ? units::parseQuantity(x.asString(), Unit::percent)
                                : Result<double>(Error("orrery.type", "step values are numbers"));
                        const std::string vpath = path + "/values/" + std::to_string(k);
                        if (!pct) {
                            fail(std::move(pct).error().at(vpath));
                        } else if (!(*pct >= -100.0 && *pct <= 100.0)) {
                            fail(Error("orrery.steps.range", "step values are -100..100 %", vpath,
                                       "got " + std::to_string(*pct)));
                        } else {
                            st_.stepValues[s].push_back(*pct);
                        }
                    }
                }
            }
            fields(m.value, step(s), path, {"retrigger", "values"});
        }
    }

    void macros(const Value& v) {
        if (!expectObject(v, "/macros", "macros")) return;
        constexpr std::string_view names[] = {"macro1", "macro2", "macro3", "macro4",
                                              "macro5", "macro6", "macro7", "macro8"};
        for (const auto& m : v.asObject()) {
            const std::string path = pointer({"macros", m.key});
            const int k = slotIndex(m.key, names);
            if (k < 0) {
                fail(Error("orrery.unknown_slot", "\"" + m.key + "\" is not a macro", path,
                           didYouMean(m.key, names)));
                continue;
            }
            if (!expectObject(m.value, path, m.key)) continue;
            if (const Value* n = m.value.find("name")) {
                if (!n->isString() || n->asString().size() > 24)
                    fail(Error("orrery.macro.name", "a macro name is a string of ≤ 24 characters",
                               path + "/name", R"(e.g. "Brightness")"));
                else
                    st_.macroName[k] = n->asString();
            }
            fields(m.value, macro(k), path, {"name"});
        }
    }

    void fx(const Value& v) {
        if (!expectObject(v, "/fx", "fx")) return;
        if (const Value* chain = v.find("chain")) {
            if (!chain->isArray()) {
                fail(Error("orrery.type", "fx.chain must be an array of FX names", "/fx/chain",
                           R"(e.g. ["chorus", "reverb"])"));
            } else {
                for (std::size_t k = 0; k < chain->size(); ++k) {
                    const std::string path = "/fx/chain/" + std::to_string(k);
                    auto i = name(chain->at(k), kFxNames, path, "FX");
                    if (!i) continue;
                    const auto kind = static_cast<FxKind>(*i);
                    if (std::ranges::count(st_.chain, kind))
                        fail(Error("orrery.fx.duplicate",
                                   std::string(fxName(kind)) + " appears twice in fx.chain", path,
                                   "each FX runs at most once"));
                    else
                        st_.chain.push_back(kind);
                }
            }
        }
        for (const auto& m : v.asObject()) {
            if (m.key == "chain") continue;
            const std::string path = pointer({"fx", m.key});
            auto i = find(kFxNames, m.key);
            if (!i) {
                std::vector<std::string_view> names(std::begin(kFxNames), std::end(kFxNames));
                names.insert(names.begin(), "chain");
                fail(Error("orrery.unknown_slot", "\"" + m.key + "\" is not a member of fx", path,
                           didYouMean(m.key, names)));
                continue;
            }
            const auto kind = static_cast<FxKind>(*i);
            if (!std::ranges::count(st_.chain, kind)) {
                fail(Error("orrery.fx.not_in_chain", m.key + " is not in fx.chain", path,
                           "add \"" + m.key + "\" to fx.chain, or remove the block"));
                continue;
            }
            if (!expectObject(m.value, path, m.key)) continue;
            fields(m.value, static_cast<Slot>(kChorus + static_cast<int>(kind)), path, {});
        }
    }

    // After the other blocks: explicit targets other than filter1 must exist.
    void checkTargets() {
        for (int o = 0; o < kOscCount; ++o) {
            if (!st_.osc[o] || !explicitTo_[o]) continue;
            const OscTarget t = st_.to[o];
            const std::string path = pointer({"oscillators", slotInfo(osc(o)).name, "to"});
            auto need = [&](int f) {
                if (!st_.filter[f])
                    fail(Error("orrery.target_missing",
                               std::string(slotInfo(osc(o)).name) + " goes to " +
                                   std::string(slotInfo(filter(f)).name) +
                                   ", which is not in this patch",
                               path,
                               "add filters." + std::string(slotInfo(filter(f)).name) +
                                   ", or set \"to\" to \"direct\""));
            };
            // filter1 is the default target and passes straight through when
            // no filter exists, so naming it is never an error.
            if (t == OscTarget::Filter2) need(1);
            if (t == OscTarget::Both) {
                need(0);
                need(1);
            }
        }
    }

    void routes(const Value& v) {
        if (!expectObject(v, "/routes", "routes")) return;
        if (v.asObject().size() > kMaxRoutes)
            fail(Error("orrery.route.too_many",
                       "at most 64 routes, got " + std::to_string(v.asObject().size()), "/routes",
                       "combine routes through a macro"));
        static const std::vector<std::string> destNames = allDestNames();
        std::vector<std::string_view> destViews(destNames.begin(), destNames.end());
        for (const auto& m : v.asObject()) {
            const std::string path = pointer({"routes", m.key});
            const auto gt = m.key.find('>');
            if (gt == std::string::npos) {
                fail(Error("orrery.route.key", "a route key is \"<source>><destination>\"", path,
                           R"(e.g. "env2>filter1.cutoff")"));
                continue;
            }
            const std::string srcName = m.key.substr(0, gt), dstName = m.key.substr(gt + 1);
            Route r;
            auto src = findSource(srcName);
            if (!src) {
                fail(Error("orrery.route.source", "unknown modulation source \"" + srcName + "\"",
                           path, didYouMean(srcName, kSourceNames)));
                continue;
            }
            r.source = *src;
            auto dst = findDest(dstName);
            if (!dst) {
                fail(Error("orrery.route.dest",
                           "unknown modulation destination \"" + dstName + "\"", path,
                           didYouMean(dstName, destViews)));
                continue;
            }
            r.dest = *dst;
            if (!checkRoute(r, srcName, dstName, path)) continue;
            if (!m.value.isObject()) {
                fail(Error("orrery.type", "a route is an object with an amount", path,
                           R"(e.g. {"amount": "+2 oct"})"));
                continue;
            }
            bool ok = true;
            for (const auto& f : m.value.asObject()) {
                if (f.key == "amount") {
                    auto a = amount(f.value, r.dest, path + "/amount");
                    if (a)
                        r.amount = *a;
                    else
                        ok = false;
                } else if (f.key == "via") {
                    auto via = f.value.isString() ? findSource(f.value.asString()) : std::nullopt;
                    if (!via || isAudioSource(*via)) {
                        fail(Error("orrery.route.via",
                                   "via must name a control source (e.g. \"modwheel\")",
                                   path + "/via",
                                   f.value.isString() ? didYouMean(f.value.asString(), kSourceNames)
                                                      : std::string("a source name")));
                        ok = false;
                    } else if (!checkSource(*via, f.value.asString(), path + "/via") ||
                               !checkScope(*via, r.dest, f.value.asString(), dstName,
                                           path + "/via")) {
                        ok = false;
                    } else {
                        r.via = *via;
                    }
                } else if (f.key == "curve") {
                    auto c = f.value.isNumber() ? Result<double>(f.value.asDouble())
                             : f.value.isString()
                                 ? units::parseQuantity(f.value.asString(), Unit::percent)
                                 : Result<double>(Error("orrery.type", "curve is a percentage"));
                    if (!c || !(*c >= -100.0 && *c <= 100.0)) {
                        fail(Error("orrery.route.curve", "curve is -100..100 %", path + "/curve",
                                   "0 is linear; positive bends toward exponential"));
                        ok = false;
                    } else {
                        r.curve = *c;
                    }
                } else {
                    constexpr std::string_view names[] = {"amount", "via", "curve"};
                    fail(Error("orrery.route.field", "\"" + f.key + "\" is not a route field",
                               path + pointer({f.key}), didYouMean(f.key, names)));
                    ok = false;
                }
            }
            if (!m.value.find("amount")) {
                fail(Error("orrery.route.amount", "the route needs an amount", path + "/amount",
                           "in " + std::string(units::unitName(modUnit(r.dest))) + ", e.g. " +
                               example(modUnit(r.dest))));
                ok = false;
            }
            if (ok) st_.routes.push_back(r);
        }
    }

  private:
    static std::string example(Unit u) {
        switch (u) {
            case Unit::oct:
                return "\"+1.5 oct\"";
            case Unit::st:
                return "\"+7 st\"";
            case Unit::dB:
                return "\"-6 dB\"";
            case Unit::x:
                return "\"2 x\"";
            default:
                return "\"25 %\"";
        }
    }

    bool checkSource(Source s, const std::string& name, const std::string& path) {
        const Slot slot = sourceSlot(s);
        if (slot == kSlotCount || st_.has(slot)) return true;
        std::string where = slotInfo(slot).block == Block::Osc   ? "oscillators"
                            : slotInfo(slot).block == Block::Env ? "envelopes"
                            : slotInfo(slot).block == Block::Lfo ? "lfos"
                                                                 : "steps";
        fail(Error("orrery.route.source_missing", name + " is not in this patch", path,
                   "add it to " + where + ", or remove the route"));
        return false;
    }

    bool checkScope(Source s, const Dest& d, const std::string& srcName, const std::string& dstName,
                    const std::string& path) {
        if (!isGlobalDest(d, st_) || isGlobalSource(s, st_)) return true;
        fail(Error("orrery.route.voice_to_global",
                   srcName + " is a per-voice source; " + dstName +
                       " is global and takes only global sources",
                   path,
                   "global sources: modwheel, expression, breath, pitchbend, aftertouch, "
                   "macro1..macro8, and LFOs/steps with retrigger free or mono"));
        return false;
    }

    bool checkRoute(const Route& r, const std::string& srcName, const std::string& dstName,
                    const std::string& path) {
        if (!checkSource(r.source, srcName, path)) return false;
        const Slot ds = destSlot(r.dest);
        if (!st_.has(ds)) {
            const Block b = slotInfo(ds).block;
            std::string where = b == Block::Osc      ? "oscillators"
                                : b == Block::Filter ? "filters"
                                : b == Block::Env    ? "envelopes"
                                : b == Block::Lfo    ? "lfos"
                                : b == Block::Step   ? "steps"
                                : b == Block::Drive  ? "drive (with a type)"
                                                     : "fx.chain";
            fail(Error("orrery.route.dest_missing",
                       std::string(slotInfo(ds).name) + " is not in this patch", path,
                       "add it to " + where + ", or remove the route"));
            return false;
        }
        if (r.dest.kind == Dest::Kind::Param) {
            const ParamLocation loc = locate(r.dest.index);
            const ParamSpec& p = spec(r.dest.index);
            if (!fieldApplies(st_, loc.slot, loc.field)) {
                auto names = validFieldNames(st_, loc.slot);
                fail(Error(
                    "orrery.route.dest_field",
                    dstName + " does not apply to this " + std::string(slotInfo(loc.slot).name),
                    path, "its fields: " + list(names)));
                return false;
            }
            if (!(p.flags & params::kModulatable)) {
                const bool coarse = slotInfo(loc.slot).block == Block::Osc &&
                                    loc.field == static_cast<std::uint16_t>(OscF::coarse);
                fail(Error("orrery.route.not_modulatable",
                           dstName + " is stepped and cannot be modulated", path,
                           coarse ? std::string(slotInfo(loc.slot).name) +
                                        ".pitch modulates the oscillator's pitch in semitones"
                                  : std::string("modulate a continuous field, or use a macro")));
                return false;
            }
        }
        const bool audio = isAudioSource(r.source);
        const bool audioDest = r.dest.kind == Dest::Kind::Fm || r.dest.kind == Dest::Kind::Am;
        if (audio && !audioDest) {
            fail(
                Error("orrery.route.audio_source",
                      srcName + " is an audio-rate source; it can only modulate oscN.fm or oscN.am",
                      path, "e.g. \"" + srcName + ">osc1.fm\""));
            return false;
        }
        if (!audio && audioDest) {
            fail(Error("orrery.route.audio_dest", dstName + " takes an oscillator as its source",
                       path,
                       r.dest.kind == Dest::Kind::Fm
                           ? "e.g. \"osc2>" + dstName +
                                 "\"; to shape the FM depth, modulate the modulator's level "
                                 "(e.g. \"env2>osc2.level\")"
                           : "e.g. \"osc2>" + dstName + "\""));
            return false;
        }
        return checkScope(r.source, r.dest, srcName, dstName, path);
    }

    Result<double> amount(const Value& v, const Dest& d, const std::string& path) {
        const Unit u = modUnit(d);
        const std::string name = destName(d);
        auto wrong = [&](std::string got) {
            fail(Error("orrery.route.unit",
                       "the amount for " + name + " is in " + std::string(units::unitName(u)) +
                           " (" + std::string(units::dimensionName(u)) + ")" + got,
                       path, "e.g. " + example(u)));
            return Error("orrery.route.unit", "reported");
        };
        double x = 0.0;
        if (v.isNumber()) {
            x = v.asDouble();
        } else if (v.isString()) {
            auto parsed = units::parseQuantity(v.asString(), u);
            // Pitch-family units convert into each other (8 ct → 0.08 st).
            if (!parsed && (u == Unit::st || u == Unit::oct)) {
                const double perSt = u == Unit::st ? 1.0 : 1.0 / 12.0;
                if (auto st = units::parseQuantity(v.asString(), Unit::st))
                    parsed = *st * perSt;
                else if (auto ct = units::parseQuantity(v.asString(), Unit::ct))
                    parsed = *ct / 100.0 * perSt;
                else if (auto oct = units::parseQuantity(v.asString(), Unit::oct))
                    parsed = *oct * 12.0 * perSt;
            }
            if (!parsed) return wrong(", not \"" + v.asString() + "\"");
            x = *parsed;
        } else {
            return wrong("");
        }
        const double limit = u == Unit::oct  ? 10.0
                             : u == Unit::st ? 96.0
                             : u == Unit::dB ? 96.0
                             : u == Unit::x  ? 20.0
                                             : 100.0;
        if (!std::isfinite(x) || std::fabs(x) > limit) {
            fail(Error("orrery.route.range",
                       "the amount for " + name + " is ±" + units::formatQuantity(limit, u), path,
                       "got " + units::formatQuantity(x, u)));
            return Error("orrery.route.range", "reported");
        }
        return x;
    }

    Body& b_;
    Structure& st_;
    std::vector<Error> errors_;
    std::set<std::size_t> written_;
    std::array<bool, kOscCount> explicitTo_{};
};

Body defaultBody() {
    Body b;
    b.values.resize(paramCount());
    for (std::size_t i = 0; i < b.values.size(); ++i) b.values[i] = spec(i).def;
    return b;
}

}  // namespace

// --- encoding ----------------------------------------------------------------------------

namespace {

Value fieldValue(std::size_t i, double v) {
    const ParamSpec& p = spec(i);
    if (writesLabel(i) && p.type == params::Type::Enum) {
        const auto k = static_cast<std::size_t>(v);
        if (k < p.choices.size()) return Value(std::string(p.choices[k].label));
    }
    return params::toJson(p, v);
}

void writeFields(Value& obj, const Body& b, Slot s, patch::Detail detail) {
    const Block block = slotInfo(s).block;
    for (std::uint16_t f = 0; f < fieldCount(block); ++f) {
        if (!fieldApplies(b.structure, s, f)) continue;
        const std::size_t i = index(s, f);
        if (detail == patch::Detail::Changed && b.values[i] == defaultValue(b.structure, i))
            continue;
        obj[fieldName(block, f)] = fieldValue(i, b.values[i]);
    }
}

bool nonEmpty(const Value& v) { return v.isObject() && v.size() > 0; }

Value encodeBody(const Body& b, patch::Detail detail) {
    const Structure& st = b.structure;
    const bool full = detail == patch::Detail::Full;
    Value out = Value::object();

    Value voice = Value::object();
    if (full || st.quality != Quality::Normal)
        voice["quality"] = kQualities[static_cast<int>(st.quality)];
    writeFields(voice, b, kVoice, detail);
    if (full || nonEmpty(voice)) out["voice"] = std::move(voice);

    Value oscs = Value::object();
    for (int o = 0; o < kOscCount; ++o) {
        if (!st.osc[o]) continue;
        Value slot = Value::object();
        slot["type"] = kOscTypes[static_cast<int>(*st.osc[o])];
        writeFields(slot, b, osc(o), detail);
        if (full || st.to[o] != OscTarget::Filter1)
            slot["to"] = kTargets[static_cast<int>(st.to[o])];
        oscs[slotInfo(osc(o)).name] = std::move(slot);
    }
    if (full || nonEmpty(oscs)) out["oscillators"] = std::move(oscs);

    Value filters = Value::object();
    if (full || st.routing != FilterRouting::Serial)
        filters["routing"] = kRoutings[static_cast<int>(st.routing)];
    writeFields(filters, b, kFilters, detail);
    for (int f = 0; f < kFilterCount; ++f) {
        if (!st.filter[f]) continue;
        Value slot = Value::object();
        slot["type"] = kFilterTypes[static_cast<int>(*st.filter[f])];
        writeFields(slot, b, filter(f), detail);
        filters[slotInfo(filter(f)).name] = std::move(slot);
    }
    if (full || nonEmpty(filters)) out["filters"] = std::move(filters);

    if (st.drive != DriveType::Off) {
        Value drive = Value::object();
        drive["type"] = kDriveTypes[static_cast<int>(st.drive)];
        writeFields(drive, b, kDrive, detail);
        out["drive"] = std::move(drive);
    }

    Value amp = Value::object();
    writeFields(amp, b, kAmp, detail);
    if (full || nonEmpty(amp)) out["amp"] = std::move(amp);

    Value envs = Value::object();
    for (int e = 0; e < kEnvCount; ++e) {
        if (!st.env[e]) continue;
        Value slot = Value::object();
        writeFields(slot, b, env(e), detail);
        if (e == 0 && !full && !nonEmpty(slot)) continue;
        envs[slotInfo(env(e)).name] = std::move(slot);
    }
    if (full || nonEmpty(envs)) out["envelopes"] = std::move(envs);

    Value lfos = Value::object();
    for (int l = 0; l < kLfoCount; ++l) {
        if (!st.lfo[l]) continue;
        Value slot = Value::object();
        writeFields(slot, b, lfo(l), detail);
        if (full || st.lfoRetrigger[l] != Retrigger::Voice)
            slot["retrigger"] = kRetriggers[static_cast<int>(st.lfoRetrigger[l])];
        lfos[slotInfo(lfo(l)).name] = std::move(slot);
    }
    if (full || nonEmpty(lfos)) out["lfos"] = std::move(lfos);

    Value steps = Value::object();
    for (int s = 0; s < kStepCount; ++s) {
        if (!st.step[s]) continue;
        Value slot = Value::object();
        writeFields(slot, b, step(s), detail);
        if (full || st.stepRetrigger[s] != Retrigger::Voice)
            slot["retrigger"] = kRetriggers[static_cast<int>(st.stepRetrigger[s])];
        if (full || !st.stepValues[s].empty()) {
            Value values = Value::array();
            for (double v : st.stepValues[s]) values.push_back(v);
            slot["values"] = std::move(values);
        }
        steps[slotInfo(step(s)).name] = std::move(slot);
    }
    if (full || nonEmpty(steps)) out["steps"] = std::move(steps);

    Value routes = Value::object();
    for (const Route& r : st.routes) {
        Value route = Value::object();
        route["amount"] = r.amount;
        if (r.via) route["via"] = sourceName(*r.via);
        if (r.curve != 0.0) route["curve"] = r.curve;
        routes[std::string(sourceName(r.source)) + ">" + destName(r.dest)] = std::move(route);
    }
    if (full || nonEmpty(routes)) out["routes"] = std::move(routes);

    Value macros = Value::object();
    for (int m = 0; m < kMacroCount; ++m) {
        Value slot = Value::object();
        if (full || !st.macroName[m].empty())
            slot["name"] =
                st.macroName[m].empty() ? "Macro " + std::to_string(m + 1) : st.macroName[m];
        writeFields(slot, b, macro(m), detail);
        if (full || nonEmpty(slot)) macros[slotInfo(macro(m)).name] = std::move(slot);
    }
    if (full || nonEmpty(macros)) out["macros"] = std::move(macros);

    Value fx = Value::object();
    if (full || !st.chain.empty()) {
        Value chain = Value::array();
        for (FxKind k : st.chain) chain.push_back(fxName(k));
        fx["chain"] = std::move(chain);
    }
    for (FxKind k : st.chain) {
        Value slot = Value::object();
        writeFields(slot, b, static_cast<Slot>(kChorus + static_cast<int>(k)), detail);
        fx[fxName(k)] = std::move(slot);
    }
    if (full || nonEmpty(fx)) out["fx"] = std::move(fx);

    Value output = Value::object();
    writeFields(output, b, kOutput, detail);
    if (full || nonEmpty(output)) out["output"] = std::move(output);
    return out;
}

// A macro name equal to the default "Macro N" is the default.
void canonicalMacroNames(Structure& st) {
    for (int m = 0; m < kMacroCount; ++m)
        if (st.macroName[m] == "Macro " + std::to_string(m + 1)) st.macroName[m].clear();
}

}  // namespace

// --- PatchType ---------------------------------------------------------------------------

PatchType::PatchType() : codec_("orrery", 1, 0, true, {}) {}

Patch PatchType::defaults() const {
    Patch p;
    p.body = defaultBody();
    p.body.structure.osc[0] = OscType::Va;
    p.body.values[index(osc(0), OscF::wave)] = static_cast<double>(Wave::Saw);
    return p;
}

Result<Patch> PatchType::fromJson(const json::Value& document,
                                  std::vector<patch::Warning>* warnings) const {
    auto split = codec_.split(document, warnings);
    if (!split) return std::move(split).error();
    Patch p;
    p.meta = std::move(split->meta);
    p.body = defaultBody();
    Decoder d(p.body);
    const Value& body = split->body;
    if (!body.isObject()) return Error("orrery.type", "a patch is a JSON object", "");
    for (const auto& m : body.asObject()) {
        if (!find(kBlocks, m.key))
            d.fail(Error("orrery.unknown_block", "\"" + m.key + "\" is not a patch block",
                         pointer({m.key}), didYouMean(m.key, kBlocks)));
    }
    // Slots first (routes and targets refer to them), routes last.
    if (const Value* v = body.find("voice")) d.voice(*v);
    if (const Value* v = body.find("oscillators")) d.oscillators(*v);
    if (const Value* v = body.find("filters")) d.filters(*v);
    if (const Value* v = body.find("drive")) d.drive(*v);
    if (const Value* v = body.find("amp")) d.simple(*v, kAmp, "amp");
    if (const Value* v = body.find("envelopes")) d.envelopes(*v);
    if (const Value* v = body.find("lfos")) d.lfos(*v);
    if (const Value* v = body.find("steps")) d.steps(*v);
    if (const Value* v = body.find("macros")) d.macros(*v);
    if (const Value* v = body.find("fx")) d.fx(*v);
    if (const Value* v = body.find("output")) d.simple(*v, kOutput, "output");
    d.checkTargets();
    if (const Value* v = body.find("routes")) d.routes(*v);
    // Phaser stages are even.
    const std::size_t stages = index(kPhaser, PhaserF::stages);
    if (p.body.structure.has(kPhaser) && static_cast<int>(p.body.values[stages]) % 2 != 0)
        d.fail(Error("orrery.fx.stages", "phaser stages must be even", "/fx/phaser/stages",
                     "2, 4, 6, 8, 10 or 12"));
    auto& errors = d.errors();
    if (errors.size() == 1) return std::move(errors.front());
    if (!errors.empty()) {
        Error e("orrery.patch.invalid", std::to_string(errors.size()) + " problems in the patch",
                errors.front().path, "see details");
        e.details = std::move(errors);
        return e;
    }
    canonicalMacroNames(p.body.structure);
    normalize(p.body);
    return p;
}

Result<Patch> PatchType::parse(std::string_view text, std::vector<patch::Warning>* warnings) const {
    auto v = json::parse(text);
    if (!v) return std::move(v).error();
    return fromJson(*v, warnings);
}

json::Value PatchType::toJson(const Patch& p, patch::Detail detail) const {
    return codec_.join(p.meta, encodeBody(p.body, detail));
}

std::string PatchType::serialize(const Patch& p, patch::Detail detail) const {
    return json::writeCanonical(toJson(p, detail));
}

Result<Patch> PatchType::merge(const Patch& current, const json::Value& incoming) const {
    SUPPLE_TRY(codec_.checkMergePatch(incoming));
    // Flat "params" (keys as in the parameter table) → nested members.
    Value mergePatch = incoming;
    if (const Value* flat = incoming.find("params")) {
        if (!flat->isObject())
            return Error("orrery.params", "params must be an object of parameter keys", "/params",
                         R"(e.g. {"params": {"filter1.cutoff": "800 Hz"}})");
        Value nested = mergePatch;
        nested.asObject().erase("params");
        std::vector<std::string_view> keys;
        for (const ParamSpec& p : table()) keys.push_back(p.key);
        for (const auto& m : flat->asObject()) {
            const std::string path = pointer({"params", m.key});
            const ParamSpec* p = params::findByKey(table(), m.key);
            if (!p)
                return Error("orrery.unknown_param", "unknown parameter \"" + m.key + "\"", path,
                             didYouMean(m.key, keys));
            const std::size_t i = params::indexOf(table(), *p);
            if (!paramActive(current.body.structure, i)) {
                const Slot s = locate(i).slot;
                return Error("orrery.param_inactive",
                             m.key + " belongs to " + std::string(slotInfo(s).name) +
                                 ", which is not in this patch (or has no such field for its type)",
                             path, "add the slot (or change its type) with a patch merge first");
            }
            Value* at = &nested;
            const auto parts = patchPath(i);
            for (std::size_t k = 0; k + 1 < parts.size(); ++k) {
                if (!at->find(parts[k]) || !(*at)[parts[k]].isObject())
                    (*at)[parts[k]] = Value::object();
                at = &(*at)[parts[k]];
            }
            (*at)[parts.back()] = m.value;
        }
        mergePatch = std::move(nested);
    }
    Value base = toJson(current, patch::Detail::Changed);
    // A new slot type drops the old type's fields (and a wave the new type
    // cannot take); FX leaving the chain drop their blocks.
    auto retype = [&](std::string_view block, int count, auto slotOf, auto applies) {
        const Value* mp = mergePatch.find(block);
        Value* bb = base.asObject().find(block);
        if (!mp || !mp->isObject() || !bb || !bb->isObject()) return;
        for (int k = 0; k < count; ++k) {
            const std::string_view name = slotInfo(slotOf(k)).name;
            const Value* ms = mp->find(name);
            Value* bs = bb->asObject().find(name);
            if (!ms || !ms->isObject() || !bs || !bs->isObject()) continue;
            const Value* newType = ms->find("type");
            const Value* oldType = bs->find("type");
            if (!newType || !newType->isString() || !oldType || *newType == *oldType) continue;
            std::vector<std::string> drop;
            for (const auto& m : bs->asObject())
                if (m.key != "type" && m.key != "to" &&
                    !applies(newType->asString(), m.key, m.value))
                    drop.push_back(m.key);
            for (const auto& key : drop) bs->asObject().erase(key);
        }
    };
    retype("oscillators", kOscCount, osc,
           [](const std::string& type, const std::string& field, const Value& value) {
               auto t = find(kOscTypes, type);
               auto f = findField(Block::Osc, field);
               if (!t || !f) return true;  // leave it for validation to report
               const auto ot = static_cast<OscType>(*t);
               if (!oscFieldApplies(ot, static_cast<OscF>(*f))) return false;
               if (static_cast<OscF>(*f) == OscF::wave && value.isString()) {
                   auto w = params::fromJson(spec(index(kOsc1, OscF::wave)), value);
                   return w && waveApplies(ot, static_cast<Wave>(*w));
               }
               return true;
           });
    retype("filters", kFilterCount, filter,
           [](const std::string& type, const std::string& field, const Value&) {
               auto t = find(kFilterTypes, type);
               auto f = findField(Block::Filter, field);
               if (!t || !f) return true;
               return filterFieldApplies(static_cast<FilterType>(*t), static_cast<FilterF>(*f));
           });
    if (const Value* mfx = mergePatch.find("fx"); mfx && mfx->isObject()) {
        if (const Value* chain = mfx->find("chain"); chain && chain->isArray()) {
            if (Value* bfx = base.asObject().find("fx"); bfx && bfx->isObject()) {
                for (auto name : kFxNames) {
                    bool kept = false;
                    for (const auto& c : chain->asArray())
                        if (c.isString() && c.asString() == name) kept = true;
                    if (!kept) bfx->asObject().erase(name);
                }
            }
        }
    }
    return fromJson(json::mergePatch(base, mergePatch));
}

json::Value PatchType::diff(const Patch& a, const Patch& b) const {
    auto p = json::createMergePatch(toJson(a, patch::Detail::Changed),
                                    toJson(b, patch::Detail::Changed));
    return p ? std::move(*p) : Value::object();
}

namespace {

Value fieldSchema(std::size_t i) {
    const ParamSpec& p = spec(i);
    Value s = Value::object();
    std::string desc(p.description);
    switch (p.type) {
        case params::Type::Enum: {
            Value choices = Value::array();
            for (const auto& c : p.choices)
                choices.push_back(writesLabel(i) ? std::string(c.label) : std::string(c.key));
            s["enum"] = std::move(choices);
            break;
        }
        case params::Type::Bool:
            s["type"] = "boolean";
            break;
        case params::Type::Int:
            s["type"] = "integer";
            s["minimum"] = p.min;
            s["maximum"] = p.max;
            break;
        case params::Type::Float: {
            Value any = Value::array();
            Value num = Value::object();
            num["type"] = "number";
            num["minimum"] = p.min;
            num["maximum"] = p.max;
            any.push_back(std::move(num));
            Value str = Value::object();
            str["type"] = "string";
            any.push_back(std::move(str));
            s["anyOf"] = std::move(any);
            if (p.unit != Unit::none)
                desc += " Unit " + std::string(units::unitName(p.unit)) +
                        " (strings with units accepted).";
            break;
        }
    }
    Value defv = params::toJson(p, p.def);
    s["default"] = std::move(defv);
    s["description"] = desc;
    return s;
}

Value slotSchema(Slot s, std::initializer_list<std::pair<std::string_view, Value>> extra) {
    Value obj = Value::object();
    obj["type"] = "object";
    Value props = Value::object();
    for (const auto& [k, v] : extra) props[k] = v;
    const Block b = slotInfo(s).block;
    for (std::uint16_t f = 0; f < fieldCount(b); ++f)
        props[fieldName(b, f)] = fieldSchema(index(s, f));
    obj["properties"] = std::move(props);
    obj["additionalProperties"] = false;
    return obj;
}

Value enumSchema(std::span<const std::string_view> names, std::string_view description) {
    Value s = Value::object();
    Value e = Value::array();
    for (auto n : names) e.push_back(n);
    s["enum"] = std::move(e);
    s["description"] = description;
    return s;
}

Value slots(std::initializer_list<std::pair<std::string_view, Value>> members,
            std::string_view description) {
    Value obj = Value::object();
    obj["type"] = "object";
    obj["description"] = description;
    Value props = Value::object();
    for (const auto& [k, v] : members) props[k] = v;
    obj["properties"] = std::move(props);
    obj["additionalProperties"] = false;
    return obj;
}

}  // namespace

json::Value PatchType::jsonSchema() const {
    Value s = Value::object();
    s["$schema"] = "https://json-schema.org/draft/2020-12/schema";
    s["type"] = "object";
    Value props = codec_.envelopeProperties();
    props["voice"] = slotSchema(
        kVoice, {{"quality", enumSchema(kQualities, "Oversampling: eco 1x, normal 2x, high 4x.")}});
    auto oscSlot = [&](int o) {
        return slotSchema(
            osc(o),
            {{"type", enumSchema(kOscTypes, "Oscillator type (required).")},
             {"to",
              enumSchema(kTargets, "Where the oscillator goes; none = FM/AM modulator only.")}});
    };
    props["oscillators"] = slots(
        {{"osc1", oscSlot(0)}, {"osc2", oscSlot(1)}, {"osc3", oscSlot(2)}, {"osc4", oscSlot(3)}},
        "Oscillator slots; absent slots do not exist.");
    auto filterSlot = [&](int f) {
        return slotSchema(filter(f),
                          {{"type", enumSchema(kFilterTypes, "Filter type (required).")}});
    };
    props["filters"] =
        slots({{"routing", enumSchema(kRoutings, "serial: filter1 → filter2; parallel: summed.")},
               {"balance", fieldSchema(index(kFilters, FiltersF::balance))},
               {"filter1", filterSlot(0)},
               {"filter2", filterSlot(1)}},
              "Filter section.");
    props["drive"] =
        slotSchema(kDrive, {{"type", enumSchema(kDriveTypes, "Drive curve (required).")}});
    props["amp"] = slotSchema(kAmp, {});
    props["envelopes"] = slots({{"amp", slotSchema(kEnvAmp, {})},
                                {"env2", slotSchema(kEnv2, {})},
                                {"env3", slotSchema(kEnv3, {})},
                                {"env4", slotSchema(kEnv4, {})}},
                               "Envelopes; amp always exists and drives the amp.");
    auto lfoSlot = [&](int l) {
        return slotSchema(
            lfo(l),
            {{"retrigger", enumSchema(kRetriggers, "voice: per voice; free/mono: global.")}});
    };
    props["lfos"] = slots(
        {{"lfo1", lfoSlot(0)}, {"lfo2", lfoSlot(1)}, {"lfo3", lfoSlot(2)}, {"lfo4", lfoSlot(3)}},
        "LFO slots.");
    Value values = Value::object();
    values["type"] = "array";
    values["maxItems"] = kMaxSteps;
    values["description"] = "Step values in % -100..100.";
    auto stepSlot = [&](int k) {
        return slotSchema(
            step(k),
            {{"retrigger", enumSchema(kRetriggers, "voice: per voice; free/mono: global.")},
             {"values", values}});
    };
    props["steps"] = slots({{"step1", stepSlot(0)}, {"step2", stepSlot(1)}}, "Step modulators.");
    Value routes = Value::object();
    routes["type"] = "object";
    routes["description"] =
        "Modulation routes keyed \"<source>><destination>\"; value {amount, via?, curve?}. "
        "Amounts: oct for frequencies, st for pitch, dB for levels, x for oscN.fm, % otherwise.";
    routes["maxProperties"] = static_cast<int>(kMaxRoutes);
    props["routes"] = std::move(routes);
    Value nameSchema = Value::object();
    nameSchema["type"] = "string";
    nameSchema["maxLength"] = 24;
    Value macroMembers = Value::object();
    for (int m = 0; m < kMacroCount; ++m)
        macroMembers[slotInfo(macro(m)).name] = slotSchema(macro(m), {{"name", nameSchema}});
    Value macros = Value::object();
    macros["type"] = "object";
    macros["properties"] = std::move(macroMembers);
    macros["additionalProperties"] = false;
    props["macros"] = std::move(macros);
    Value chain = Value::object();
    chain["type"] = "array";
    chain["items"] = enumSchema(kFxNames, "FX name.");
    chain["maxItems"] = kFxCount;
    chain["uniqueItems"] = true;
    props["fx"] = slots({{"chain", chain},
                         {"chorus", slotSchema(kChorus, {})},
                         {"phaser", slotSchema(kPhaser, {})},
                         {"delay", slotSchema(kDelay, {})},
                         {"reverb", slotSchema(kReverb, {})},
                         {"eq", slotSchema(kEq, {})}},
                        "Global post-FX in chain order; blocks only for FX in the chain.");
    props["output"] = slotSchema(kOutput, {});
    s["properties"] = std::move(props);
    Value required = Value::array();
    required.push_back("schema");
    s["required"] = std::move(required);
    s["additionalProperties"] = false;
    return s;
}

// --- describe ------------------------------------------------------------------------------

namespace {

std::string fmt(std::size_t i, double v) { return params::formatCompact(spec(i), v); }
std::string fmtFull(std::size_t i, double v) { return params::format(spec(i), v); }

std::string choiceKey(const Body& b, Slot s, std::uint16_t f) {
    const std::size_t i = index(s, f);
    return std::string(spec(i).choices[static_cast<std::size_t>(b.values[i])].key);
}

bool changed(const Body& b, std::size_t i) { return b.values[i] != defaultValue(b.structure, i); }

std::string signedCents(double ct) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%+g ct", ct);
    return buf;
}

std::string describeOsc(const Body& b, int o, bool changedOnly) {
    const Structure& st = b.structure;
    const Slot s = osc(o);
    const OscType t = *st.osc[o];
    std::string d = std::string(slotInfo(s).name) + " ";
    auto v = [&](OscF f) { return b.values[index(s, f)]; };
    switch (t) {
        case OscType::Va:
            d += "va " + choiceKey(b, s, static_cast<std::uint16_t>(OscF::wave));
            break;
        case OscType::Wavetable:
            d += "wavetable " + choiceKey(b, s, static_cast<std::uint16_t>(OscF::table)) + " @" +
                 fmt(index(s, OscF::position), v(OscF::position));
            break;
        case OscType::Fm: {
            d += "fm";
            const auto mode = static_cast<RatioMode>(v(OscF::ratio_mode));
            if (mode == RatioMode::Fixed)
                d += " " + fmtFull(index(s, OscF::fixed_freq), v(OscF::fixed_freq));
            else
                d += " ×" + params::formatCompact(spec(index(s, OscF::ratio)), v(OscF::ratio));
            if (v(OscF::feedback) > 0.0)
                d += " fb " + fmt(index(s, OscF::feedback), v(OscF::feedback));
            break;
        }
        case OscType::Noise:
            d += choiceKey(b, s, static_cast<std::uint16_t>(OscF::color)) + " noise";
            break;
        case OscType::Sub:
            d += "sub " + choiceKey(b, s, static_cast<std::uint16_t>(OscF::wave)) + " " +
                 std::to_string(static_cast<int>(v(OscF::octave))) + " oct";
            break;
    }
    if (oscFieldApplies(t, OscF::coarse) && v(OscF::coarse) != 0.0)
        d += " " + fmtFull(index(s, OscF::coarse), v(OscF::coarse));
    if (oscFieldApplies(t, OscF::fine) && v(OscF::fine) != 0.0)
        d += " " + signedCents(v(OscF::fine));
    if (!changedOnly || changed(b, index(s, OscF::level)))
        d += " " + fmtFull(index(s, OscF::level), v(OscF::level));
    if (st.to[o] == OscTarget::None) d += " (modulator)";
    if (st.to[o] == OscTarget::Direct) d += " (direct)";
    return d;
}

std::string describeFilter(const Body& b, int f) {
    const Slot s = filter(f);
    const FilterType t = *b.structure.filter[f];
    auto v = [&](FilterF x) { return b.values[index(s, x)]; };
    auto q = [&](FilterF x) { return fmtFull(index(s, x), v(x)); };
    std::string d;
    switch (t) {
        case FilterType::Ladder:
            d = "ladder " + std::string(v(FilterF::slope) > 0.5 ? "24" : "12") + " dB LP " +
                q(FilterF::cutoff);
            break;
        case FilterType::Svf:
            d = "svf " + choiceKey(b, s, static_cast<std::uint16_t>(FilterF::mode)) + " " +
                q(FilterF::cutoff);
            break;
        case FilterType::Comb:
            d = "comb " + q(FilterF::cutoff) + " fb " + q(FilterF::feedback);
            break;
        case FilterType::Formant:
            d = "formant vowel " + fmt(index(s, FilterF::vowel), v(FilterF::vowel));
            break;
    }
    if (filterFieldApplies(t, FilterF::resonance) && v(FilterF::resonance) > 0.0)
        d += " res " + q(FilterF::resonance);
    if (v(FilterF::keytrack) > 0.0) d += ", keytrack " + q(FilterF::keytrack);
    if (v(FilterF::drive) > 0.0) d += ", drive " + q(FilterF::drive);
    return d;
}

std::string describeEnv(const Body& b, int e) {
    const Slot s = env(e);
    auto q = [&](EnvF f) { return fmtFull(index(s, f), b.values[index(s, f)]); };
    std::string d = "A " + q(EnvF::attack) + " D " + q(EnvF::decay) + " S " + q(EnvF::sustain) +
                    " R " + q(EnvF::release);
    if (b.values[index(s, EnvF::delay)] > 0.0) d = "delay " + q(EnvF::delay) + " " + d;
    return d;
}

}  // namespace

std::string describe(const Patch& p, bool changedOnly) {
    const Body& b = p.body;
    const Structure& st = b.structure;
    std::string out;
    auto add = [&](const std::string& s) {
        if (s.empty()) return;
        if (!out.empty()) out += ' ';
        out += s;
    };
    const auto mode = static_cast<VoiceMode>(b.values[index(kVoice, VoiceF::mode)]);
    const int unison = static_cast<int>(b.values[index(kVoice, VoiceF::unison)]);
    std::string voice = mode == VoiceMode::Poly   ? "Poly"
                        : mode == VoiceMode::Mono ? "Mono"
                                                  : "Legato";
    if (unison > 1)
        voice += ", " + std::to_string(unison) + "-voice unison (" +
                 fmtFull(index(kVoice, VoiceF::unison_detune),
                         b.values[index(kVoice, VoiceF::unison_detune)]) +
                 ")";
    const double glide = b.values[index(kVoice, VoiceF::glide)];
    if (glide > 0.0) voice += ", glide " + fmtFull(index(kVoice, VoiceF::glide), glide);
    add(voice + ".");

    std::string oscs;
    for (int o = 0; o < kOscCount; ++o) {
        if (!st.osc[o]) continue;
        if (!oscs.empty()) oscs += ", ";
        oscs += describeOsc(b, o, changedOnly);
    }
    std::string filters;
    for (int f = 0; f < kFilterCount; ++f) {
        if (!st.filter[f]) continue;
        if (!filters.empty()) filters += st.routing == FilterRouting::Serial ? " → " : " ∥ ";
        filters += describeFilter(b, f);
    }
    std::string chain = oscs.empty() ? std::string("No oscillators") : oscs;
    if (!filters.empty()) chain += " → " + filters;
    if (st.drive != DriveType::Off)
        chain += " → " + std::string(kDriveTypes[static_cast<int>(st.drive)]) + " drive " +
                 fmtFull(index(kDrive, DriveF::amount), b.values[index(kDrive, DriveF::amount)]);
    add(chain + ".");

    for (int e = 1; e < kEnvCount; ++e)
        if (st.env[e]) add(std::string(slotInfo(env(e)).name) + " (" + describeEnv(b, e) + ").");
    for (int l = 0; l < kLfoCount; ++l) {
        if (!st.lfo[l]) continue;
        const Slot s = lfo(l);
        const auto sync = static_cast<NoteValue>(b.values[index(s, LfoF::sync)]);
        std::string rate =
            sync == NoteValue::Off
                ? fmtFull(index(s, LfoF::rate), b.values[index(s, LfoF::rate)])
                : std::string(
                      spec(index(s, LfoF::sync)).choices[static_cast<std::size_t>(sync)].label);
        add(std::string(slotInfo(s).name) + " " +
            choiceKey(b, s, static_cast<std::uint16_t>(LfoF::wave)) + " " + rate +
            (st.lfoRetrigger[l] != Retrigger::Voice ? " (global)." : "."));
    }
    std::string routes;
    for (const Route& r : st.routes) {
        if (!routes.empty()) routes += "; ";
        // Sub-semitone pitch amounts read better in cents ("+8 ct").
        std::string amount = modUnit(r.dest) == Unit::st && std::fabs(r.amount) < 1.0
                                 ? signedCents(r.amount * 100.0)
                                 : units::formatQuantity(r.amount, modUnit(r.dest));
        if (r.amount > 0 && amount.front() != '+') amount = "+" + amount;
        std::string src(sourceName(r.source));
        if (r.source >= Source::Macro1 && r.source <= Source::Macro8) {
            const int m = static_cast<int>(r.source) - static_cast<int>(Source::Macro1);
            if (!st.macroName[m].empty()) src += " \"" + st.macroName[m] + "\"";
        }
        routes += src + " → " + destName(r.dest) + " " + amount;
        if (r.via) routes += " via " + std::string(sourceName(*r.via));
    }
    if (!routes.empty()) add(routes + ".");
    add("Amp " + describeEnv(b, 0) + ".");
    if (!st.chain.empty()) {
        std::string fx = "FX:";
        for (std::size_t k = 0; k < st.chain.size(); ++k) {
            const Slot s = static_cast<Slot>(kChorus + static_cast<int>(st.chain[k]));
            fx += (k ? " → " : " ") + std::string(fxName(st.chain[k]));
            if (st.chain[k] == FxKind::Reverb)
                fx += " " + fmtFull(index(s, ReverbF::decay), b.values[index(s, ReverbF::decay)]);
            if (st.chain[k] != FxKind::Eq) {
                const std::uint16_t mixField =
                    static_cast<std::uint16_t>(fieldCount(slotInfo(s).block) - 1);
                fx += " " + fmtFull(index(s, mixField), b.values[index(s, mixField)]);
            }
        }
        add(fx + ".");
    }
    constexpr std::size_t kMax = 600;
    if (out.size() > kMax) {
        std::size_t cut = kMax - 3;
        while (cut > 0 && (static_cast<unsigned char>(out[cut]) & 0xC0) == 0x80) --cut;
        out = out.substr(0, cut) + "…";
    }
    return out;
}

}  // namespace supple::orrery
