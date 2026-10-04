#include "plan.h"
#include "supple/model/fade_shape.h"

#include <atomic>
#include <cassert>
#include <cstdio>
#include <map>
#include <set>

#include "supple/clap/semantic_params.h"

namespace supple::engine::detail {

namespace {

std::int64_t latTap(const Node& n, model::Tap tap) {
    return tap == model::Tap::PreFx ? n.latIn : n.latIn + n.latChain;
}

double fadeCurve(model::FadeShape shape, double curve, double t) {
    return model::fadeGain(shape, curve, t);
}

// Note and control events of the MIDI items on `track`, in samples from the
// render start (before PDC shifting).
// `include`: which of the track's items to take (empty: all).
std::vector<TimedEvent> trackEvents(const Session& s, Id track, double startSeconds, double rate,
                                    const std::function<bool(Id item)>& include = {}) {
    std::vector<std::pair<int, TimedEvent>> items;  // rank (0 off, 1 ctl, 2 on), event
    const time::TimeMap& tm = *s.timeMap;
    const std::int64_t origin = time::samplesAt(startSeconds, rate);
    std::int32_t noteId = 0;
    for (const auto& e : s.items) {
        const model::Item& it = *e.value;
        if (it.track != track || it.mute || !it.activeTake) continue;
        if (include && !include(e.id)) continue;
        const model::Take* take = s.takes.find(*it.activeTake);
        if (!take || !take->source || take->source->kind != Kind::Midi) continue;
        const model::MidiSource* src = s.midi.find(*take->source);
        if (!src) continue;
        const midi::Sequence& seq = src->sequence;
        const model::ItemSpan span = model::itemSpan(s, it);
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
    for (const auto& e : s.routes) {
        const model::Route& r = *e.value;
        const std::optional<Id> to = r.to.fx ? model::trackOfFx(s, *r.to.fx) : r.to.track;
        if (r.from.track && to && !r.mute) link(*r.from.track, *to);
    }
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

// A plain lane whose instance takes normalized parameter values (VST3): the
// envelope points move into the plugin's normalized domain, so the events the
// audio thread sends need no conversion there. An edit controller is called
// here, on the compile (main) thread, never on the audio thread.
bool toNormalizedDomain(host::Instance& in, ParamLane& lane) {
    for (auto& pt : lane.curve.points) {
        auto n = in.normalizedParam(lane.clapId, pt.value);
        if (!n) return false;
        pt.value = *n;
    }
    return true;
}

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
    p.linearConsole = !s.project->console9;
    p.rate = rate > 0 ? rate : s.project->sampleRate;
    p.smoothFrames = static_cast<std::uint32_t>(std::max(1.0, std::round(0.010 * p.rate)));
    p.block = block;
    p.startSeconds = startSeconds;
    p.timeMap = s.timeMap;
    auto order = topoOrder(s);
    if (!order) return std::move(order).error();
    std::map<Id, std::size_t> index;
    for (std::size_t i = 0; i < order->size(); ++i) index[(*order)[i]] = i;
    p.mainMaster = index.at(model::kMaster);
    p.nodes.resize(order->size());
    for (std::size_t i = 0; i < order->size(); ++i) {
        Node& n = p.nodes[i];
        n.id = (*order)[i];
        const model::Track& t = *s.tracks.find(n.id);
        n.volumeDb = t.volumeDb;
        n.pan = t.pan;
        n.type = t.type;
        n.master = t.type == model::StripType::Master;
        n.decodeInput = t.type != model::StripType::Channel;
        n.staticGain = n.master                          ? dbToGain(t.volumeDb)
                       : t.volumeDb <= model::kSilenceDb ? 0.0
                                                         : console9::faderGain(t.volumeDb);
        n.staticPan = console9::nativePan(t.pan);
        n.encoder.init(console9::seedsFor(s.project->seed, (n.id.n << 3) | 1), p.linearConsole);
        n.decoder.init(console9::seedsFor(s.project->seed, (n.id.n << 3) | 2), p.linearConsole);
        n.mute = t.mute;
        auto makeStage = [&](Id fxId) {
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
            return st;
        };
        n.auxCount = std::clamp(t.channels - 2, 0, 6);
        const int channels = 2 + n.auxCount;
        for (Id fxId : t.chain) {
            Stage st = makeStage(fxId);
            // Pins (ADR-0020): which track channels the plug-in reads, writes
            // and keys from.
            const model::PinMap& pins = s.fx.find(fxId)->pins;
            auto valid = [&](const std::vector<int>& list, const char* what) {
                for (int c : list)
                    if (c < 1 || c > channels) {
                        p.warnings.push_back(fxId.str() + ": " + what + " pin " +
                                             std::to_string(c) + " is beyond the track's " +
                                             std::to_string(channels) + " channels; ignored");
                        return false;
                    }
                return !list.empty();
            };
            if (valid(pins.in, "input")) {
                st.inPin[0] = pins.in[0] - 1;
                st.inPin[1] = pins.in[pins.in.size() > 1 ? 1 : 0] - 1;
            }
            if (valid(pins.out, "output")) {
                st.outPin[0] = pins.out[0] - 1;
                st.outPin[1] = pins.out.size() > 1 ? pins.out[1] - 1 : -1;
            }
            st.pinned =
                st.inPin[0] != 0 || st.inPin[1] != 1 || st.outPin[0] != 0 || st.outPin[1] != 1;
            if (valid(pins.key, "key")) {
                if (!st.instance || st.instance->sidechainChannels() == 0) {
                    p.warnings.push_back(fxId.str() +
                                         " has no sidechain input; its key pins are ignored");
                } else {
                    for (int c : pins.key) st.keyPins.push_back(c - 1);
                    st.keyed = true;
                }
            }
            n.chain.push_back(std::move(st));
        }
        // Takes with FX or envelopes play through a chain of their own (gap log #22).
        for (const auto& e : s.items) {
            const model::Item& it = *e.value;
            if (it.track != n.id || it.mute || !it.activeTake) continue;
            const model::Take* take = s.takes.find(*it.activeTake);
            if (!take) continue;
            const bool enveloped =
                std::any_of(s.envelopes.begin(), s.envelopes.end(), [&](auto& en) {
                    return en.value->target.object == *it.activeTake && en.value->active &&
                           !en.value->points.empty();
                });
            if (take->chain.empty() && !enveloped) continue;
            TakeChain tc;
            tc.item = e.id;
            tc.take = *it.activeTake;
            for (Id fxId : take->chain) tc.chain.push_back(makeStage(fxId));
            n.takeChains.push_back(std::move(tc));
        }
    }
    for (const Node& n : p.nodes) {
        for (const Stage& st : n.chain)
            if (st.instance) p.instances.push_back(st.instance);
        for (const TakeChain& tc : n.takeChains)
            for (const Stage& st : tc.chain)
                if (st.instance) p.instances.push_back(st.instance);
    }
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
        const std::optional<Id> toTrack = r.to.fx ? model::trackOfFx(s, *r.to.fx) : r.to.track;
        if (!r.from.track || !toTrack) continue;
        Edge ed;
        ed.from = index.at(*r.from.track);
        ed.route = e.id;
        if (r.to.fx) {  // a key route: into that stage's sidechain input (ADR-0018)
            const model::Track& owner = *s.tracks.find(*toTrack);
            ed.stage = static_cast<int>(
                std::find(owner.chain.begin(), owner.chain.end(), *r.to.fx) - owner.chain.begin());
            Stage& st = p.nodes[index.at(*toTrack)].chain[static_cast<std::size_t>(ed.stage)];
            if (!st.instance || st.instance->sidechainChannels() == 0) {
                p.warnings.push_back(e.id.str() + ": " + r.to.fx->str() +
                                     " has no sidechain input; the key route is ignored");
                continue;
            }
            st.keyed = true;
        }
        ed.tap = r.tap;
        ed.volumeDb = r.volumeDb;
        ed.followPan = !r.pan.percent;
        ed.muted = r.mute;
        ed.pan = r.pan.resolve(s.tracks.find(*r.from.track)->pan);
        const double g = dbToGain(r.volumeDb);
        auto [pl, pr] = panGains(ed.pan / 100.0, -3.0);
        ed.gainL = g * pl;
        ed.gainR = g * pr;
        ed.mono = r.mono;
        ed.invert = r.phaseInvert;
        if (!r.channels.from.empty()) {
            // Main pairs make the matrix; pairs that touch auxiliary channels
            // become links (ADR-0020).
            const int fromChannels = 2 + p.nodes[ed.from].auxCount;
            const int toChannels = r.to.fx ? 2 : 2 + p.nodes[index.at(*toTrack)].auxCount;
            float m[2][2] = {{0.0f, 0.0f}, {0.0f, 0.0f}};
            bool anyMain = false;
            for (std::size_t k = 0; k < r.channels.from.size() && k < r.channels.to.size(); ++k) {
                const int from = r.channels.from[k], to = r.channels.to[k];
                if (from < 1 || from > fromChannels || to < 1 || to > toChannels) {
                    p.warnings.push_back(e.id.str() + ": channel " +
                                         std::to_string(std::max(from, to)) +
                                         " is beyond its strip's channels; that pair is ignored");
                    continue;
                }
                if (from > 2 || to > 2) {
                    Edge::AuxLink link;
                    link.from = from - 1;
                    link.to = to - 1;
                    ed.auxLinks.push_back(std::move(link));
                    continue;
                }
                m[to - 1][from - 1] += 1.0f;
                anyMain = true;
            }
            ed.auxOnly = !anyMain && !ed.auxLinks.empty();
            ed.mapped = !(m[0][0] == 1 && m[0][1] == 0 && m[1][0] == 0 && m[1][1] == 1);
            std::copy(&m[0][0], &m[0][0] + 4, &ed.map[0][0]);
        }
        pending.push_back({ed.from, index.at(*toTrack), std::move(ed), 1 + e.id.n});
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
        // What plays: the envelope with its automation items (gap log #28).
        const std::vector<model::EnvPoint> played =
            env.target.object.kind == Kind::Take ? env.points : model::effectivePoints(s, e.id);
        if (!env.active || played.empty()) continue;
        Automation a;
        for (const model::EnvPoint& pt : played) {
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
            slot = param == "volume" ? &n.autoVolume
                   : param == "pan"  ? &n.autoPan
                   : param == "mute" ? &n.autoMute
                                     : nullptr;
        } else if (o.kind == Kind::Route) {
            for (Edge& ed : p.edges)
                if (ed.route == o)
                    slot = param == "volume" ? &ed.autoVolume
                           : param == "pan"  ? &ed.autoPan
                           : param == "mute" ? &ed.autoMute
                                             : nullptr;
        } else if (o.kind == Kind::Take) {
            // Positions are item-relative beats: they move with the item.
            for (Node& n : p.nodes)
                for (TakeChain& tc : n.takeChains) {
                    if (tc.take != o) continue;
                    const model::Item* item = s.items.find(tc.item);
                    if (!item) continue;
                    const double startBeat = model::itemSpan(s, *item).startBeat;
                    a.points.clear();
                    for (const model::EnvPoint& pt : env.points)
                        a.points.push_back(
                            {time::samplesAt(s.timeMap->secondsAt(startBeat + pt.pos), p.rate) -
                                 origin,
                             pt.value, pt.shape, pt.tension});
                    slot = param == "volume" ? &tc.volume
                           : param == "pan"  ? &tc.pan
                           : param == "mute" ? &tc.mute
                                             : nullptr;
                }
            if (!slot) continue;  // an inactive take's, or not one the engine plays
        } else if (o.kind == Kind::Fx) {
            bool placed = false;
            std::vector<Stage*> stages;  // track chains, then take chains
            for (Node& n : p.nodes) {
                for (Stage& st : n.chain) stages.push_back(&st);
                for (TakeChain& tc : n.takeChains)
                    for (Stage& st : tc.chain) stages.push_back(&st);
            }
            for (Stage* stage : stages) {
                Stage& st = *stage;
                {
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
                    // A VST3 processor is given normalized parameter values, so
                    // a plain lane for such an instance interpolates in that
                    // domain (a semantic lane already interpolates in its own
                    // normalized domain). The conversion is a controller call:
                    // here, on the compile thread, never on the audio thread.
                    if (!lane->lane.spec && st.instance->paramsNormalized() &&
                        !toNormalizedDomain(*st.instance, lane->lane)) {
                        p.warnings.push_back(e.id.str() + ": " + o.str() + " " + param +
                                             " cannot be automated by this plugin");
                        placed = true;
                        continue;
                    }
                    st.lanes.push_back(std::move(lane->lane));
                    placed = true;
                }
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
    if (p.anyLive) {
        p.nodes[p.mainMaster].live = true;
        // A main-master key may feed later strips, which must stay with the live executor.
        for (Node& n : p.nodes)
            for (std::size_t edge : n.in) n.live = n.live || p.nodes[p.edges[edge].from].live;
        // Auxiliary links do not cross the frontier (its rings carry the main
        // pair): a strip that feeds a live strip's aux channels, or is fed
        // from aux into a live one, plays live too, and what it feeds.
        for (bool changed = true; changed;) {
            changed = false;
            for (const Edge& e : p.edges)
                if (!e.auxLinks.empty() && !p.nodes[e.from].live) {
                    const auto to = static_cast<std::size_t>(
                        std::find_if(p.nodes.begin(), p.nodes.end(),
                                     [&](const Node& n) {
                                         return std::find(n.in.begin(), n.in.end(),
                                                          static_cast<std::size_t>(
                                                              &e - p.edges.data())) != n.in.end();
                                     }) -
                        p.nodes.begin());
                    if (to < p.nodes.size() && p.nodes[to].live) {
                        p.nodes[e.from].live = true;
                        changed = true;
                    }
                }
            for (Node& n : p.nodes)
                for (std::size_t edge : n.in)
                    if (!n.live && p.nodes[p.edges[edge].from].live) {
                        n.live = true;
                        changed = true;
                    }
        }
    }
    for (Edge& e : p.edges) {
        if (e.stage >= 0) {
            e.stripPan = p.nodes[e.from].autoPan;
            continue;
        }
        if (e.auxOnly) continue;  // nothing reaches the main pair
        e.encode = e.tap != model::Tap::PostFader || e.volumeDb != 0 || !e.followPan || e.mono ||
                   e.invert || e.mapped || e.muted || e.autoVolume.active() || e.autoPan.active() ||
                   e.autoMute.active();
        if (!e.encode) continue;
        Node& source = p.nodes[e.from];
        source.out.push_back(static_cast<std::size_t>(&e - p.edges.data()));
        e.stripVolume = source.autoVolume;
        e.stripPan = source.autoPan;
        e.stripMute = source.autoMute;
        const double db = e.volumeDb <= model::kSilenceDb || (e.tap == model::Tap::PostFader &&
                                                              source.volumeDb <= model::kSilenceDb)
                              ? model::kSilenceDb
                              : e.volumeDb + (e.tap == model::Tap::PostFader ? source.volumeDb : 0);
        e.staticGain = db <= model::kSilenceDb ? 0.0 : console9::faderGain(db);
        e.staticPan = console9::nativePan(e.pan);
        const auto salt = std::rotl(source.id.n, 32) ^ ((e.route->n << 3) | 3);
        e.encoder.init(console9::seedsFor(s.project->seed, salt), p.linearConsole);
    }
    SUPPLE_TRY(checkStrips(p));
    return p;
}

// Activates instances, measures latencies, sizes buffers and delays.
Status prepare(Plan& p, const Session& s) {
    const std::size_t block = p.block;
    // One stage: activated at the plan's rate and block, buffers sized.
    auto prepareStage = [&](Stage& st) -> Status {
        if (st.instance) {
            host::Instance& in = *st.instance;
            in.serviceMainThread();
            const bool keyChanged = in.sidechainActive() != (st.keyed && in.sidechainChannels());
            if (in.active() && (in.sampleRate() != p.rate || in.maxFrames() < block ||
                                (keyChanged && !in.processing())))
                in.deactivate();
            if (!in.active()) {
                SUPPLE_TRY(in.setSidechainActive(st.keyed));
                SUPPLE_TRY(in.activate(p.rate, 1, p.block));
            }
            st.keyChannels = std::min<std::uint32_t>(in.sidechainChannels(), 2);
            st.latency = in.latency();
            st.instrument = in.audioInputChannels() == 0;
            if (in.audioOutputChannels() != 2)
                return Error("engine.layout", in.info().name + " does not have a stereo output");
        }
        st.dry.init(st.latency);
        st.out.init(block);
        st.dryBuf.init(block);
        st.key.init(st.keyed ? block : 0);
        st.outStorage.resize(256);
        st.outEvents.storage = st.outStorage;
        return {};
    };
    for (Node& n : p.nodes) {
        for (TakeChain& tc : n.takeChains) {
            tc.latency = 0;
            int stageIndex = 0;
            for (Stage& st : tc.chain) {
                SUPPLE_TRY(prepareStage(st));
                if (tc.noteTarget < 0 && st.instance && st.instance->acceptsNotes())
                    tc.noteTarget = stageIndex;
                tc.latency += st.latency;
                ++stageIndex;
            }
            tc.buf.init(block);
        }
        n.latChain = 0;
        int stageIndex = 0;
        for (Stage& st : n.chain) {
            if (st.instance) {
                host::Instance& in = *st.instance;
                in.serviceMainThread();
                // A key connected or not is part of the activation (a VST3's
                // auxiliary bus); an instance still processing keeps its own
                // (Engine::load replaces it, ADR-0018).
                const bool keyChanged =
                    in.sidechainActive() != (st.keyed && in.sidechainChannels());
                if (in.active() && (in.sampleRate() != p.rate || in.maxFrames() < block ||
                                    (keyChanged && !in.processing())))
                    in.deactivate();
                if (!in.active()) {
                    SUPPLE_TRY(in.setSidechainActive(st.keyed));
                    SUPPLE_TRY(in.activate(p.rate, 1, p.block));
                }
                st.keyChannels = std::min<std::uint32_t>(in.sidechainChannels(), 2);
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
            st.key.init(st.keyed ? block : 0);
            st.outStorage.resize(256);
            st.outEvents.storage = st.outStorage;
            st.chanDelay.clear();
            if (st.pinned || n.auxCount > 0) {
                st.chanDelay.resize(static_cast<std::size_t>(2 + n.auxCount));
                for (MonoDelay& d : st.chanDelay) d.init(st.latency);
                st.pinBuf.init(block);
            }
            n.latChain += st.latency;
            ++stageIndex;
        }
        n.aux.assign(static_cast<std::size_t>(n.auxCount), std::vector<float>(block, 0.0f));
        n.sum.init(block);
        n.preFx.init(block);
        n.preFader.init(block);
        n.post.init(block);
    }
    // Latency compensation, in topological order. A key lines up with its
    // stage's input: the node's input latency plus the stages before it.
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        Node& n = p.nodes[i];
        std::vector<std::int64_t> before(n.chain.size() + 1, 0);
        for (std::size_t k = 0; k < n.chain.size(); ++k)
            before[k + 1] = before[k] + n.chain[k].latency;
        auto at = [&](const Edge& ed) {
            return ed.stage < 0 ? std::int64_t{0} : before[static_cast<std::size_t>(ed.stage)];
        };
        // An auxiliary link reads a source's aux channel after its chain, a
        // main channel at the route's tap (ADR-0020).
        auto linkLatency = [&](const Edge& ed, const Edge::AuxLink& link) {
            return latTap(p.nodes[ed.from], link.from >= 2 ? model::Tap::PostFader : ed.tap);
        };
        n.latIn = 0;
        for (std::size_t e : n.in) {
            const Edge& ed = p.edges[e];
            if (!ed.auxOnly) n.latIn = std::max(n.latIn, latTap(p.nodes[ed.from], ed.tap) - at(ed));
            if (ed.stage < 0)
                for (const Edge::AuxLink& link : ed.auxLinks)
                    n.latIn = std::max(n.latIn, linkLatency(ed, link));
        }
        for (std::size_t e : n.in) {
            Edge& ed = p.edges[e];
            ed.delay.init(
                std::max<std::int64_t>(0, n.latIn + at(ed) - latTap(p.nodes[ed.from], ed.tap)));
            ed.tmp.init(block);
            if (ed.encode) ed.encoded.init(block);
            for (Edge::AuxLink& link : ed.auxLinks)
                link.delay.init(n.latIn - linkLatency(ed, link));
            ed.auxTmp.assign(ed.auxLinks.empty() ? 0 : block, 0.0f);
        }
    }
    const Node& master = p.nodes[p.mainMaster];
    p.outputLatency = master.latIn + master.latChain;
    // Automation lines up with the signal where it is applied: the fader sees
    // the track's input and chain latency, a route's gain its target's input.
    for (Node& n : p.nodes) {
        const std::int64_t at = n.latIn + n.latChain;
        for (Automation* a : {&n.autoVolume, &n.autoPan, &n.autoMute}) a->shift(at);
        for (std::size_t ei : n.in) {
            Edge& e = p.edges[ei];
            auto shift = e.encode ? latTap(p.nodes[e.from], e.tap) : n.latIn;
            if (e.stage >= 0)
                for (int k = 0; k < e.stage; ++k)
                    shift += n.chain[static_cast<std::size_t>(k)].latency;
            for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripVolume,
                                  &e.stripPan, &e.stripMute})
                a->shift(shift);
        }
        // A plugin's parameters: its input is the track's, delayed by the
        // stages before it.
        std::int64_t before = n.latIn;
        for (Stage& st : n.chain) {
            for (ParamLane& ln : st.lanes) ln.curve.shift(before);
            before += st.latency;
        }
        // A take chain's output lines up with the track's input; its stages
        // run its latency ahead of it.
        for (TakeChain& tc : n.takeChains) {
            for (Automation* a : {&tc.volume, &tc.pan, &tc.mute}) a->shift(n.latIn);
            std::int64_t ahead = n.latIn - tc.latency;
            for (Stage& st : tc.chain) {
                for (ParamLane& ln : st.lanes) ln.curve.shift(ahead);
                ahead += st.latency;
            }
        }
    }
    // Take chains: the item's span, gain, pan and fades (applied after the chain).
    for (Node& n : p.nodes)
        for (TakeChain& tc : n.takeChains) {
            const model::Item& it = *s.items.find(tc.item);
            const model::Take& take = *s.takes.find(tc.take);
            const model::ItemSpan span = model::itemSpan(s, it);
            const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
            tc.start = time::samplesAt(span.startSec, p.rate) - origin + n.latIn;
            tc.end = time::samplesAt(span.endSec, p.rate) - origin + n.latIn;
            const double g =
                dbToGain(it.gainDb) * dbToGain(take.gainDb) * (take.phaseInvert ? -1.0 : 1.0);
            const auto [pl, pr] =
                take.pan == 0.0 ? std::pair{1.0, 1.0} : panGains(take.pan / 100.0, -3.0);
            tc.gainL = g * pl;
            tc.gainR = g * pr;
            tc.fadeIn = time::samplesAt(it.fadeIn.length, p.rate);
            tc.fadeOut = time::samplesAt(it.fadeOut.length, p.rate);
            tc.fadeInShape = it.fadeIn.shape;
            tc.fadeOutShape = it.fadeOut.shape;
            tc.fadeInCurve = it.fadeIn.curve;
            tc.fadeOutCurve = it.fadeOut.curve;
        }
    // Audio items, like events shifted by the track's input latency.
    for (Node& n : p.nodes) {
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
            const model::ItemSpan span = model::itemSpan(s, it);
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
            const auto [pl, pr] =
                take->pan == 0.0 ? std::pair{1.0, 1.0} : panGains(take->pan / 100.0, -3.0);
            c.gainL = g * pl;
            c.gainR = g * pr;
            c.fadeIn = time::samplesAt(it.fadeIn.length, p.rate);
            c.fadeOut = time::samplesAt(it.fadeOut.length, p.rate);
            c.fadeInShape = it.fadeIn.shape;
            c.fadeOutShape = it.fadeOut.shape;
            c.fadeInCurve = it.fadeIn.curve;
            c.fadeOutCurve = it.fadeOut.curve;
            for (std::size_t k = 0; k < n.takeChains.size(); ++k) {
                if (n.takeChains[k].item != e.id) continue;
                // Through its take chain: the raw source; gain, pan and fades after it.
                c.takeChain = static_cast<int>(k);
                c.gainL = c.gainR = 1.0;
                c.fadeIn = c.fadeOut = 0;
                n.takeChains[k].clip = static_cast<int>(n.clips.size());
            }
            n.clips.push_back(std::move(c));
        }
    }
    // Events, shifted by each track's input latency so its own notes stay
    // aligned with what arrives from upstream.
    for (Node& n : p.nodes) {
        // A MIDI take whose chain holds an instrument plays it there, not on the track's.
        std::vector<Id> ownInstrument;
        for (TakeChain& tc : n.takeChains) {
            if (tc.noteTarget >= 0) {
                ownInstrument.push_back(tc.item);
                tc.events = trackEvents(s, n.id, p.startSeconds, p.rate,
                                        [&](Id item) { return item == tc.item; });
                for (auto& ev : tc.events)  // its input runs the chain's latency ahead
                    ev.sample = std::max<std::int64_t>(0, ev.sample + n.latIn - tc.latency);
            }
            // Reserve every block's events (processing never allocates): notes
            // and their releases for its instrument, parameter automation.
            for (std::size_t k = 0; k < tc.chain.size(); ++k) {
                Stage& st = tc.chain[k];
                std::size_t need = 1;
                if (static_cast<int>(k) == tc.noteTarget)
                    need += std::min<std::size_t>(tc.events.size(), 4 * block) + 16 * 128;
                for (const ParamLane& ln : st.lanes)
                    need += block / kAutomationStep + 2 + ln.curve.points.size();
                st.blockEvents.reserve(need);
            }
        }
        n.events = trackEvents(s, n.id, p.startSeconds, p.rate, [&](Id item) {
            return std::find(ownInstrument.begin(), ownInstrument.end(), item) ==
                   ownInstrument.end();
        });
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
        for (const TakeChain& tc : p.nodes[i].takeChains)
            for (const Stage& st : tc.chain)
                if (st.instance)
                    (p.nodes[i].live ? p.liveInstances : p.rendererInstances)
                        .push_back(st.instance);
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

Status checkStrips(const Plan& p) {
    for (std::size_t i = 0; i < p.nodes.size(); ++i) {
        const Node& n = p.nodes[i];
        if (n.decodeInput != (n.type != model::StripType::Channel) ||
            n.master != (n.type == model::StripType::Master))
            return Error("engine.strip_order", "implicit strip stages disagree with its type");
        for (std::size_t ei : n.in) {
            if (ei >= p.edges.size()) return Error("engine.strip_edge", "missing input edge");
            const Edge& e = p.edges[ei];
            if (e.from >= i)
                return Error("engine.strip_edge", "input is not topologically ordered");
            if (e.stage >= 0) {
                if (static_cast<std::size_t>(e.stage) >= n.chain.size() || e.encode)
                    return Error("engine.strip_edge", "key routes must bypass send encoders");
                continue;
            }
            if (e.auxOnly) {  // linear links into auxiliary channels (ADR-0020)
                if (e.encode) return Error("engine.strip_edge", "aux-only routes are not encoded");
                continue;
            }
            if (e.encode &&
                std::count(p.nodes[e.from].out.begin(), p.nodes[e.from].out.end(), ei) != 1)
                return Error("engine.strip_edge", "send encoder is not owned by its source");
            if (!n.decodeInput || p.nodes[e.from].master ||
                (!e.encode && (e.tap != model::Tap::PostFader || e.volumeDb != 0 || !e.followPan ||
                               e.mono || e.invert || e.mapped || e.muted || e.autoVolume.active() ||
                               e.autoPan.active() || e.autoMute.active())))
                return Error("engine.strip_edge",
                             "summing inputs require an unmodified Channel output");
        }
    }
    return okStatus();
}

const Stereo& edgeSignal(const Plan& p, const Edge& e) SUPPLE_RT {
    if (e.encode) return e.encoded;
    const Node& src = p.nodes[e.from];
    return e.tap == model::Tap::PreFx      ? src.preFx
           : e.tap == model::Tap::PreFader ? src.preFader
                                           : src.post;
}

namespace {
// Audio edges carry only the encoded signal and PDC; key controls are conventional.
void addEdge(Plan& p, Edge& e, double panLawDb, Stereo& into, std::int64_t pos,
             std::uint32_t n) SUPPLE_RT {
    const Stereo& tap = p.split && e.frontier ? e.frontierBuf : edgeSignal(p, e);
    e.tmp.copyFrom(tap, n);
    e.delay.process(e.tmp.l.data(), e.tmp.r.data(), n);
    for (std::uint32_t i = 0; i < n; ++i) {
        float l = e.tmp.l[i], r = e.tmp.r[i];
        if (e.stage >= 0) {
            const auto at = pos + i;
            const bool muted = e.autoMute.active() ? e.autoMute.at(at) >= .5 : e.muted;
            const double g =
                muted ? 0 : dbToGain(e.autoVolume.active() ? e.autoVolume.at(at) : e.volumeDb);
            const auto [pl, pr] = panGains((e.autoPan.active()                   ? e.autoPan.at(at)
                                            : e.followPan && e.stripPan.active() ? e.stripPan.at(at)
                                                                                 : e.pan) /
                                               100,
                                           panLawDb);
            if (e.mono) l = r = .5f * (l + r);
            const double sign = e.invert ? -1 : 1;
            l *= static_cast<float>(sign * g * pl);
            r *= static_cast<float>(sign * g * pr);
            e.applyMap(l, r);  // on the send's output, after its pan
        }
        into.l[i] += l;
        into.r[i] += r;
    }
}
}  // namespace

namespace {
// An audio item's samples for [pos, pos + n), added into `into`.
void playClip(const Clip& c, Stereo& into, std::int64_t pos, std::uint32_t n,
              std::int64_t shift) SUPPLE_RT {
    // Timeline sample t plays the clip at t + shift (a take chain's input
    // runs its latency ahead).
    const std::int64_t from = std::max<std::int64_t>(pos, c.start - shift);
    const std::int64_t to = std::min<std::int64_t>(pos + n, c.start + c.length - shift);
    const auto& ch = c.data->channels;
    const auto frames = static_cast<std::int64_t>(c.data->frames());
    if (ch.empty() || frames == 0) return;
    const std::size_t right = ch.size() > 1 ? 1 : 0;
    for (std::int64_t t = from; t < to; ++t) {
        const std::int64_t k = t + shift - c.start;  // sample within the item
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
            fade *= fadeCurve(c.fadeInShape, c.fadeInCurve,
                              static_cast<double>(k) / static_cast<double>(c.fadeIn));
        if (c.fadeOut > 0 && k >= c.length - c.fadeOut)
            fade *= fadeCurve(c.fadeOutShape, c.fadeOutCurve,
                              static_cast<double>(c.length - k) / static_cast<double>(c.fadeOut));
        const auto i = static_cast<std::size_t>(t - pos);
        into.l[i] += static_cast<float>(static_cast<double>(l) * c.gainL * fade);
        into.r[i] += static_cast<float>(static_cast<double>(r) * c.gainR * fade);
    }
}

// Plugin parameter automation: values at the block start, every
// kAutomationStep samples and at each point, in time order (the lane's
// cursor only moves forward), merged into the block's sorted events.
void automateStage(Stage& st, std::int64_t pos, std::uint32_t n) SUPPLE_RT {
    if (st.lanes.empty()) return;
    const std::size_t before = st.blockEvents.size();
    for (ParamLane& ln : st.lanes) {
        const auto& pts = ln.curve.points;
        auto pi =
            static_cast<std::size_t>(std::lower_bound(pts.begin(), pts.end(), pos,
                                                      [](const Automation::Pt& pt, std::int64_t x) {
                                                          return pt.sample < x;
                                                      }) -
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
            for (std::size_t j = i; j > 0 && st.blockEvents[j - 1].time > st.blockEvents[j].time;
                 --j)
                std::swap(st.blockEvents[j - 1], st.blockEvents[j]);
}

// One stage of a chain, in place on `sum` (the track's or a take's). False on
// a plugin error.
bool runStage(Plan& p, Stage& st, Stereo& sum, std::uint32_t n, const host::Transport& transport,
              std::int64_t pos, const void* self) SUPPLE_RT {
    // A stage whose plugin is not processing (start_processing failed)
    // passes audio through, delay-compensated, like a bypass.
    if (!st.instance || !st.instance->processing() || (self && st.instance->owner() != self)) {
        st.dry.process(sum.l.data(), sum.r.data(), n);  // compensated bypass
        st.appliedWet = -1.0f;
        st.wetRampLeft = 0;  // no ramp to carry: `appliedWet` holds nothing
        return true;
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
        st.dry.process(sum.l.data(), sum.r.data(), n);
        st.appliedWet = 0.0f;
        return true;
    }
    const float* in[2] = {sum.l.data(), sum.r.data()};
    float* out[2] = {st.out.l.data(), st.out.r.data()};
    host::ProcessBlock b;
    b.inputs = st.instrument ? nullptr : in;
    b.inputChannels = st.instrument ? 0 : 2;
    b.outputs = out;
    b.outputChannels = 2;
    const float* key[2] = {st.key.l.data(), st.key.r.data()};
    if (st.keyed) {
        b.sidechain = key;
        b.sidechainChannels = st.keyChannels;
    }
    b.frames = n;
    b.events = st.blockEvents;
    b.transport = &transport;
    b.steadyTime = pos;
    st.outEvents.clear();
    if (!st.instance->process(b, &st.outEvents)) return false;
    // Dry signal delayed by the stage's latency: an instrument adds its
    // output to what came in; an effect mixes wet and dry.
    st.dryBuf.copyFrom(sum, n);
    st.dry.process(st.dryBuf.l.data(), st.dryBuf.r.data(), n);
    if (st.wetRampLeft == 0) {
        const float wet = target, dryGain = st.instrument ? 1.0f : 1.0f - wet;
        for (std::uint32_t i = 0; i < n; ++i) {
            sum.l[i] = st.out.l[i] * wet + st.dryBuf.l[i] * dryGain;
            sum.r[i] = st.out.r[i] * wet + st.dryBuf.r[i] * dryGain;
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
            sum.l[i] = st.out.l[i] * wet + st.dryBuf.l[i] * dryGain;
            sum.r[i] = st.out.r[i] * wet + st.dryBuf.r[i] * dryGain;
            st.appliedWet = wet;
        }
    }
    return true;
}

// A stage of a strip with auxiliary channels, or with pins (ADR-0020): the
// plug-in reads its input pins and writes its output pins (its dry signal is
// its input's); the channels it does not write are delayed by its latency. A
// bypassed or idle pinned stage passes every channel through, delayed.
bool runPinned(Plan& p, Node& node, Stage& st, std::uint32_t n, const host::Transport& transport,
               std::int64_t pos, const void* self) SUPPLE_RT {
    const int channels = 2 + node.auxCount;
    if (!st.pinned) {  // the main pair as usual; the aux channels keep time
        for (int c = 2; c < channels; ++c)
            st.chanDelay[static_cast<std::size_t>(c)].process(node.channel(c), n);
        return runStage(p, st, node.sum, n, transport, pos, self);
    }
    if (!st.instance || !st.instance->processing() || (self && st.instance->owner() != self) ||
        st.bypass) {
        for (int c = 0; c < channels; ++c)
            st.chanDelay[static_cast<std::size_t>(c)].process(node.channel(c), n);
        st.appliedWet = st.bypass ? 0.0f : -1.0f;
        st.wetRampLeft = 0;
        return true;
    }
    std::copy_n(node.channel(st.inPin[0]), n, st.pinBuf.l.begin());
    std::copy_n(node.channel(st.inPin[1]), n, st.pinBuf.r.begin());
    if (!runStage(p, st, st.pinBuf, n, transport, pos, self)) return false;
    for (int c = 0; c < channels; ++c)
        if (c != st.outPin[0] && c != st.outPin[1])
            st.chanDelay[static_cast<std::size_t>(c)].process(node.channel(c), n);
    std::copy_n(st.pinBuf.l.begin(), n, node.channel(st.outPin[0]));
    if (st.outPin[1] >= 0) std::copy_n(st.pinBuf.r.begin(), n, node.channel(st.outPin[1]));
    return true;
}

// A take chain's block (gap log #22): its source (the clip, or its MIDI for
// its instrument) runs `latency` ahead through its stages; the output, cut to
// the item's span with the item and take gain, pan, fades and the take's
// envelopes, is added to the track's sum. Outside its span it does not run;
// it starts fresh (plugins reset) each time it is entered.
bool processTake(Plan& p, Node& node, TakeChain& tc, std::int64_t pos, std::uint32_t n,
                 const host::Transport& transport, const void* self) SUPPLE_RT {
    for (Stage& st : tc.chain) st.blockEvents.clear();
    if (!transport.playing || pos + n <= tc.start - tc.latency || pos >= tc.end) {
        tc.active = false;
        return true;
    }
    if (!tc.active) {
        tc.active = true;
        for (Stage& st : tc.chain) {
            st.dry.clear();
            if (st.instance && st.instance->processing() && (!self || st.instance->owner() == self))
                st.instance->reset();
        }
    }
    tc.buf.clear(n);
    if (tc.clip >= 0)
        playClip(node.clips[static_cast<std::size_t>(tc.clip)], tc.buf, pos, n, tc.latency);
    if (tc.noteTarget >= 0) {
        auto& target = tc.chain[static_cast<std::size_t>(tc.noteTarget)].blockEvents;
        if (tc.releaseHeld) {  // a loop wrap: what is held ends first
            tc.releaseHeld = false;
            for (std::size_t k = 0; k < tc.held.size(); ++k) {
                if (tc.held[k] == 0 || target.size() >= target.capacity()) continue;
                host::Event off;
                off.kind = host::Event::Kind::NoteOff;
                off.channel = static_cast<std::int16_t>(k / 128);
                off.key = static_cast<std::int16_t>(k % 128);
                off.velocity = 0.0;
                target.push_back(off);
            }
            tc.held.fill(0);
        }
        while (tc.nextEvent < tc.events.size() && tc.events[tc.nextEvent].sample < pos + n) {
            const TimedEvent& te = tc.events[tc.nextEvent++];
            host::Event e = te.event;
            e.time = static_cast<std::uint32_t>(std::max<std::int64_t>(0, te.sample - pos));
            if (target.size() < target.capacity()) target.push_back(e);  // never allocate here
            if (e.kind == host::Event::Kind::NoteOn || e.kind == host::Event::Kind::NoteOff) {
                const auto k = static_cast<std::size_t>((e.channel & 15) * 128 + (e.key & 127));
                if (e.kind == host::Event::Kind::NoteOn && tc.held[k] < 255)
                    ++tc.held[k];
                else if (e.kind == host::Event::Kind::NoteOff && tc.held[k] > 0)
                    --tc.held[k];
            }
        }
    }
    for (Stage& st : tc.chain) automateStage(st, pos, n);
    for (Stage& st : tc.chain)
        if (!runStage(p, st, tc.buf, n, transport, pos, self)) return false;
    const std::int64_t length = tc.end - tc.start;
    for (std::uint32_t i = 0; i < n; ++i) {
        const std::int64_t t = pos + i;
        if (t < tc.start || t >= tc.end) continue;
        const std::int64_t k = t - tc.start;
        double g = 1.0;
        if (tc.fadeIn > 0 && k < tc.fadeIn)
            g *= fadeCurve(tc.fadeInShape, tc.fadeInCurve,
                           static_cast<double>(k) / static_cast<double>(tc.fadeIn));
        if (tc.fadeOut > 0 && k >= length - tc.fadeOut)
            g *= fadeCurve(tc.fadeOutShape, tc.fadeOutCurve,
                           static_cast<double>(length - k) / static_cast<double>(tc.fadeOut));
        if (tc.volume.active()) g *= dbToGain(tc.volume.at(t));
        if (tc.mute.active() && tc.mute.at(t) >= .5) g = 0.0;
        double l = tc.gainL * g, r = tc.gainR * g;
        if (tc.pan.active()) {
            const auto [pl, pr] = panGains(tc.pan.at(t) / 100.0, -3.0);
            l *= pl;
            r *= pr;
        }
        node.sum.l[i] += static_cast<float>(static_cast<double>(tc.buf.l[i]) * l);
        node.sum.r[i] += static_cast<float>(static_cast<double>(tc.buf.r[i]) * r);
    }
    return true;
}

}  // namespace

// One block of every node, in order. Returns false on a plugin error.
bool processNode(Plan& p, std::size_t index, std::int64_t pos, std::uint32_t n,
                 const host::Transport& transport, const void* self) SUPPLE_RT {
    Node& node = p.nodes[index];
    // 1. Sum the inputs (fixed order), each through its compensation delay.
    node.sum.clear(n);
    for (std::size_t ei : node.in)
        if (p.edges[ei].stage < 0 && !p.edges[ei].auxOnly)
            addEdge(p, p.edges[ei], -3.0, node.sum, pos, n);
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
    if (node.decodeInput)
        for (std::uint32_t i = 0; i < n; ++i)
            node.decoder.sample(node.sum.l[i], node.sum.r[i], node.sum.l[i], node.sum.r[i]);
    // Auxiliary links (ADR-0020), linear, after the decode.
    for (auto& a : node.aux) std::fill_n(a.begin(), n, 0.0f);
    for (std::size_t ei : node.in) {
        Edge& e = p.edges[ei];
        if (e.stage >= 0 || e.auxLinks.empty() || (p.split && e.frontier)) continue;
        Node& src = p.nodes[e.from];
        const bool post = e.tap == model::Tap::PostFader;
        const double routeGain = e.muted ? 0.0 : dbToGain(e.volumeDb);
        for (Edge::AuxLink& link : e.auxLinks) {
            const float* from = nullptr;
            double g = routeGain * (e.invert ? -1.0 : 1.0);
            if (link.from >= 2) {
                from = src.aux[static_cast<std::size_t>(link.from - 2)].data();
            } else {
                const Stereo& tap = e.tap == model::Tap::PreFx ? src.preFx : src.preFader;
                from = link.from == 0 ? tap.l.data() : tap.r.data();
                if (post) g *= src.mute || src.soloSilenced ? 0.0 : dbToGain(src.volumeDb);
            }
            std::copy_n(from, n, e.auxTmp.begin());
            link.delay.process(e.auxTmp.data(), n);
            float* to = node.channel(link.to);
            const auto gain = static_cast<float>(g);
            for (std::uint32_t i = 0; i < n; ++i) to[i] += e.auxTmp[i] * gain;
        }
    }
    node.preFx.copyFrom(node.sum, n);
    // Audio items play after the pre-FX tap, before the chain (not when stopped).
    for (const Clip& c : node.clips)
        if (transport.playing && c.takeChain < 0) playClip(c, node.sum, pos, n, 0);
    // Takes with chains or envelopes: through their chains, into the sum.
    for (TakeChain& tc : node.takeChains)
        if (!processTake(p, node, tc, pos, n, transport, self)) return false;
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
    // 2b. Plugin parameter automation (automateStage).
    for (Stage& st : node.chain) automateStage(st, pos, n);
    // 3. The FX chain, in place on `sum`.
    for (Stage& st : node.chain) {
        // Its key first, every block (the key edges' delay lines and
        // automation keep time even while the stage is bypassed).
        if (st.keyed) {
            const int stageIndex = static_cast<int>(&st - node.chain.data());
            st.key.clear(n);
            for (std::size_t ei : node.in)
                if (p.edges[ei].stage == stageIndex) addEdge(p, p.edges[ei], -3.0, st.key, pos, n);
            // Key pins: track channels into the detector (a key on 3/4).
            if (!st.keyPins.empty()) {
                const float* kl = node.channel(st.keyPins[0]);
                const float* kr = node.channel(st.keyPins[st.keyPins.size() > 1 ? 1 : 0]);
                for (std::uint32_t i = 0; i < n; ++i) {
                    st.key.l[i] += kl[i];
                    st.key.r[i] += kr[i];
                }
            }
            if (st.keyChannels == 1)
                for (std::uint32_t i = 0; i < n; ++i)
                    st.key.l[i] = 0.5f * (st.key.l[i] + st.key.r[i]);
        }
        if (st.chanDelay.empty()) {
            if (!runStage(p, st, node.sum, n, transport, pos, self)) return false;
        } else if (!runPinned(p, node, st, n, transport, pos, self)) {
            return false;
        }
    }
    node.preFader.copyFrom(node.sum, n);
    // Metering (P5.3c): a strip's meter shows the signal before the Console 9
    // encode — after the fader and after the pan law (one multiply per channel
    // per sample, with the pan gains computed from libs/console9 when the pan
    // moves); a master's meter is its output, the signal the device is given.
    const bool metered = node.meterSlot != 0xffff;
    double meterPeakL = 0.0, meterPeakR = 0.0, meterSumL = 0.0, meterSumR = 0.0;
    double meterLeft = 0.0, meterRight = 0.0, meterPan = -1.0;
    // Fader controls and mute act before the channel encode; masters balance decoded audio.
    for (std::uint32_t i = 0; i < n; ++i) {
        const auto at = pos + i;
        const double db = node.autoVolume.active() ? node.autoVolume.at(at) : node.volumeDb;
        const double pan = node.autoPan.active() ? node.autoPan.at(at) : node.pan;
        const bool muted =
            node.soloSilenced || (node.autoMute.active() ? node.autoMute.at(at) >= .5 : node.mute);
        const double g = node.autoVolume.active()
                             ? (node.master               ? dbToGain(db)
                                : db <= model::kSilenceDb ? 0.0
                                                          : console9::faderGain(db))
                             : node.staticGain;
        node.fader.set(g, node.autoPan.active() ? console9::nativePan(pan) : node.staticPan, muted,
                       node.autoVolume.active() || node.autoPan.active(), p.smoothFrames);
        const double gain = node.fader.gain.next() * node.fader.gate.next();
        const double nativePan = node.fader.pan.next();
        if (node.master) {
            const double balance = nativePan * 2 - 1;
            const double l = balance <= 0 ? 1 : std::cos(balance * std::numbers::pi / 2);
            const double r = balance >= 0 ? 1 : std::cos(balance * std::numbers::pi / 2);
            node.post.l[i] = node.sum.l[i] * static_cast<float>(gain * l);
            node.post.r[i] = node.sum.r[i] * static_cast<float>(gain * r);
            if (metered) {
                meterPeakL = std::max(meterPeakL, std::fabs(double(node.post.l[i])));
                meterPeakR = std::max(meterPeakR, std::fabs(double(node.post.r[i])));
                meterSumL += double(node.post.l[i]) * double(node.post.l[i]);
                meterSumR += double(node.post.r[i]) * double(node.post.r[i]);
            }
        } else {
            node.encoder.sample(node.sum.l[i], node.sum.r[i], node.post.l[i], node.post.r[i], gain,
                                nativePan);
            if (metered) {
                if (nativePan != meterPan) {
                    console9::panGains(console9::panAngle(nativePan), meterLeft, meterRight);
                    meterPan = nativePan;
                }
                const double l = double(node.sum.l[i]) * gain * meterLeft;
                const double r = double(node.sum.r[i]) * gain * meterRight;
                meterPeakL = std::max(meterPeakL, std::fabs(l));
                meterPeakR = std::max(meterPeakR, std::fabs(r));
                meterSumL += l * l;
                meterSumR += r * r;
            }
        }
    }
    if (metered) {
        node.meterPeak[0] = static_cast<float>(meterPeakL);
        node.meterPeak[1] = static_cast<float>(meterPeakR);
        node.meterSumSq[0] = static_cast<float>(meterSumL);
        node.meterSumSq[1] = static_cast<float>(meterSumR);
    }
    // Independent encoders run at the source, before PDC and frontier publication.
    for (std::size_t ei : node.out) {
        Edge& e = p.edges[ei];
        const Stereo& input = e.tap == model::Tap::PreFx ? node.preFx : node.preFader;
        for (std::uint32_t i = 0; i < n; ++i) {
            const auto at = pos + i;
            const bool post = e.tap == model::Tap::PostFader;
            const double sourceDb = e.stripVolume.active() ? e.stripVolume.at(at) : node.volumeDb;
            const double routeDb = e.autoVolume.active() ? e.autoVolume.at(at) : e.volumeDb;
            const double sourcePan = e.stripPan.active() ? e.stripPan.at(at) : node.pan;
            const double pan = e.autoPan.active() ? e.autoPan.at(at)
                               : e.followPan      ? sourcePan
                                                  : e.pan;
            const bool muted =
                node.soloSilenced ||
                (post && (e.stripMute.active() ? e.stripMute.at(at) >= .5 : node.mute)) ||
                (e.autoMute.active() ? e.autoMute.at(at) >= .5 : e.muted);
            // -inf plus a send boost remains -inf.
            const double db =
                (routeDb <= model::kSilenceDb || (post && sourceDb <= model::kSilenceDb))
                    ? model::kSilenceDb
                    : routeDb + (post ? sourceDb : 0);
            const double g = e.autoVolume.active() || (post && e.stripVolume.active())
                                 ? db <= model::kSilenceDb ? 0.0 : console9::faderGain(db)
                                 : e.staticGain;
            const double nativePan = e.autoPan.active() || (e.followPan && e.stripPan.active())
                                         ? console9::nativePan(pan)
                                         : e.staticPan;
            e.fader.set(g, nativePan, muted,
                        e.autoVolume.active() || e.autoPan.active() ||
                            (post && e.stripVolume.active()) ||
                            (e.followPan && e.stripPan.active()),
                        p.smoothFrames);
            float l = input.l[i], r = input.r[i];
            if (e.mono) l = r = .5f * (l + r);
            if (e.invert) {
                l = -l;
                r = -r;
            }
            e.encoder.sample(l, r, e.encoded.l[i], e.encoded.r[i],
                             e.fader.gain.next() * e.fader.gate.next(), e.fader.pan.next());
            // The channel map acts on the send's output (after its pan): the
            // encode is per channel, so moving encoded channels is exact.
            e.applyMap(e.encoded.l[i], e.encoded.r[i]);
        }
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

namespace {
// Take chains (gap log #22) after a seek (`wrap` false: fresh at the next
// entry) or a loop wrap (held notes end; delay lines and tails kept).
void locateTakes(Node& n, std::int64_t pos, bool wrap, bool keepPlaying = false) SUPPLE_RT {
    for (TakeChain& tc : n.takeChains) {
        const auto it =
            std::lower_bound(tc.events.begin(), tc.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        tc.nextEvent = static_cast<std::size_t>(it - tc.events.begin());
        for (Automation* a : {&tc.volume, &tc.pan, &tc.mute}) a->cursor = 0;
        for (Stage& st : tc.chain) {
            if (!wrap && !st.preserveDryOnLocate) st.dry.clear();
            st.preserveDryOnLocate = false;
            for (ParamLane& ln : st.lanes) {
                ln.curve.cursor = 0;
                ln.lastSent = std::numeric_limits<double>::quiet_NaN();
            }
        }
        if (wrap) {
            tc.releaseHeld = true;
        } else if (!keepPlaying) {  // a seek: fresh at the next entry
            tc.held.fill(0);
            tc.active = false;
        }
    }
}
}  // namespace

void locateClass(Plan& p, std::int64_t pos, bool live, bool restartConsole) SUPPLE_RT {
    for (Node& n : p.nodes) {
        if (n.live != live) continue;
        locateTakes(n, pos, false, !restartConsole);
        auto it =
            std::lower_bound(n.events.begin(), n.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        n.nextEvent = static_cast<std::size_t>(it - n.events.begin());
        if (restartConsole) {
            n.encoder.reset();
            n.decoder.reset();
            n.fader.reset();
        }
        for (std::size_t ei : n.out) {
            Edge& e = p.edges[ei];
            if (restartConsole) {
                e.encoder.reset();
                e.fader.reset();
            }
            for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripVolume,
                                  &e.stripPan, &e.stripMute})
                a->cursor = 0;
        }
        if (!n.releaseHeld) n.held.fill(0);
        for (Automation* a : {&n.autoVolume, &n.autoPan, &n.autoMute}) a->cursor = 0;
        for (Stage& st : n.chain) {
            if (!st.preserveDryOnLocate) {
                st.dry.clear();
                for (MonoDelay& d : st.chanDelay) d.clear();
            }
            st.preserveDryOnLocate = false;
            for (ParamLane& ln : st.lanes) {
                ln.curve.cursor = 0;
                ln.lastSent = std::numeric_limits<double>::quiet_NaN();
            }
        }
        for (std::size_t ei : n.in) {
            Edge& e = p.edges[ei];
            e.delay.clear();
            for (Edge::AuxLink& link : e.auxLinks) link.delay.clear();
            if (!e.encode)
                for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripPan})
                    a->cursor = 0;
        }
    }
}

bool processBlockOn(Workers& workers, Plan& p, std::int64_t pos, std::uint32_t n) {
    const host::Transport transport = transportAt(p, pos);
    BlockRun run{&p, pos, n, &transport};
    workers.run(*p.graph, &runNode, &run);
    return run.ok.load(std::memory_order_relaxed);
}

void locate(Plan& p, std::int64_t pos, bool restartConsole) SUPPLE_RT {
    for (Node& n : p.nodes) {
        locateTakes(n, pos, false, !restartConsole);  // adoption keeps takes playing
        auto it =
            std::lower_bound(n.events.begin(), n.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        n.nextEvent = static_cast<std::size_t>(it - n.events.begin());
        if (restartConsole) {
            n.encoder.reset();
            n.decoder.reset();
            n.fader.reset();
        }
        for (std::size_t ei : n.out) {
            Edge& e = p.edges[ei];
            if (restartConsole) {
                e.encoder.reset();
                e.fader.reset();
            }
            for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripVolume,
                                  &e.stripPan, &e.stripMute})
                a->cursor = 0;
        }
        if (!n.releaseHeld) n.held.fill(0);
        for (Automation* a : {&n.autoVolume, &n.autoPan, &n.autoMute}) a->cursor = 0;
        for (Stage& st : n.chain) {
            if (!st.preserveDryOnLocate) {
                st.dry.clear();
                for (MonoDelay& d : st.chanDelay) d.clear();
            }
            st.preserveDryOnLocate = false;
            for (ParamLane& ln : st.lanes) {
                ln.curve.cursor = 0;
                ln.lastSent = std::numeric_limits<double>::quiet_NaN();  // resend at once
            }
        }
    }
    for (Edge& e : p.edges) {
        e.delay.clear();
        for (Edge::AuxLink& link : e.auxLinks) link.delay.clear();
        for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripPan}) a->cursor = 0;
    }
}

void wrapTo(Plan& p, std::int64_t pos, int liveClass) SUPPLE_RT {
    for (Node& n : p.nodes) {
        if (liveClass >= 0 && n.live != (liveClass == 1)) continue;
        locateTakes(n, pos, true);
        auto it =
            std::lower_bound(n.events.begin(), n.events.end(), pos,
                             [](const TimedEvent& e, std::int64_t x) { return e.sample < x; });
        n.nextEvent = static_cast<std::size_t>(it - n.events.begin());
        n.encoder.reset();
        n.decoder.reset();
        for (std::size_t ei : n.out) {
            Edge& e = p.edges[ei];
            e.encoder.reset();
            for (Automation* a : {&e.autoVolume, &e.autoPan, &e.autoMute, &e.stripVolume,
                                  &e.stripPan, &e.stripMute})
                a->cursor = 0;
        }
        n.releaseHeld = true;
        for (Automation* a : {&n.autoVolume, &n.autoPan, &n.autoMute}) a->cursor = 0;
        for (Stage& st : n.chain)
            for (ParamLane& ln : st.lanes) {
                ln.curve.cursor = 0;
                ln.lastSent = std::numeric_limits<double>::quiet_NaN();  // the loop start's value
            }
        for (std::size_t ei : n.in)
            if (!p.edges[ei].encode)
                for (Automation* a : {&p.edges[ei].autoVolume, &p.edges[ei].autoPan,
                                      &p.edges[ei].autoMute, &p.edges[ei].stripPan})
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
            if (n.type == old.type) {
                n.fader = old.fader;
                n.encoder.carry(old.encoder, to.smoothFrames);
                n.decoder.carry(old.decoder, to.smoothFrames);
                for (std::size_t ei : n.out)
                    for (std::size_t oi : old.out) {
                        Edge& e = to.edges[ei];
                        const Edge& oe = from.edges[oi];
                        if (e.route == oe.route && e.tap == oe.tap) {
                            e.fader = oe.fader;
                            e.encoder.carry(oe.encoder, to.smoothFrames);
                            break;
                        }
                    }
            }
            auto carryStages = [](std::vector<Stage>& chain, std::vector<Stage>& oldChain) {
                for (Stage& st : chain)
                    for (Stage& os : oldChain)
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
            };
            carryStages(n.chain, old.chain);
            // A take chain playing on keeps playing (no reset at the swap).
            for (TakeChain& tc : n.takeChains)
                for (TakeChain& otc : old.takeChains)
                    if (otc.take == tc.take) {
                        tc.active = otc.active;
                        tc.held = otc.held;
                        carryStages(tc.chain, otc.chain);
                        break;
                    }
            break;
        }
    }
}

}  // namespace supple::engine::detail
