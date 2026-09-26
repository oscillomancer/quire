#include "orrery/edits.h"

#include <algorithm>

namespace supple::orrery::edits {

namespace {

using json::Value;

constexpr std::string_view kDriveNames[] = {"off", "soft", "tape", "fold", "hard"};
constexpr std::string_view kQualityNames[] = {"eco", "normal", "high"};
constexpr std::string_view kTargetNames[] = {"filter1", "filter2", "both", "direct", "none"};

Value object() { return Value::object(); }

// {"<block>": {"<slot>": v}}
Value nested(std::string_view block, std::string_view slot, Value v) {
    Value inner = object();
    inner[slot] = std::move(v);
    Value out = object();
    out[block] = std::move(inner);
    return out;
}

std::string_view blockOf(Slot s) {
    switch (slotInfo(s).block) {
        case Block::Osc:
            return "oscillators";
        case Block::Filter:
        case Block::Filters:
            return "filters";
        case Block::Env:
            return "envelopes";
        case Block::Lfo:
            return "lfos";
        case Block::Step:
            return "steps";
        case Block::Macro:
            return "macros";
        default:
            return slotInfo(s).name;  // voice, drive, amp, output, fx blocks
    }
}

// Merges `extra` into `into` (both merge patches; objects combine).
void combine(Value& into, const Value& extra) {
    for (const auto& m : extra.asObject()) {
        Value* at = into.asObject().find(m.key);
        if (at && at->isObject() && m.value.isObject())
            combine(*at, m.value);
        else
            into[m.key] = m.value;
    }
}

bool touches(const Route& r, Slot s) {
    if (auto src = sourceSlot(r.source); src && *src == s) return true;
    if (r.via)
        if (auto v = sourceSlot(*r.via); v && *v == s) return true;
    return destSlot(r.dest) == s;
}

Value chainValue(const std::vector<FxKind>& chain) {
    Value a = Value::array();
    for (FxKind k : chain) a.push_back(Value(std::string(fxName(k))));
    Value fx = object();
    fx["chain"] = std::move(a);
    Value out = object();
    out["fx"] = std::move(fx);
    return out;
}

}  // namespace

std::optional<Slot> sourceSlot(Source s) {
    const auto i = static_cast<int>(s);
    if (s <= Source::Env4) return env(i - static_cast<int>(Source::Amp));
    if (s <= Source::Lfo4) return lfo(i - static_cast<int>(Source::Lfo1));
    if (s <= Source::Step2) return step(i - static_cast<int>(Source::Step1));
    if (s >= Source::Macro1 && s <= Source::Macro8)
        return macro(i - static_cast<int>(Source::Macro1));
    if (isAudioSource(s)) return osc(i - static_cast<int>(Source::Osc1));
    return std::nullopt;
}

Slot destSlot(const Dest& d) {
    return d.kind == Dest::Kind::Param ? locate(d.index).slot : osc(d.index);
}

std::string routeKey(const Route& r) {
    return std::string(sourceName(r.source)) + ">" + destName(r.dest);
}

bool removable(Slot s) {
    switch (slotInfo(s).block) {
        case Block::Osc:
        case Block::Filter:
        case Block::Drive:
        case Block::Lfo:
        case Block::Step:
        case Block::Chorus:
        case Block::Phaser:
        case Block::Delay:
        case Block::Reverb:
        case Block::Eq:
            return true;
        case Block::Env:
            return s != kEnvAmp;
        default:
            return false;
    }
}

json::Value addSlot(const Structure& st, Slot s) {
    const std::string_view name = slotInfo(s).name;
    Value v = object();
    switch (slotInfo(s).block) {
        case Block::Osc:
            v["type"] = "va";
            v["to"] = st.filter[0] ? "filter1" : st.filter[1] ? "filter2" : "direct";
            return nested("oscillators", name, std::move(v));
        case Block::Filter:
            v["type"] = "svf";
            return nested("filters", name, std::move(v));
        case Block::Drive: {
            Value out = object();
            v["type"] = "soft";
            out["drive"] = std::move(v);
            return out;
        }
        case Block::Env:
        case Block::Lfo:
        case Block::Step:
            return nested(blockOf(s), name, std::move(v));
        default:
            return object();
    }
}

json::Value removeSlot(const Structure& st, Slot s) {
    Value out = object();
    const std::string_view name = slotInfo(s).name;
    switch (slotInfo(s).block) {
        case Block::Osc:
        case Block::Env:
        case Block::Lfo:
        case Block::Step:
            out = nested(blockOf(s), name, Value());
            break;
        case Block::Filter: {
            out = nested("filters", name, Value());
            // Oscillators feeding the removed filter move to the other one (or direct).
            const int gone = s == kFilter1 ? 0 : 1;
            const bool other = st.filter[static_cast<std::size_t>(1 - gone)].has_value();
            const OscTarget into =
                other ? (gone == 0 ? OscTarget::Filter2 : OscTarget::Filter1) : OscTarget::Direct;
            for (int i = 0; i < kOscCount; ++i) {
                const auto k = static_cast<std::size_t>(i);
                if (!st.osc[k]) continue;
                const OscTarget t = st.to[k];
                const bool fed = t == OscTarget::Both ||
                                 t == (gone == 0 ? OscTarget::Filter1 : OscTarget::Filter2);
                if (fed) combine(out, setOscTarget(i, into));
            }
            if (!other) combine(out, nested("filters", "routing", Value("serial")));
            break;
        }
        case Block::Drive:
            out["drive"] = Value();
            break;
        default:
            for (std::size_t i = 0; i < st.chain.size(); ++i)
                if (slotInfo(s).name == "fx." + std::string(fxName(st.chain[i])))
                    return removeFx(st, i);
            return out;
    }
    Value routes = object();
    for (const Route& r : st.routes)
        if (touches(r, s)) routes[routeKey(r)] = Value();
    if (!routes.asObject().empty()) out["routes"] = std::move(routes);
    return out;
}

json::Value setType(const Structure& st, Slot s, int type) {
    Value v = object();
    // Routes to parameters the new type does not have go with the old type.
    auto dropRoutes = [&](Value& out, const Structure& next) {
        Value routes = object();
        for (const Route& r : st.routes)
            if (r.dest.kind == Dest::Kind::Param && !paramActive(next, r.dest.index))
                routes[routeKey(r)] = Value();
        if (!routes.asObject().empty()) out["routes"] = std::move(routes);
    };
    Structure next = st;
    switch (slotInfo(s).block) {
        case Block::Osc: {
            v["type"] = std::string(oscTypeName(static_cast<OscType>(type)));
            Value out = nested("oscillators", slotInfo(s).name, std::move(v));
            next.osc[static_cast<std::size_t>(s - kOsc1)] = static_cast<OscType>(type);
            dropRoutes(out, next);
            return out;
        }
        case Block::Filter: {
            v["type"] = std::string(filterTypeName(static_cast<FilterType>(type)));
            Value out = nested("filters", slotInfo(s).name, std::move(v));
            next.filter[static_cast<std::size_t>(s - kFilter1)] = static_cast<FilterType>(type);
            dropRoutes(out, next);
            return out;
        }
        case Block::Drive: {
            Value out = object();
            if (type <= 0) {
                out["drive"] = Value();  // off: the block goes
                next.drive = DriveType::Off;
                dropRoutes(out, next);
            } else {
                v["type"] = std::string(kDriveNames[static_cast<std::size_t>(type)]);
                out["drive"] = std::move(v);
            }
            return out;
        }
        default:
            return object();
    }
}

json::Value setOscTarget(int oscIndex, OscTarget t) {
    Value v = object();
    v["to"] = std::string(kTargetNames[static_cast<std::size_t>(t)]);
    return nested("oscillators", slotInfo(osc(oscIndex)).name, std::move(v));
}

json::Value setRouting(FilterRouting r) {
    return nested("filters", "routing", Value(r == FilterRouting::Serial ? "serial" : "parallel"));
}

json::Value setQuality(Quality q) {
    return nested("voice", "quality",
                  Value(std::string(kQualityNames[static_cast<std::size_t>(q)])));
}

json::Value addFx(const Structure& st, FxKind k) {
    std::vector<FxKind> chain = st.chain;
    if (std::find(chain.begin(), chain.end(), k) == chain.end()) chain.push_back(k);
    return chainValue(chain);
}

json::Value removeFx(const Structure& st, std::size_t position) {
    std::vector<FxKind> chain = st.chain;
    if (position < chain.size()) chain.erase(chain.begin() + static_cast<std::ptrdiff_t>(position));
    return chainValue(chain);
}

json::Value moveFx(const Structure& st, std::size_t from, std::size_t to) {
    std::vector<FxKind> chain = st.chain;
    if (from < chain.size() && to < chain.size()) {
        const FxKind k = chain[from];
        chain.erase(chain.begin() + static_cast<std::ptrdiff_t>(from));
        chain.insert(chain.begin() + static_cast<std::ptrdiff_t>(to), k);
    }
    return chainValue(chain);
}

json::Value setRoute(const Route& r) {
    Value v = object();
    v["amount"] = r.amount;
    v["via"] = r.via ? Value(std::string(sourceName(*r.via))) : Value();
    v["curve"] = r.curve != 0.0 ? Value(r.curve) : Value();
    Value routes = object();
    routes[routeKey(r)] = std::move(v);
    Value out = object();
    out["routes"] = std::move(routes);
    return out;
}

json::Value removeRoute(const Route& r) {
    Value routes = object();
    routes[routeKey(r)] = Value();
    Value out = object();
    out["routes"] = std::move(routes);
    return out;
}

std::vector<Source> availableSources(const Structure& st) {
    std::vector<Source> out;
    for (int i = 0; i < static_cast<int>(Source::Count); ++i) {
        const auto s = static_cast<Source>(i);
        if (auto slot = sourceSlot(s); slot && !st.has(*slot)) continue;
        out.push_back(s);
    }
    return out;
}

std::vector<Dest> availableDests(const Structure& st, Source source) {
    std::vector<Dest> out;
    const auto t = table();
    auto allowed = [&](const Dest& d) {
        return !isGlobalDest(d, st) || isGlobalSource(source, st);
    };
    if (isAudioSource(source)) {
        const int self = static_cast<int>(source) - static_cast<int>(Source::Osc1);
        for (int j = 0; j < kOscCount; ++j) {
            if (j == self || !st.osc[static_cast<std::size_t>(j)]) continue;
            out.push_back({Dest::Kind::Fm, static_cast<std::uint16_t>(j)});
            out.push_back({Dest::Kind::Am, static_cast<std::uint16_t>(j)});
        }
        return out;
    }
    for (int j = 0; j < kOscCount; ++j)
        if (st.osc[static_cast<std::size_t>(j)]) {
            const Dest d{Dest::Kind::Pitch, static_cast<std::uint16_t>(j)};
            if (allowed(d)) out.push_back(d);
        }
    for (std::size_t i = 0; i < t.size(); ++i) {
        if (!(t[i].flags & params::kModulatable) || !paramActive(st, i)) continue;
        const Dest d{Dest::Kind::Param, static_cast<std::uint16_t>(i)};
        if (allowed(d)) out.push_back(d);
    }
    return out;
}

double defaultAmount(const Dest& d) {
    switch (modUnit(d)) {
        case units::Unit::oct:
            return 1.0;
        case units::Unit::st:
            return 1.0;
        case units::Unit::dB:
            return -6.0;
        case units::Unit::x:
            return 1.0;
        default:
            return 25.0;
    }
}

}  // namespace supple::orrery::edits
