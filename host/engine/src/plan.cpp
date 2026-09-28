#include "plan.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <set>

#include "supple/clap/semantic_params.h"

namespace supple::engine::detail {

namespace {

std::int64_t latTap(const Node& n, model::Tap tap) {
    return tap == model::Tap::PreFx ? n.latIn : n.latIn + n.latChain;
}

// Where an item sits, after resolving its timebase (item, then track, then
// project): its start in beats and seconds and its end in seconds.
struct ItemSpan {
    double startBeat, startSec, endSec;
};
ItemSpan itemSpan(const Session& s, const model::Item& it, const model::Track* tr) {
    const time::TimeMap& tm = *s.timeMap;
    model::Timebase tb = s.project->timebase;
    if (it.timebase == model::ItemTimebase::Time)
        tb = model::Timebase::Time;
    else if (it.timebase == model::ItemTimebase::Beats)
        tb = model::Timebase::Beats;
    else if (it.timebase == model::ItemTimebase::BeatsPosition)
        tb = model::Timebase::BeatsPosition;
    else if (tr && tr->timebase == model::TrackTimebase::Time)
        tb = model::Timebase::Time;
    else if (tr && tr->timebase == model::TrackTimebase::Beats)
        tb = model::Timebase::Beats;
    else if (tr && tr->timebase == model::TrackTimebase::BeatsPosition)
        tb = model::Timebase::BeatsPosition;
    if (tb == model::Timebase::Time)
        return {tm.beatAt(it.position), it.position, it.position + it.length};
    const double startSec = tm.secondsAt(it.position);
    if (tb == model::Timebase::Beats)
        return {it.position, startSec, tm.secondsAt(it.position + it.length)};
    return {it.position, startSec, startSec + it.length};  // beats position, seconds length
}

double fadeCurve(model::FadeShape shape, double t) {
    t = std::clamp(t, 0.0, 1.0);
    switch (shape) {
        case model::FadeShape::Linear:
            return t;
        case model::FadeShape::Slow:
            return t * t;
        case model::FadeShape::Fast:
            return 1.0 - (1.0 - t) * (1.0 - t);
        case model::FadeShape::SlowFast:
            return t * t * (3.0 - 2.0 * t);
        case model::FadeShape::FastSlow:
            return 0.5 - std::sin(std::asin(1.0 - 2.0 * t) / 3.0);
        case model::FadeShape::Cos:
            return std::sin(t * std::numbers::pi / 2.0);  // equal power
        case model::FadeShape::Exp:
            return t * t * t;
    }
    return t;
}

// Note and control events of the MIDI items on `track`, in samples from the
// render start (before PDC shifting).
std::vector<TimedEvent> trackEvents(const Session& s, Id track, double startSeconds, double rate) {
    std::vector<std::pair<int, TimedEvent>> items;  // rank (0 off, 1 ctl, 2 on), event
    const time::TimeMap& tm = *s.timeMap;
    const std::int64_t origin = time::samplesAt(startSeconds, rate);
    const model::Track* tr = s.tracks.find(track);
    std::int32_t noteId = 0;
    for (const auto& e : s.items) {
        const model::Item& it = *e.value;
        if (it.track != track || it.mute || !it.activeTake) continue;
        const model::Take* take = s.takes.find(*it.activeTake);
        if (!take || !take->source || take->source->kind != Kind::Midi) continue;
        const model::MidiSource* src = s.midi.find(*take->source);
        if (!src) continue;
        const midi::Sequence& seq = src->sequence;
        const ItemSpan span = itemSpan(s, it, tr);
        const double startBeat = span.startBeat, endSec = span.endSec;
        const double rateScale = take->playrate > 0 ? take->playrate : 1.0;
        const double loopBeats = seq.ppq > 0 ? static_cast<double>(seq.length) / seq.ppq : 0.0;
        auto toSample = [&](double contentBeat, double loopOffset) -> std::optional<std::int64_t> {
            const double itemBeat = (contentBeat - take->startOffset) / rateScale + loopOffset;
            if (itemBeat < 0) return std::nullopt;
            const double sec = tm.secondsAt(startBeat + itemBeat);
            return time::samplesAt(std::min(sec, endSec), rate) - origin;
        };
        const std::int64_t endSample = time::samplesAt(endSec, rate) - origin;
        const int reps = it.loop && loopBeats > 0
                             ? 1 + static_cast<int>(std::floor((tm.beatAt(endSec) - startBeat) *
                                                               rateScale / loopBeats))
                             : 1;
        for (int k = 0; k < reps; ++k) {
            const double offset = k * loopBeats / rateScale;
            for (const midi::Note& n : seq.notes) {
                if (k > 0 && n.start >= seq.length) continue;
                const double b0 = static_cast<double>(n.start) / seq.ppq;
                auto on = toSample(b0, offset);
                // A take can start in the middle of a note (for example after
                // item.split). Retrigger the held note at the item's boundary.
                if (!on) {
                    if (static_cast<double>(n.start + n.length) / seq.ppq <= take->startOffset)
                        continue;
                    on = time::samplesAt(tm.secondsAt(startBeat + offset), rate) - origin;
                }
                if (*on >= endSample) continue;
                auto off = toSample(static_cast<double>(n.start + n.length) / seq.ppq, offset);
                const std::int64_t offS = std::max(*on + 1, off ? *off : *on + 1);
                TimedEvent a;
                a.sample = *on;
                a.event.kind = host::Event::Kind::NoteOn;
                a.event.channel = n.channel;
                a.event.key = n.pitch;
                a.event.noteId = noteId;
                a.event.velocity = n.velocity / 127.0;
                items.push_back({2, a});
                TimedEvent b = a;
                b.sample = offS;
                b.event.kind = host::Event::Kind::NoteOff;
                b.event.velocity = n.offVelocity / 127.0;
                items.push_back({0, b});
                ++noteId;
            }
            for (const midi::Control& c : seq.controls) {
                auto at = toSample(static_cast<double>(c.tick) / seq.ppq, offset);
                if (!at || *at >= endSample) continue;
                TimedEvent ev;
                ev.sample = *at;
                ev.event.kind = host::Event::Kind::Midi;
                const auto ch = static_cast<std::uint8_t>(c.channel & 0x0f);
                auto b7 = [](std::int32_t v) {
                    return static_cast<std::uint8_t>(std::clamp(v, 0, 127));
                };
                switch (c.kind) {
                    case midi::ControlKind::CC:
                        ev.event.midi[0] = static_cast<std::uint8_t>(0xb0 | ch);
                        ev.event.midi[1] = b7(c.number);
                        ev.event.midi[2] = b7(c.value);
                        break;
                    case midi::ControlKind::ProgramChange:
                        ev.event.midi[0] = static_cast<std::uint8_t>(0xc0 | ch);
                        ev.event.midi[1] = b7(c.value);
                        break;
                    case midi::ControlKind::ChannelPressure:
                        ev.event.midi[0] = static_cast<std::uint8_t>(0xd0 | ch);
                        ev.event.midi[1] = b7(c.value);
                        break;
                    case midi::ControlKind::PolyPressure:
                        ev.event.midi[0] = static_cast<std::uint8_t>(0xa0 | ch);
                        ev.event.midi[1] = b7(c.number);
                        ev.event.midi[2] = b7(c.value);
                        break;
                    case midi::ControlKind::PitchBend: {
                        const std::int32_t v = std::clamp(c.value, -8192, 8191) + 8192;
                        ev.event.midi[0] = static_cast<std::uint8_t>(0xe0 | ch);
                        ev.event.midi[1] = static_cast<std::uint8_t>(v & 0x7f);
                        ev.event.midi[2] = static_cast<std::uint8_t>((v >> 7) & 0x7f);
                        break;
                    }
                }
                items.push_back({1, ev});
            }
        }
    }
    std::stable_sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        if (a.second.sample != b.second.sample) return a.second.sample < b.second.sample;
        return a.first < b.first;
    });
    std::vector<TimedEvent> out;
    out.reserve(items.size());
    for (auto& [rank, ev] : items)
        if (ev.sample >= 0) out.push_back(ev);  // no chasing of notes before the start (yet)
    return out;
}

// Topological order: every track after the tracks feeding it; ties by ID.
Result<std::vector<Id>> topoOrder(const Session& s) {
    std::map<Id, std::set<Id>> feeds;  // source -> targets
    std::map<Id, int> indeg;
    for (const auto& e : s.tracks) indeg[e.id];
    auto link = [&](Id a, Id b) {
        if (feeds[a].insert(b).second) ++indeg[b];
    };
    for (const auto& e : s.tracks)
        if (e.value->parent && e.value->parentSend.enabled) link(e.id, *e.value->parent);
    for (const auto& e : s.routes)
        if (e.value->from.track && e.value->to.track && !e.value->mute)
            link(*e.value->from.track, *e.value->to.track);
    std::set<Id> ready;
    for (const auto& [id, d] : indeg)
        if (d == 0) ready.insert(id);
    std::vector<Id> order;
    while (!ready.empty()) {
        const Id id = *ready.begin();
        ready.erase(ready.begin());
        order.push_back(id);
        for (Id t : feeds[id])
            if (--indeg[t] == 0) ready.insert(t);
    }
    if (order.size() != s.tracks.size()) return Error("engine.cycle", "routing cycle");
    return order;
}

}  // namespace

// Resolves an envelope's plugin parameter: a semantic key (physical values)
// or, for any plugin, a CLAP id ("12") or parameter name (CLAP values).
struct ResolvedLane {
    ParamLane lane;
    double lo = 0.0, hi = 1.0;
    double toLaneDomain(double v) const {
        if (lane.spec)
            return params::toNormalized(lane.spec->spec, params::clampPhysical(lane.spec->spec, v));
        return std::clamp(v, lo, hi);
    }
};

Result<ResolvedLane> paramLane(host::Instance& in, const std::string& param,
                               std::map<host::Instance*, json::Value>& schemas) {
    ResolvedLane r;
    if (in.info().semantic) {
        auto it = schemas.find(&in);
        if (it == schemas.end()) {
            auto schema = in.semanticSchema();
            it = schemas.emplace(&in, schema ? std::move(*schema) : json::Value()).first;
        }
        const json::Value* entry = nullptr;
        if (const json::Value* ps = it->second.isObject() ? it->second.find("params") : nullptr)
            entry = ps->find(param);
        if (entry) {
            auto d = params::fromDescription(param, *entry);
            if (!d) return std::move(d).error();
            if ((*d)->spec.flags & params::kReadonly)
                return Error("engine.automation", param + " is read-only");
            r.lane.clapId = (*d)->spec.id;
            r.lane.spec = std::shared_ptr<const params::DescribedParam>(std::move(*d));
            return r;
        }
    }
    const auto list = in.params();
    const bool numeric = !param.empty() && std::all_of(param.begin(), param.end(),
                                                       [](char c) { return c >= '0' && c <= '9'; });
    for (const host::ParamInfo& pi : list) {
        const bool match = numeric ? std::to_string(pi.id) == param : pi.name == param;
        if (!match) continue;
        r.lane.clapId = pi.id;
        r.lo = pi.min;
        r.hi = pi.max;
        return r;
    }
    return Error("engine.automation", in.info().name + " has no parameter \"" + param + "\"");
}

Result<Plan> compile(const Session& s, InstancePool& pool, double startSeconds, std::uint32_t block,
                     double rate) {
    Plan p;
    p.rate = rate > 0 ? rate : s.project->sampleRate;
    p.smoothFrames = static_cast<std::uint32_t>(std::max(1.0, std::round(0.010 * p.rate)));
    p.block = block;
    p.startSeconds = startSeconds;
    p.timeMap = s.timeMap;
    auto order = topoOrder(s);
    if (!order) return std::move(order).error();
    std::map<Id, std::size_t> index;
    for (std::size_t i = 0; i < order->size(); ++i) index[(*order)[i]] = i;
    // The master must be last so its output is final (it has no outputs).
    if (order->back() != model::kMaster)
        return Error("engine.topology", "the master is not the last track");
    p.nodes.resize(order->size());
    for (std::size_t i = 0; i < order->size(); ++i) {
        Node& n = p.nodes[i];
        n.id = (*order)[i];
        const model::Track& t = *s.tracks.find(n.id);
        n.volume = dbToGain(t.volumeDb);
        n.volumeDb = t.volumeDb;
        n.pan = t.pan;
        n.panLawDb = s.project->panLawDb;
        n.master = n.id == model::kMaster;
        n.width = t.width / 100.0;
        if (n.id == model::kMaster) {
            // The master pans as a balance control: no attenuation at the centre.
            const double b = std::clamp(t.pan / 100.0, -1.0, 1.0);
            n.panL = b <= 0 ? 1.0 : 1.0 - b;
            n.panR = b >= 0 ? 1.0 : 1.0 + b;
        } else {
            std::tie(n.panL, n.panR) = panGains(t.pan / 100.0, s.project->panLawDb);
        }
        n.mute = t.mute;
        for (Id fxId : t.chain) {
            const model::FxInstance& fx = *s.fx.find(fxId);
            Stage st;
            st.fx = fxId;
            st.bypass = fx.bypass || fx.offline;
            st.wet = fx.wet / 100.0;
            if (!fx.offline) {
                auto inst = pool.get(fxId, fx, s);
                if (!inst) {
                    p.warnings.push_back(fxId.str() + " (" + fx.plugin.id +
                                         "): " + inst.error().message + "; passing audio through");
                } else {
                    st.instance = *inst;
                }
            }
            n.chain.push_back(std::move(st));
        }
    }
    for (const Node& n : p.nodes)
        for (const Stage& st : n.chain)
            if (st.instance) p.instances.push_back(st.instance);
    std::sort(p.instances.begin(), p.instances.end());
    p.instances.erase(std::unique(p.instances.begin(), p.instances.end()), p.instances.end());
    // Edges: children's parent sends, then routes; each target sums its
    // inputs in (source order, edge id) order.
    struct Pending {
        std::size_t from, to;
        Edge e;
        std::uint64_t key;
    };
    std::vector<Pending> pending;
    for (const auto& e : s.tracks) {
        const model::Track& t = *e.value;
        if (!t.parent || !t.parentSend.enabled) continue;
        Edge ed;
        ed.from = index.at(e.id);
        ed.tap = model::Tap::PostFader;
        pending.push_back({index.at(e.id), index.at(*t.parent), std::move(ed), 0});
    }
    for (const auto& e : s.routes) {
        const model::Route& r = *e.value;
        if (!r.from.track || !r.to.track || r.mute) continue;
        Edge ed;
        ed.from = index.at(*r.from.track);
        ed.route = e.id;
        ed.tap = r.tap;
        ed.volumeDb = r.volumeDb;
        ed.pan = r.pan;
        const double g = dbToGain(r.volumeDb);
        auto [pl, pr] = panGains(r.pan / 100.0, s.project->panLawDb);
        ed.gainL = g * pl;
        ed.gainR = g * pr;
        ed.mono = r.mono;
        ed.invert = r.phaseInvert;
        pending.push_back({ed.from, index.at(*r.to.track), std::move(ed), 1 + e.id.n});
    }
    std::sort(pending.begin(), pending.end(), [](const Pending& a, const Pending& b) {
        return a.to != b.to ? a.to < b.to : a.from != b.from ? a.from < b.from : a.key < b.key;
    });
    for (auto& pe : pending) {
        p.nodes[pe.to].in.push_back(p.edges.size());
        p.edges.push_back(std::move(pe.e));
    }
    // Solo: with any track soloed, a track stays audible only if it is soloed,
    // solo-defeated, the master, or on a soloed track's signal path: what
    // feeds it (soloing a bus or folder keeps its sources and children) and
    // where it goes (soloing a source keeps its folders, buses and the master).
    // Siblings that merely share a folder with a soloed track are silenced.
    {
        std::set<Id> soloed;
        for (const auto& e : s.tracks)
            if (e.value->solo == model::Solo::On || e.value->solo == model::Solo::InPlace)
                soloed.insert(e.id);
        if (!soloed.empty()) {
            std::map<Id, std::vector<Id>> down, up;  // signal flow
            for (const auto& e : s.tracks)
                if (e.value->parent && e.value->parentSend.enabled) {
                    down[e.id].push_back(*e.value->parent);
                    up[*e.value->parent].push_back(e.id);
                }
            for (const auto& e : s.routes)
                if (e.value->from.track && e.value->to.track) {
                    down[*e.value->from.track].push_back(*e.value->to.track);
                    up[*e.value->to.track].push_back(*e.value->from.track);
                }
            std::set<Id> audible = soloed;
            auto flood = [&](std::map<Id, std::vector<Id>>& g) {
                std::vector<Id> stack(soloed.begin(), soloed.end());
                std::set<Id> seen;
                while (!stack.empty()) {
                    const Id id = stack.back();
                    stack.pop_back();
                    for (Id nxt : g[id])
                        if (seen.insert(nxt).second) {
                            audible.insert(nxt);
                            stack.push_back(nxt);
                        }
                }
            };
            flood(down);
            flood(up);
            for (Node& n : p.nodes) {
                const model::Track& t = *s.tracks.find(n.id);
                n.soloSilenced = !audible.count(n.id) && t.solo != model::Solo::Defeat && !n.master;
            }
        }
    }
    // Envelopes on mixer targets and plugin parameters.
    const std::int64_t origin = time::samplesAt(startSeconds, p.rate);
    std::map<host::Instance*, json::Value> schemas;  // semantic schemas, fetched once
    for (const auto& e : s.envelopes) {
        const model::Envelope& env = *e.value;
        if (!env.active || env.points.empty()) continue;
        Automation a;
        for (const model::EnvPoint& pt : env.points) {
            const double sec =
                env.timebase == model::Timebase::Time ? pt.pos : s.timeMap->secondsAt(pt.pos);
            a.points.push_back(
                {time::samplesAt(sec, p.rate) - origin, pt.value, pt.shape, pt.tension});
        }
        const Id o = env.target.object;
        const std::string& param = env.target.param;
        Automation* slot = nullptr;
        if (o.kind == Kind::Track && index.count(o)) {
            Node& n = p.nodes[index.at(o)];
            slot = param == "volume"  ? &n.autoVolume
                   : param == "trim"  ? &n.autoTrim
                   : param == "pan"   ? &n.autoPan
                   : param == "width" ? &n.autoWidth
                   : param == "mute"  ? &n.autoMute
                                      : nullptr;
        } else if (o.kind == Kind::Route) {
            for (Edge& ed : p.edges)
                if (ed.route == o)
                    slot = param == "volume" ? &ed.autoVolume
                           : param == "pan"  ? &ed.autoPan
                           : param == "mute" ? &ed.autoMute
                                             : nullptr;
        } else if (o.kind == Kind::Fx) {
            bool placed = false;
            for (Node& n : p.nodes)
                for (Stage& st : n.chain) {
                    if (st.fx != o || !st.instance) continue;
                    auto lane = paramLane(*st.instance, param, schemas);
                    if (!lane) {
                        p.warnings.push_back(e.id.str() + ": " + lane.error().message);
                        placed = true;  // reported
                        continue;
                    }
                    lane->lane.curve = a;
                    for (auto& pt : lane->lane.curve.points)
                        pt.value = lane->toLaneDomain(pt.value);
                    st.lanes.push_back(std::move(lane->lane));
                    placed = true;
                }
            if (placed) continue;
        }
        if (slot)
            *slot = std::move(a);
        else
            p.warnings.push_back(e.id.str() + ": automation of " + o.str() + " " + param +
                                 " is not rendered yet");
    }
    // Input monitoring and the live region (ADR-0014): a track monitors its
    // record input when monitor is on, or auto while armed; whatever it feeds
    // is live too (nodes are in topological order, so one pass propagates).
    for (Node& n : p.nodes) {
        const model::Track& t = *s.tracks.find(n.id);
        const bool monitoring = t.record.monitor == model::Monitor::On ||
                                (t.record.monitor == model::Monitor::Auto && t.record.armed);
        if (monitoring && t.record.input)
            if (const model::Route* in = s.routes.find(*t.record.input)) {
                if (in->from.hwChannels)
                    for (int ch : *in->from.hwChannels) n.monitorInputs.push_back(ch - 1);
                if (in->from.midiDevice)
                    n.midiIn =
                        Node::MidiIn{*in->from.midiDevice, in->midi ? in->midi->fromChannel : 0,
                                     in->midi ? in->midi->toChannel : 0, 0};
            }
        n.live = !n.monitorInputs.empty() || n.midiIn.has_value();
        for (std::size_t e : n.in) n.live = n.live || p.nodes[p.edges[e].from].live;
        p.anyLive = p.anyLive || n.live;
    }
    // The master is the output: live whenever anything is (even a monitored
    // track that feeds nothing).
    if (p.anyLive) p.nodes.back().live = true;
    return p;
}

// Activates instances, measures latencies, sizes buffers and delays.
Status prepare(Plan& p, const Session& s) {
    const std::size_t block = p.block;
    for (Node& n : p.nodes) {
        n.latChain = 0;
        int stageIndex = 0;
        for (Stage& st : n.chain) {
            if (st.instance) {
                host::Instance& in = *st.instance;
                in.serviceMainThread();
                if (in.active() && (in.sampleRate() != p.rate || in.maxFrames() < block))
                    in.deactivate();
                if (!in.active()) SUPPLE_TRY(in.activate(p.rate, 1, p.block));
                st.latency = in.latency();
                st.instrument = in.audioInputChannels() == 0;
                if (n.noteTarget < 0 && in.acceptsNotes()) n.noteTarget = stageIndex;
                if (in.audioOutputChannels() != 2)
                    return Error("engine.layout",
                                 in.info().name + " does not have a stereo output");
            }
            st.dry.init(st.latency);
            st.out.init(block);
            st.dryBuf.init(block);
            st.outStorage.resize(256);
            st.outEvents.storage = st.outStorage;
            n.latChain += st.latency;
            ++stageIndex;
        }
        n.sum.init(block);
        n.preFx.init(block);
        n.preFader.init(block);
        n.post.init(block);
    }
    // Latency compensation, in topological order.
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        Node& n = p.nodes[i];
        n.latIn = 0;
        for (std::size_t e : n.in)
            n.latIn = std::max(n.latIn, latTap(p.nodes[p.edges[e].from], p.edges[e].tap));
        for (std::size_t e : n.in) {
            Edge& ed = p.edges[e];
            ed.delay.init(n.latIn - latTap(p.nodes[ed.from], ed.tap));
            ed.tmp.init(block);
        }
    }
    const Node& master = p.nodes.back();
    p.outputLatency = master.latIn + master.latChain;
    // Automation lines up with the signal where it is applied: the fader sees
    // the track's input and chain latency, a route's gain its target's input.
    for (Node& n : p.nodes) {
        const std::int64_t at = n.latIn + n.latChain;
        for (Automation* a : {&n.autoVolume, &n.autoTrim, &n.autoPan, &n.autoWidth, &n.autoMute})
            a->shift(at);
        for (std::size_t ei : n.in)
            for (Automation* a :
                 {&p.edges[ei].autoVolume, &p.edges[ei].autoPan, &p.edges[ei].autoMute})
                a->shift(n.latIn);
        // A plugin's parameters: its input is the track's, delayed by the
        // stages before it.
        std::int64_t before = n.latIn;
        for (Stage& st : n.chain) {
            for (ParamLane& ln : st.lanes) ln.curve.shift(before);
            before += st.latency;
        }
    }
    // Audio items, like events shifted by the track's input latency.
    for (Node& n : p.nodes) {
        const model::Track* tr = s.tracks.find(n.id);
        for (const auto& e : s.items) {
            const model::Item& it = *e.value;
            if (it.track != n.id || it.mute || !it.activeTake) continue;
            const model::Take* take = s.takes.find(*it.activeTake);
            if (!take || !take->source || take->source->kind != Kind::Media) continue;
            const model::MediaSource* src = s.media.find(*take->source);
            if (!src) continue;
            std::filesystem::path path = src->path;
            if (path.is_relative()) {
                const auto inMedia = p.projectDir / s.project->mediaDir / path;
                path = std::filesystem::exists(inMedia) ? inMedia : p.projectDir / path;
            }
            // The take's playback variant (session-model §1.5): resample
            // (varispeed, the pitch follows the rate) or a pitch-preserving
            // stretch. Built and kept once per combination (engine.md §4).
            MediaVariant variant;
            variant.playrate = take->playrate > 0.0 ? take->playrate : 1.0;
            variant.pitch = take->pitch;
            variant.preservePitch = take->preservePitch;
            variant.pitchMode = take->pitchMode;
            auto data = p.media->get(path, p.rate, variant);
            if (!data) {
                p.warnings.push_back(e.id.str() + ": " + data.error().message + " (media " +
                                     take->source->str() + ")");
                continue;
            }
            // A resample cannot shift the pitch on its own; the stretch mode
            // can (the pitch follows the rate instead without one).
            if (!variant.stretch() && take->preservePitch && take->pitch != 0.0)
                p.warnings.push_back(e.id.str() + ": pitch " + std::to_string(take->pitch) +
                                     " st needs pitch_mode \"signalsmith\"; it is ignored and "
                                     "the take is resampled");
            if (!take->stretchMarkers.empty())
                p.warnings.push_back(e.id.str() +
                                     ": stretch_markers are not applied yet; the "
                                     "take plays at its playrate");
            const ItemSpan span = itemSpan(s, it, tr);
            const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
            Clip c;
            c.data = *data;
            c.start = time::samplesAt(span.startSec, p.rate) - origin + n.latIn;
            c.length =
                time::samplesAt(span.endSec, p.rate) - time::samplesAt(span.startSec, p.rate);
            // The item's source offset is in source seconds; the variant is
            // read one frame per timeline sample, so `speed()` source seconds
            // pass per timeline second.
            c.offset = time::samplesAt(take->startOffset, p.rate / variant.speed());
            c.loop = it.loop;
            c.mode = take->channelMode;
            const double g =
                dbToGain(it.gainDb) * dbToGain(take->gainDb) * (take->phaseInvert ? -1.0 : 1.0);
            const auto [pl, pr] = take->pan == 0.0
                                      ? std::pair{1.0, 1.0}
                                      : panGains(take->pan / 100.0, s.project->panLawDb);
            c.gainL = g * pl;
            c.gainR = g * pr;
            c.fadeIn = time::samplesAt(it.fadeIn.length, p.rate);
            c.fadeOut = time::samplesAt(it.fadeOut.length, p.rate);
            c.fadeInShape = it.fadeIn.shape;
            c.fadeOutShape = it.fadeOut.shape;
            n.clips.push_back(std::move(c));
        }
    }
    // Events, shifted by each track's input latency so its own notes stay
    // aligned with what arrives from upstream.
    for (Node& n : p.nodes) {
        n.events = trackEvents(s, n.id, p.startSeconds, p.rate);
        for (auto& ev : n.events) ev.sample += n.latIn;
        // Reserve the most events any block-long window holds (events are
        // sorted; after a seek, blocks start anywhere), so processBlock never
        // allocates.
        std::size_t windowMax = 0;
        for (std::size_t a = 0, b = 0; a < n.events.size(); ++a) {
            if (b < a) b = a;
            while (b < n.events.size() &&
                   n.events[b].sample < n.events[a].sample + static_cast<std::int64_t>(block))
                ++b;
            windowMax = std::max(windowMax, b - a);
        }
        for (Stage& st : n.chain) {
            // Automation events: the grid plus the most points in a block window.
            std::size_t lanes = 0;
            for (const ParamLane& ln : st.lanes) {
                std::size_t pts = 0;
                for (std::size_t a = 0, b = 0; a < ln.curve.points.size(); ++a) {
                    if (b < a) b = a;
                    while (b < ln.curve.points.size() &&
                           ln.curve.points[b].sample <
                               ln.curve.points[a].sample + static_cast<std::int64_t>(block))
                        ++b;
                    pts = std::max(pts, b - a);
                }
                lanes += block / kAutomationStep + 2 + pts;
            }
            st.blockEvents.reserve(windowMax + 1 + lanes);
        }
    }
    // The node graph for the worker pool: a node waits for its edges' sources.
    std::vector<std::vector<std::uint32_t>> deps(p.nodes.size());
    for (std::size_t i = 0; i < p.nodes.size(); ++i)
        for (std::size_t e : p.nodes[i].in)
            deps[i].push_back(static_cast<std::uint32_t>(p.edges[e].from));
    p.graph = std::make_unique<TaskGraph>();
    p.graph->build(deps);
    // The executors' shares (ADR-0014). Live propagates downstream, so a
    // renderer node never depends on a live one.
    std::vector<std::uint32_t> task(p.nodes.size(), 0);
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        (p.nodes[i].live ? p.liveNodes : p.rendererNodes).push_back(i);
        task[i] = static_cast<std::uint32_t>(p.rendererNodes.size() - 1);
        for (const Stage& st : p.nodes[i].chain)
            if (st.instance)
                (p.nodes[i].live ? p.liveInstances : p.rendererInstances).push_back(st.instance);
    }
    std::vector<std::vector<std::uint32_t>> rdeps;
    for (std::size_t i : p.rendererNodes) {
        rdeps.emplace_back();
        for (std::uint32_t d : deps[i]) rdeps.back().push_back(task[d]);
    }
    p.rendererGraph = std::make_unique<TaskGraph>();
    p.rendererGraph->build(rdeps);
    for (auto* list : {&p.rendererInstances, &p.liveInstances}) {
        std::sort(list->begin(), list->end());
        list->erase(std::unique(list->begin(), list->end()), list->end());
    }
    for (std::size_t i : p.liveNodes)
        for (std::size_t e : p.nodes[i].in)
            if (!p.nodes[p.edges[e].from].live) {
                p.edges[e].frontier = true;
                p.edges[e].ring = p.frontier.size();
                p.edges[e].frontierBuf.init(block);
                p.frontier.push_back(e);
            }
    return okStatus();
}

host::Transport transportAt(const Plan& p, std::int64_t sample, const LoopRange* loop) SUPPLE_RT {
    host::Transport t;
    const double seconds = p.startSeconds + static_cast<double>(sample) / p.rate;
    const double beat = p.timeMap->beatAt(seconds);
    const auto bb = p.timeMap->barBeat(beat);
    const auto meter = p.timeMap->meterAt(bb.bar);
    t.playing = true;
    t.tempo = p.timeMap->bpmAt(beat);
    t.beat = beat;
    t.barStartBeat = p.timeMap->beatOfBar(bb.bar);
    t.bar = bb.bar - 1;
    t.numerator = static_cast<std::uint16_t>(meter.numerator);
    t.denominator = static_cast<std::uint16_t>(meter.denominator);
    t.seconds = seconds;
    if (loop && loop->active()) {
        t.loopActive = true;
        t.loopStartSeconds = p.startSeconds + static_cast<double>(loop->start) / p.rate;
        t.loopEndSeconds = p.startSeconds + static_cast<double>(loop->end) / p.rate;
        t.loopStartBeat = p.timeMap->beatAt(t.loopStartSeconds);
        t.loopEndBeat = p.timeMap->beatAt(t.loopEndSeconds);
    }
    return t;
}

// One block of every node, in order. Returns false on a plugin error.
bool processNode(Plan& p, std::size_t index, std::int64_t pos, std::uint32_t n,
                 const host::Transport& transport, const void* self) SUPPLE_RT {
    Node& node = p.nodes[index];
    // 1. Sum the inputs (fixed order), each through its compensation delay.
    node.sum.clear(n);
    for (std::size_t ei : node.in) {
        Edge& e = p.edges[ei];
        const Node& src = p.nodes[e.from];
        const Stereo& tap = p.split && e.frontier           ? e.frontierBuf
                            : e.tap == model::Tap::PreFx    ? src.preFx
                            : e.tap == model::Tap::PreFader ? src.preFader
                                                            : src.post;
        e.tmp.copyFrom(tap, n);
        e.delay.process(e.tmp.l.data(), e.tmp.r.data(), n);
        const bool automated = e.autoVolume.active() || e.autoPan.active() || e.autoMute.active();
        const double sign = e.invert ? -1.0 : 1.0;
        float gl = static_cast<float>(sign * e.gainL), gr = static_cast<float>(sign * e.gainR);
        for (std::uint32_t i = 0; i < n; ++i) {
            if (automated) {
                const std::int64_t at = pos + i;
                const double g =
                    (e.autoMute.active() && e.autoMute.at(at) >= 0.5)
                        ? 0.0
                        : dbToGain(e.autoVolume.active() ? e.autoVolume.at(at) : e.volumeDb);
                const auto [pl, pr] = panGains(
                    (e.autoPan.active() ? e.autoPan.at(at) : e.pan) / 100.0, node.panLawDb);
                gl = static_cast<float>(sign * g * pl);
                gr = static_cast<float>(sign * g * pr);
            }
            float l = e.tmp.l[i], r = e.tmp.r[i];
            if (e.mono) l = r = 0.5f * (l + r);
            node.sum.l[i] += l * gl;
            node.sum.r[i] += r * gr;
        }
    }
    // Monitored device input enters with the inputs (spec §1.1).
    if (!node.monitorInputs.empty() && p.deviceIn) {
        const auto channel = [&](std::size_t k) -> const float* {
            const int c = node.monitorInputs[std::min(k, node.monitorInputs.size() - 1)];
            return c >= 0 && static_cast<std::uint32_t>(c) < p.deviceInChannels ? p.deviceIn[c]
                                                                                : nullptr;
        };
        if (const float* l = channel(0))
            for (std::uint32_t i = 0; i < n; ++i) node.sum.l[i] += l[i];
        if (const float* r = channel(1))
            for (std::uint32_t i = 0; i < n; ++i) node.sum.r[i] += r[i];
    }
    node.preFx.copyFrom(node.sum, n);
    // Audio items play after the pre-FX tap, before the chain (not when stopped).
    for (const Clip& c : node.clips) {
        if (!transport.playing) break;
        const std::int64_t from = std::max<std::int64_t>(pos, c.start);
        const std::int64_t to = std::min<std::int64_t>(pos + n, c.start + c.length);
        const auto& ch = c.data->channels;
        const auto frames = static_cast<std::int64_t>(c.data->frames());
        if (ch.empty() || frames == 0) continue;
        const std::size_t right = ch.size() > 1 ? 1 : 0;
        for (std::int64_t t = from; t < to; ++t) {
            const std::int64_t k = t - c.start;  // sample within the item
            std::int64_t f = c.offset + k;
            if (c.loop) f = ((f % frames) + frames) % frames;
            if (f < 0 || f >= frames) continue;
            const auto fi = static_cast<std::size_t>(f);
            float l = ch[0][fi], r = ch[right][fi];
            switch (c.mode) {
                case model::ChannelMode::Normal:
                    break;
                case model::ChannelMode::ReverseStereo:
                    std::swap(l, r);
                    break;
                case model::ChannelMode::MonoMix:
                    l = r = 0.5f * (l + r);
                    break;
                case model::ChannelMode::Left:
                    r = l;
                    break;
                case model::ChannelMode::Right:
                    l = r;
                    break;
            }
            double fade = 1.0;
            if (c.fadeIn > 0 && k < c.fadeIn)
                fade *= fadeCurve(c.fadeInShape,
                                  static_cast<double>(k) / static_cast<double>(c.fadeIn));
            if (c.fadeOut > 0 && k >= c.length - c.fadeOut)
                fade *= fadeCurve(c.fadeOutShape, static_cast<double>(c.length - k) /
                                                      static_cast<double>(c.fadeOut));
            const auto i = static_cast<std::size_t>(t - pos);
            node.sum.l[i] += static_cast<float>(static_cast<double>(l) * c.gainL * fade);
            node.sum.r[i] += static_cast<float>(static_cast<double>(r) * c.gainR * fade);
        }
    }
    // 2. Route this block's events to the note target. Notes still held at
    // a loop wrap end first, at the start of the block after it.
    for (Stage& st : node.chain) st.blockEvents.clear();
    if (node.releaseHeld) {
        node.releaseHeld = false;
        if (node.noteTarget >= 0) {
            auto& target = node.chain[static_cast<std::size_t>(node.noteTarget)].blockEvents;
            for (std::size_t k = 0; k < node.held.size(); ++k) {
                if (node.held[k] == 0 || target.size() >= target.capacity()) continue;
                host::Event off;
                off.kind = host::Event::Kind::NoteOff;
                off.channel = static_cast<std::int16_t>(k / 128);
                off.key = static_cast<std::int16_t>(k % 128);
                off.velocity = 0.0;
                target.push_back(off);
            }
        }
        node.held.fill(0);
    }
    while (transport.playing && node.nextEvent < node.events.size() &&
           node.events[node.nextEvent].sample < pos + n) {
        const TimedEvent& te = node.events[node.nextEvent++];
        if (node.noteTarget < 0) continue;
        host::Event e = te.event;
        e.time = static_cast<std::uint32_t>(std::max<std::int64_t>(0, te.sample - pos));
        auto& target = node.chain[static_cast<std::size_t>(node.noteTarget)].blockEvents;
        if (target.size() < target.capacity()) target.push_back(e);  // never allocate here
        if (e.kind == host::Event::Kind::NoteOn || e.kind == host::Event::Kind::NoteOff) {
            const auto k = static_cast<std::size_t>((e.channel & 15) * 128 + (e.key & 127));
            if (e.kind == host::Event::Kind::NoteOn && node.held[k] < 255)
                ++node.held[k];
            else if (e.kind == host::Event::Kind::NoteOff && node.held[k] > 0)
                --node.held[k];
        }
    }
    // Live MIDI from the device (ADR-0016), playing or not, after timeline MIDI
    // at the same frame; not counted as held (a loop wrap does not cut what
    // the performer holds).
    if (node.midiIn && node.noteTarget >= 0 && !p.deviceMidi.empty()) {
        auto& target = node.chain[static_cast<std::size_t>(node.noteTarget)].blockEvents;
        const std::size_t before = target.size();
        for (const MidiInputEvent& ev : p.deviceMidi) {
            if (ev.frame < p.deviceMidiOffset) continue;
            if (ev.frame >= p.deviceMidiOffset + n || target.size() >= target.capacity()) break;
            std::uint8_t m[3];
            if (!routeMidi(ev, node.midiIn->ports, node.midiIn->fromChannel, node.midiIn->toChannel,
                           m))
                continue;
            host::Event e;
            e.time = ev.frame - p.deviceMidiOffset;
            e.channel = static_cast<std::int16_t>(m[0] & 0x0f);
            const int kind = m[0] & 0xf0;
            if (kind == 0x90 && m[2] > 0) {
                e.kind = host::Event::Kind::NoteOn;
                e.key = m[1];
                e.velocity = m[2] / 127.0;
            } else if (kind == 0x80 || kind == 0x90) {
                e.kind = host::Event::Kind::NoteOff;
                e.key = m[1];
                e.velocity = m[2] / 127.0;
            } else {
                e.kind = host::Event::Kind::Midi;
                std::copy_n(m, 3, e.midi);
            }
            target.push_back(e);
            if (e.kind == host::Event::Kind::NoteOn || e.kind == host::Event::Kind::NoteOff) {
                const auto k = static_cast<std::size_t>((e.channel & 15) * 128 + (e.key & 127));
                if (e.kind == host::Event::Kind::NoteOn && node.liveHeld[k] < 255)
                    ++node.liveHeld[k];
                else if (e.kind == host::Event::Kind::NoteOff && node.liveHeld[k] > 0)
                    --node.liveHeld[k];
            }
        }
        if (target.size() > before)  // stable insertion sort by time: no allocation
            for (std::size_t i = 1; i < target.size(); ++i)
                for (std::size_t j = i; j > 0 && target[j - 1].time > target[j].time; --j)
                    std::swap(target[j - 1], target[j]);
    }
    // 2b. Plugin parameter automation: values at the block start, every
    // kAutomationStep samples and at each point, in time order (the lane's
    // cursor only moves forward), merged into the block's sorted events.
    for (Stage& st : node.chain) {
        if (st.lanes.empty()) continue;
        const std::size_t before = st.blockEvents.size();
        for (ParamLane& ln : st.lanes) {
            const auto& pts = ln.curve.points;
            auto pi = static_cast<std::size_t>(
                std::lower_bound(
                    pts.begin(), pts.end(), pos,
                    [](const Automation::Pt& pt, std::int64_t x) { return pt.sample < x; }) -
                pts.begin());
            std::uint32_t grid = 0;
            for (;;) {
                const std::uint32_t atPoint = pi < pts.size() && pts[pi].sample < pos + n
                                                  ? static_cast<std::uint32_t>(pts[pi].sample - pos)
                                                  : n;
                const std::uint32_t at = std::min(grid, atPoint);
                if (at >= n) break;
                const double v = ln.clapAt(pos + at);
                if (v != ln.lastSent && st.blockEvents.size() < st.blockEvents.capacity()) {
                    host::Event e;
                    e.kind = host::Event::Kind::Param;
                    e.time = at;
                    e.paramId = ln.clapId;
                    e.value = v;
                    st.blockEvents.push_back(e);
                    ln.lastSent = v;
                }
                if (at == grid) grid += kAutomationStep;
                if (at == atPoint) ++pi;
            }
        }
        if (st.blockEvents.size() > before)  // stable insertion sort by time: no allocation
            for (std::size_t i = 1; i < st.blockEvents.size(); ++i)
                for (std::size_t j = i;
                     j > 0 && st.blockEvents[j - 1].time > st.blockEvents[j].time; --j)
                    std::swap(st.blockEvents[j - 1], st.blockEvents[j]);
    }
    // 3. The FX chain, in place on `sum`.
    for (Stage& st : node.chain) {
        // A stage whose plugin is not processing (start_processing failed)
        // passes audio through, delay-compensated, like a bypass.
        if (!st.instance || !st.instance->processing() || (self && st.instance->owner() != self)) {
            st.dry.process(node.sum.l.data(), node.sum.r.data(), n);  // compensated bypass
            st.appliedWet = -1.0f;
            continue;
        }
        // The wet level to reach (0 bypassed); a change ramps from the level
        // last applied, the plugin running until the ramp is done.
        const auto target = static_cast<float>(st.bypass ? 0.0 : st.instrument ? 1.0 : st.wet);
        if (st.appliedWet < 0.0f) st.appliedWet = target;  // new here: no ramp
        if (st.wetRampLeft ? target != st.rampToWet : target != st.appliedWet) {
            st.rampFromWet = st.appliedWet;
            st.rampToWet = target;
            st.wetRampLeft = p.smoothFrames;
        }
        if (st.bypass && st.wetRampLeft == 0) {  // bypassed and faded out
            st.dry.process(node.sum.l.data(), node.sum.r.data(), n);
            st.appliedWet = 0.0f;
            continue;
        }
        const float* in[2] = {node.sum.l.data(), node.sum.r.data()};
        float* out[2] = {st.out.l.data(), st.out.r.data()};
        host::ProcessBlock b;
        b.inputs = st.instrument ? nullptr : in;
        b.inputChannels = st.instrument ? 0 : 2;
        b.outputs = out;
        b.outputChannels = 2;
        b.frames = n;
        b.events = st.blockEvents;
        b.transport = &transport;
        b.steadyTime = pos;
        st.outEvents.clear();
        if (!st.instance->process(b, &st.outEvents)) return false;
        // Dry signal delayed by the stage's latency: an instrument adds its
        // output to what came in; an effect mixes wet and dry.
        st.dryBuf.copyFrom(node.sum, n);
        st.dry.process(st.dryBuf.l.data(), st.dryBuf.r.data(), n);
        if (st.wetRampLeft == 0) {
            const float wet = target, dryGain = st.instrument ? 1.0f : 1.0f - wet;
            for (std::uint32_t i = 0; i < n; ++i) {
                node.sum.l[i] = st.out.l[i] * wet + st.dryBuf.l[i] * dryGain;
                node.sum.r[i] = st.out.r[i] * wet + st.dryBuf.r[i] * dryGain;
            }
            st.appliedWet = target;
        } else {
            // Crossfading (an instrument's "dry" is what came in: it stays).
            const float span = static_cast<float>(p.smoothFrames);
            for (std::uint32_t i = 0; i < n; ++i) {
                float wet = target;
                if (st.wetRampLeft > 0) {
                    const float t = 1.0f - static_cast<float>(st.wetRampLeft) / span;
                    wet = st.rampFromWet + (st.rampToWet - st.rampFromWet) * t;
                    --st.wetRampLeft;
                    if (st.wetRampLeft == 0) wet = st.rampToWet;
                }
                const float dryGain = st.instrument ? 1.0f : 1.0f - wet;
                node.sum.l[i] = st.out.l[i] * wet + st.dryBuf.l[i] * dryGain;
                node.sum.r[i] = st.out.r[i] * wet + st.dryBuf.r[i] * dryGain;
                st.appliedWet = wet;
            }
        }
    }
    node.preFader.copyFrom(node.sum, n);
    // 4. Width, volume, pan, mute (and solo). A changed static value ramps
    // from the one last applied (carried across plan swaps).
    float w = static_cast<float>(node.width);
    const bool silent = node.mute || node.soloSilenced;
    float gl = silent ? 0.0f : static_cast<float>(node.volume * node.panL);
    float gr = silent ? 0.0f : static_cast<float>(node.volume * node.panR);
    const bool automated = node.automated();
    if (!automated) {
        const Node::Mix target{gl, gr, w};
        if (!node.mixValid) {
            node.applied = target;
            node.mixValid = true;
        }
        if (!(target == node.applied) && !(node.rampLeft > 0 && target == node.rampTo)) {
            node.rampFrom = node.applied;
            node.rampTo = target;
            node.rampLeft = p.smoothFrames;
        }
    } else {
        node.rampLeft = 0;  // the envelope decides
    }
    const float span = static_cast<float>(p.smoothFrames);
    for (std::uint32_t i = 0; i < n; ++i) {
        if (!automated && node.rampLeft > 0) {
            const float t = 1.0f - static_cast<float>(node.rampLeft) / span;
            gl = node.rampFrom.gl + (node.rampTo.gl - node.rampFrom.gl) * t;
            gr = node.rampFrom.gr + (node.rampTo.gr - node.rampFrom.gr) * t;
            w = node.rampFrom.w + (node.rampTo.w - node.rampFrom.w) * t;
            if (--node.rampLeft == 0) {
                gl = node.rampTo.gl;
                gr = node.rampTo.gr;
                w = node.rampTo.w;
            }
        } else if (automated) {
            const std::int64_t at = pos + i;
            const bool muted = node.autoMute.active() ? node.autoMute.at(at) >= 0.5 : node.mute;
            const double db = (node.autoVolume.active() ? node.autoVolume.at(at) : node.volumeDb) +
                              (node.autoTrim.active() ? node.autoTrim.at(at) : 0.0);
            double pl = node.panL, pr = node.panR;
            if (node.autoPan.active()) {
                const double pan = std::clamp(node.autoPan.at(at) / 100.0, -1.0, 1.0);
                if (node.master) {
                    pl = pan <= 0 ? 1.0 : 1.0 - pan;
                    pr = pan >= 0 ? 1.0 : 1.0 + pan;
                } else {
                    std::tie(pl, pr) = panGains(pan, node.panLawDb);
                }
            }
            const double g = muted || node.soloSilenced ? 0.0 : dbToGain(db);
            gl = static_cast<float>(g * pl);
            gr = static_cast<float>(g * pr);
            if (node.autoWidth.active()) w = static_cast<float>(node.autoWidth.at(at) / 100.0);
        }
        const float mid = 0.5f * (node.sum.l[i] + node.sum.r[i]);
        const float side = 0.5f * (node.sum.l[i] - node.sum.r[i]) * w;
        node.post.l[i] = (mid + side) * gl;
        node.post.r[i] = (mid - side) * gr;
    }
    node.applied = {gl, gr, w};
    node.mixValid = true;
    if (node.soloSilenced) {  // processed (plugin state continues) but not heard
        node.preFx.clear(n);  // (the pre-fader taps switch at once; the post ramps)
        node.preFader.clear(n);
    }
    return true;
}

bool processBlock(Plan& p, std::int64_t pos, std::uint32_t n, bool playing,
                  const LoopRange* loop) SUPPLE_RT {
    host::Transport transport = transportAt(p, pos, loop);
    transport.playing = playing;
    for (std::size_t i = 0; i < p.nodes.size(); ++i)
        if (!processNode(p, i, pos, n, transport)) return false;
    return true;
}

namespace {
struct BlockRun {
    Plan* plan;
    std::int64_t pos;
    std::uint32_t frames;
    const host::Transport* transport;
    const std::vector<std::size_t>* nodes = nullptr;  // task -> node; null: identity
    const void* self = nullptr;
    std::atomic<bool> ok{true};
};
void runNode(void* ctx, std::uint32_t task) SUPPLE_RT {
    auto* b = static_cast<BlockRun*>(ctx);
    const std::size_t index = b->nodes ? (*b->nodes)[task] : task;
    if (!processNode(*b->plan, index, b->pos, b->frames, *b->transport, b->self))
        b->ok.store(false, std::memory_order_relaxed);
}
}  // namespace

bool processRendererBlock(Workers* workers, Plan& p, std::int64_t pos, std::uint32_t n,
                          const void* self, const LoopRange* loop) {
    const host::Transport transport = transportAt(p, pos, loop);
    if (!workers || workers->helpers() == 0) {
        bool ok = true;
        for (std::size_t i : p.rendererNodes) ok = processNode(p, i, pos, n, transport, self) && ok;
        return ok;
    }
    BlockRun run{&p, pos, n, &transport, &p.rendererNodes, self};
    workers->run(*p.rendererGraph, &runNode, &run);
    return run.ok.load(std::memory_order_relaxed);
}

void locateClass(Plan& p, std::int64_t pos, bool live) SUPPLE_RT {
    for (Node& n : p.nodes) {
        if (n.live != live) continue;
        auto it =
            std::lower_bound(n.events.begin(), n.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        n.nextEvent = static_cast<std::size_t>(it - n.events.begin());
        if (!n.releaseHeld) n.held.fill(0);
        for (Automation* a : {&n.autoVolume, &n.autoTrim, &n.autoPan, &n.autoWidth, &n.autoMute})
            a->cursor = 0;
        for (Stage& st : n.chain) {
            if (!st.preserveDryOnLocate) st.dry.clear();
            st.preserveDryOnLocate = false;
            for (ParamLane& ln : st.lanes) {
                ln.curve.cursor = 0;
                ln.lastSent = std::numeric_limits<double>::quiet_NaN();
            }
        }
        for (std::size_t ei : n.in) {
            Edge& e = p.edges[ei];
            e.delay.clear();
            for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute}) a->cursor = 0;
        }
    }
}

bool processBlockOn(Workers& workers, Plan& p, std::int64_t pos, std::uint32_t n) {
    const host::Transport transport = transportAt(p, pos);
    BlockRun run{&p, pos, n, &transport};
    workers.run(*p.graph, &runNode, &run);
    return run.ok.load(std::memory_order_relaxed);
}

void locate(Plan& p, std::int64_t pos) SUPPLE_RT {
    for (Node& n : p.nodes) {
        auto it =
            std::lower_bound(n.events.begin(), n.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        n.nextEvent = static_cast<std::size_t>(it - n.events.begin());
        if (!n.releaseHeld) n.held.fill(0);
        for (Automation* a : {&n.autoVolume, &n.autoTrim, &n.autoPan, &n.autoWidth, &n.autoMute})
            a->cursor = 0;
        for (Stage& st : n.chain) {
            if (!st.preserveDryOnLocate) st.dry.clear();
            st.preserveDryOnLocate = false;
            for (ParamLane& ln : st.lanes) {
                ln.curve.cursor = 0;
                ln.lastSent = std::numeric_limits<double>::quiet_NaN();  // resend at once
            }
        }
    }
    for (Edge& e : p.edges) {
        e.delay.clear();
        for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute}) a->cursor = 0;
    }
}

void wrapTo(Plan& p, std::int64_t pos, int liveClass) SUPPLE_RT {
    for (Node& n : p.nodes) {
        if (liveClass >= 0 && n.live != (liveClass == 1)) continue;
        auto it =
            std::lower_bound(n.events.begin(), n.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        n.nextEvent = static_cast<std::size_t>(it - n.events.begin());
        n.releaseHeld = true;
        for (Automation* a : {&n.autoVolume, &n.autoTrim, &n.autoPan, &n.autoWidth, &n.autoMute})
            a->cursor = 0;
        for (Stage& st : n.chain)
            for (ParamLane& ln : st.lanes) {
                ln.curve.cursor = 0;
                ln.lastSent = std::numeric_limits<double>::quiet_NaN();  // the loop start's value
            }
        for (std::size_t ei : n.in)
            for (Automation* a :
                 {&p.edges[ei].autoVolume, &p.edges[ei].autoPan, &p.edges[ei].autoMute})
                a->cursor = 0;
    }
}

void carryLiveNotes(Plan& from, Plan& to, bool liveOnly) SUPPLE_RT {
    for (Node& old : from.nodes) {
        if (!old.midiIn) continue;
        const bool sounding = std::any_of(old.liveHeld.begin(), old.liveHeld.end(),
                                          [](std::uint8_t c) { return c != 0; });
        if (!sounding) continue;
        for (Node& n : to.nodes) {
            if (n.id != old.id) continue;
            if (liveOnly && !n.live) break;
            const bool sameRoute = n.midiIn && n.midiIn->ports == old.midiIn->ports &&
                                   n.midiIn->fromChannel == old.midiIn->fromChannel &&
                                   n.midiIn->toChannel == old.midiIn->toChannel;
            if (sameRoute) {
                n.liveHeld = old.liveHeld;  // still live: its note-offs will come
            } else {
                for (std::size_t k = 0; k < n.held.size(); ++k)
                    n.held[k] =
                        static_cast<std::uint8_t>(std::min(255, n.held[k] + old.liveHeld[k]));
                n.releaseHeld = true;  // ended at the start of the next block
            }
            break;
        }
    }
}

void carryMix(Plan& from, Plan& to, int liveClass) SUPPLE_RT {
    for (Node& n : to.nodes) {
        if (liveClass >= 0 && n.live != (liveClass == 1)) continue;
        for (Node& old : from.nodes) {  // (plans are small; a swap is rare)
            if (old.id != n.id) continue;
            // Only what this executor processed (the other one writes the rest).
            if (liveClass >= 0 && old.live != (liveClass == 1)) break;
            n.applied = old.applied;
            n.mixValid = old.mixValid;
            n.rampFrom = old.rampFrom;
            n.rampTo = old.rampTo;
            n.rampLeft = old.rampLeft;
            for (Stage& st : n.chain)
                for (Stage& os : old.chain)
                    if (st.instance && os.instance == st.instance) {
                        st.appliedWet = os.appliedWet;
                        st.rampFromWet = os.rampFromWet;
                        st.rampToWet = os.rampToWet;
                        st.wetRampLeft = os.wetRampLeft;
                        if (st.dry.length == os.dry.length) {
                            std::swap(st.dry.buf[0], os.dry.buf[0]);
                            std::swap(st.dry.buf[1], os.dry.buf[1]);
                            st.dry.pos = os.dry.pos;
                            st.preserveDryOnLocate = true;
                        }
                        break;
                    }
            break;
        }
    }
}

}  // namespace supple::engine::detail
