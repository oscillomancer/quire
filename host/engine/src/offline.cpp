#include "supple/engine/offline.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>
#include <optional>
#include <set>
#include <thread>

#include "supple/clap/semantic_params.h"
#include "supple/time/timeline.h"

namespace supple::engine {

using model::Id;
using model::Kind;
using model::Session;

// --- instance pool ------------------------------------------------------------------

InstancePool::InstancePool(Resolver resolve, host::HostIdentity identity)
    : resolve_(std::move(resolve)), identity_(std::move(identity)) {}

InstancePool::~InstancePool() = default;

Result<host::Instance*> InstancePool::get(Id id, const model::FxInstance& fx,
                                          const Session& session) {
    auto& e = instances_[id];
    if (!e.instance || e.pluginId != fx.plugin.id) {
        auto info = resolve_(fx.plugin);
        if (!info) {
            instances_.erase(id);
            return std::move(info).error();
        }
        auto made = host::Instance::create(*info, identity_);
        if (!made) {
            instances_.erase(id);
            return std::move(made).error();
        }
        e.instance = std::move(*made);
        e.pluginId = fx.plugin.id;
        e.appliedState.clear();
    }
    std::string desired;
    if (fx.patch)
        desired = "patch:" + json::writeCompact(*fx.patch);
    else if (fx.state)
        desired = "blob:" + *fx.state;
    if (desired != e.appliedState && !desired.empty()) {
        if (fx.patch) {
            SUPPLE_TRY(
                e.instance->setPatch(json::writeCompact(*fx.patch), CLAP_SUPPLE_SET_REPLACE));
        } else {
            auto it = session.blobs.find(*fx.state);
            if (it == session.blobs.end())
                return Error("engine.blob_missing", id.str() + " refers to a missing state blob",
                             "/fx/" + id.str() + "/state");
            SUPPLE_TRY(e.instance->loadState(it->second->data));
        }
        e.appliedState = desired;
    }
    return e.instance.get();
}

void InstancePool::prune(const Session& session) {
    for (auto it = instances_.begin(); it != instances_.end();)
        it = session.fx.contains(it->first) ? std::next(it) : instances_.erase(it);
}

// --- media -----------------------------------------------------------------------------

Result<std::shared_ptr<const audio::AudioData>> MediaCache::get(const std::filesystem::path& path) {
    const std::string key = path.lexically_normal().string();
    auto it = entries_.find(key);
    if (it != entries_.end()) return it->second;
    auto data = audio::readAudio(path);
    if (!data) return std::move(data).error();
    auto shared = std::make_shared<const audio::AudioData>(std::move(*data));
    entries_.emplace(key, shared);
    return shared;
}

// --- mixing helpers ---------------------------------------------------------------------

std::pair<double, double> panGains(double pan, double panLawDb) {
    const double p = std::clamp(pan, -1.0, 1.0);
    const double angle = (p + 1.0) * std::numbers::pi / 4.0;
    // The sin/cos curve is constant power: 1/sqrt(2) at the centre, which is
    // what "-3 dB" means. Other laws scale it to their centre gain.
    const double centre =
        panLawDb == -3.0 ? std::numbers::sqrt2 / 2.0 : std::pow(10.0, panLawDb / 20.0);
    const double scale = centre * std::numbers::sqrt2;
    return {std::cos(angle) * scale, std::sin(angle) * scale};
}

namespace {

double dbToGain(double db) { return db <= model::kSilenceDb ? 0.0 : std::pow(10.0, db / 20.0); }

// A fixed-length delay for one stereo signal (allocated at compile time).
struct Delay {
    std::int64_t length = 0;
    std::vector<float> buf[2];
    std::size_t pos = 0;
    void init(std::int64_t n) {
        length = n;
        for (auto& b : buf) b.assign(static_cast<std::size_t>(std::max<std::int64_t>(n, 1)), 0.0f);
        pos = 0;
    }
    // In place on `frames` samples of L/R.
    void process(float* l, float* r, std::uint32_t frames) SUPPLE_RT {
        if (length == 0) return;
        const auto n = static_cast<std::size_t>(length);
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float dl = buf[0][pos], dr = buf[1][pos];
            buf[0][pos] = l[i];
            buf[1][pos] = r[i];
            l[i] = dl;
            r[i] = dr;
            pos = pos + 1 == n ? 0 : pos + 1;
        }
    }
};

struct Stereo {
    std::vector<float> l, r;
    void init(std::size_t n) {
        l.assign(n, 0.0f);
        r.assign(n, 0.0f);
    }
    void clear(std::uint32_t n) SUPPLE_RT {
        std::fill_n(l.begin(), n, 0.0f);
        std::fill_n(r.begin(), n, 0.0f);
    }
    void copyFrom(const Stereo& o, std::uint32_t n) SUPPLE_RT {
        std::copy_n(o.l.begin(), n, l.begin());
        std::copy_n(o.r.begin(), n, r.begin());
    }
};

double shapeCurve(model::PointShape shape, double tension, double t) {
    switch (shape) {
        case model::PointShape::Linear:
            return t;
        case model::PointShape::Square:
            return 0.0;  // holds until the next point
        case model::PointShape::Slow:
            return t * t * (3.0 - 2.0 * t);
        case model::PointShape::FastStart:
            return 1.0 - (1.0 - t) * (1.0 - t);
        case model::PointShape::FastEnd:
            return t * t;
        case model::PointShape::Bezier:
            return t + std::clamp(tension, -1.0, 1.0) * t * (1.0 - t);
    }
    return t;
}

// A compiled envelope: points at samples from the render start (already
// shifted by the latency at the point of application), values in the
// target's unit. Playback is monotonic, so a cursor makes lookups O(1).
struct Automation {
    struct Pt {
        std::int64_t sample;
        double value;
        model::PointShape shape;
        double tension;
    };
    std::vector<Pt> points;
    std::size_t cursor = 0;
    bool active() const { return !points.empty(); }
    void shift(std::int64_t by) {
        for (auto& pt : points) pt.sample += by;
    }
    double at(std::int64_t s) SUPPLE_RT {
        if (s <= points.front().sample) return points.front().value;
        while (cursor + 1 < points.size() && points[cursor + 1].sample <= s) ++cursor;
        if (cursor + 1 >= points.size()) return points.back().value;
        const Pt& a = points[cursor];
        const Pt& b = points[cursor + 1];
        const double t =
            static_cast<double>(s - a.sample) / static_cast<double>(b.sample - a.sample);
        return a.value + (b.value - a.value) * shapeCurve(a.shape, a.tension, t);
    }
};

struct Edge {
    std::size_t from = 0;     // node index
    std::optional<Id> route;  // empty for parent sends
    model::Tap tap = model::Tap::PostFader;
    double gainL = 1.0, gainR = 1.0;
    double volumeDb = 0.0, pan = 0.0;
    bool mono = false, invert = false;
    Automation autoVolume, autoPan, autoMute;  // route envelopes (dB, %, 0/1)
    Delay delay;
    Stereo tmp;
};

struct Stage {
    host::Instance* instance = nullptr;  // null: placeholder (pass-through)
    bool bypass = false, instrument = false;
    double wet = 1.0;
    std::int64_t latency = 0;
    Delay dry;  // bypassed or partly-dry signal, delayed to match the stage
    Stereo out, dryBuf;
    std::vector<host::Event> blockEvents;
    std::vector<host::OutputEvent> outStorage;
    host::OutputEvents outEvents;
};

// An audio item ready to play: which source frames land on which samples.
struct Clip {
    std::shared_ptr<const audio::AudioData> data;
    std::int64_t start = 0, length = 0;  // samples on the track's timeline (PDC-shifted)
    std::int64_t offset = 0;             // source frame at the item start
    bool loop = false;
    model::ChannelMode mode = model::ChannelMode::Normal;
    double gainL = 1.0, gainR = 1.0;
    std::int64_t fadeIn = 0, fadeOut = 0;
    model::FadeShape fadeInShape = model::FadeShape::Linear,
                     fadeOutShape = model::FadeShape::Linear;
};

struct TimedEvent {
    std::int64_t sample;
    host::Event event;
};

struct Node {
    Id id;
    std::vector<std::size_t> in;  // incoming edges, in summation order
    std::vector<Stage> chain;
    std::int64_t latIn = 0, latChain = 0;
    Stereo sum, preFx, preFader, post;
    std::vector<TimedEvent> events;
    std::size_t nextEvent = 0;
    std::vector<Clip> clips;
    int noteTarget = -1;  // chain index receiving note events
    double volume = 1.0, width = 1.0, panL = 1.0, panR = 1.0;
    double volumeDb = 0.0, pan = 0.0, panLawDb = -3.0;
    bool mute = false, master = false;
    bool soloSilenced = false;  // another track is soloed and this one is not on its path
    Automation autoVolume, autoTrim, autoPan, autoWidth, autoMute;  // dB, dB, %, %, 0/1
    bool automated() const {
        return autoVolume.active() || autoTrim.active() || autoPan.active() || autoWidth.active() ||
               autoMute.active();
    }
};

struct Plan {
    std::vector<Node> nodes;  // topological order; the master is last
    std::vector<Edge> edges;
    double rate = 48000.0;
    double startSeconds = 0.0;
    std::int64_t outputLatency = 0;
    std::uint32_t block = 512;
    const time::TimeMap* timeMap = nullptr;
    std::vector<std::string> warnings;
    std::filesystem::path projectDir;
    MediaCache* media = nullptr;
};

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
                if (!on || *on >= endSample) continue;
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

Result<Plan> compile(const Session& s, InstancePool& pool, const OfflineSettings& opt) {
    Plan p;
    p.rate = s.project->sampleRate;
    p.block = opt.blockSize;
    p.startSeconds = opt.startSeconds;
    p.timeMap = s.timeMap.get();
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
    // Envelopes on mixer targets (plugin parameters come later).
    const std::int64_t origin = time::samplesAt(opt.startSeconds, p.rate);
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
        }
        if (slot)
            *slot = std::move(a);
        else
            p.warnings.push_back(e.id.str() + ": automation of " + o.str() + " " + param +
                                 " is not rendered yet");
    }
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
            auto data = p.media->get(path);
            if (!data) {
                p.warnings.push_back(e.id.str() + ": " + data.error().message + " (media " +
                                     take->source->str() + " is offline)");
                continue;
            }
            if ((*data)->sampleRate != p.rate) {
                p.warnings.push_back(e.id.str() + ": " + take->source->str() + " is at " +
                                     std::to_string(static_cast<int>((*data)->sampleRate)) +
                                     " Hz; resampling is not available yet, the item is silent");
                continue;
            }
            if (take->playrate != 1.0)
                p.warnings.push_back(e.id.str() + ": playrate " + std::to_string(take->playrate) +
                                     " is rendered at 1.0 (time stretching is not available yet)");
            const ItemSpan span = itemSpan(s, it, tr);
            const std::int64_t origin = time::samplesAt(p.startSeconds, p.rate);
            Clip c;
            c.data = *data;
            c.start = time::samplesAt(span.startSec, p.rate) - origin + n.latIn;
            c.length =
                time::samplesAt(span.endSec, p.rate) - time::samplesAt(span.startSec, p.rate);
            c.offset = time::samplesAt(take->startOffset, p.rate);
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
        std::size_t perBlockMax = 0;
        // Reserve the largest per-block count (events are sorted).
        for (std::size_t a = 0, b = 0; a < n.events.size(); a = b) {
            const std::int64_t blk = n.events[a].sample / static_cast<std::int64_t>(block);
            b = a;
            while (b < n.events.size() &&
                   n.events[b].sample / static_cast<std::int64_t>(block) == blk)
                ++b;
            perBlockMax = std::max(perBlockMax, b - a);
        }
        for (Stage& st : n.chain) st.blockEvents.reserve(perBlockMax + 1);
    }
    return okStatus();
}

host::Transport transportAt(const Plan& p, std::int64_t sample) {
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
    return t;
}

// One block of every node, in order. Returns false on a plugin error.
bool processBlock(Plan& p, std::int64_t pos, std::uint32_t n) SUPPLE_RT {
    const host::Transport transport = transportAt(p, pos);
    for (Node& node : p.nodes) {
        // 1. Sum the inputs (fixed order), each through its compensation delay.
        node.sum.clear(n);
        for (std::size_t ei : node.in) {
            Edge& e = p.edges[ei];
            const Node& src = p.nodes[e.from];
            const Stereo& tap = e.tap == model::Tap::PreFx      ? src.preFx
                                : e.tap == model::Tap::PreFader ? src.preFader
                                                                : src.post;
            e.tmp.copyFrom(tap, n);
            e.delay.process(e.tmp.l.data(), e.tmp.r.data(), n);
            const bool automated =
                e.autoVolume.active() || e.autoPan.active() || e.autoMute.active();
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
        node.preFx.copyFrom(node.sum, n);
        // Audio items play after the pre-FX tap, before the chain.
        for (const Clip& c : node.clips) {
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
        // 2. Route this block's events to the note target.
        for (Stage& st : node.chain) st.blockEvents.clear();
        while (node.nextEvent < node.events.size() &&
               node.events[node.nextEvent].sample < pos + n) {
            const TimedEvent& te = node.events[node.nextEvent++];
            if (node.noteTarget < 0) continue;
            host::Event e = te.event;
            e.time = static_cast<std::uint32_t>(std::max<std::int64_t>(0, te.sample - pos));
            node.chain[static_cast<std::size_t>(node.noteTarget)].blockEvents.push_back(e);
        }
        // 3. The FX chain, in place on `sum`.
        for (Stage& st : node.chain) {
            if (st.bypass || !st.instance) {
                st.dry.process(node.sum.l.data(), node.sum.r.data(), n);  // compensated bypass
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
            const float wet = static_cast<float>(st.wet),
                        dryGain = st.instrument ? 1.0f : 1.0f - wet;
            for (std::uint32_t i = 0; i < n; ++i) {
                node.sum.l[i] =
                    st.out.l[i] * (st.instrument ? 1.0f : wet) + st.dryBuf.l[i] * dryGain;
                node.sum.r[i] =
                    st.out.r[i] * (st.instrument ? 1.0f : wet) + st.dryBuf.r[i] * dryGain;
            }
        }
        node.preFader.copyFrom(node.sum, n);
        // 4. Width, volume, pan, mute.
        float w = static_cast<float>(node.width);
        float gl = node.mute ? 0.0f : static_cast<float>(node.volume * node.panL);
        float gr = node.mute ? 0.0f : static_cast<float>(node.volume * node.panR);
        const bool automated = node.automated();
        for (std::uint32_t i = 0; i < n; ++i) {
            if (automated) {
                const std::int64_t at = pos + i;
                const bool muted = node.autoMute.active() ? node.autoMute.at(at) >= 0.5 : node.mute;
                const double db =
                    (node.autoVolume.active() ? node.autoVolume.at(at) : node.volumeDb) +
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
                const double g = muted ? 0.0 : dbToGain(db);
                gl = static_cast<float>(g * pl);
                gr = static_cast<float>(g * pr);
                if (node.autoWidth.active()) w = static_cast<float>(node.autoWidth.at(at) / 100.0);
            }
            const float mid = 0.5f * (node.sum.l[i] + node.sum.r[i]);
            const float side = 0.5f * (node.sum.l[i] - node.sum.r[i]) * w;
            node.post.l[i] = (mid + side) * gl;
            node.post.r[i] = (mid - side) * gr;
        }
        if (node.soloSilenced) {  // processed (plugin state continues) but not heard
            node.preFx.clear(n);
            node.preFader.clear(n);
            node.post.clear(n);
        }
    }
    return true;
}

}  // namespace

Result<OfflineResult> renderOffline(const Session& s, InstancePool& pool,
                                    const OfflineSettings& opt) {
    if (!(opt.endSeconds > opt.startSeconds))
        return Error("engine.range", "the render range is empty", {}, "end must be after start");
    if (opt.blockSize < 1 || opt.blockSize > 8192)
        return Error("engine.block_size", "block size out of range", {}, "1 .. 8192");
    if (!(opt.tailSeconds >= 0 && opt.tailSeconds <= 600))
        return Error("engine.tail", "tail out of range", {}, "0 .. 600 s");
    auto compiled = compile(s, pool, opt);
    if (!compiled) return std::move(compiled).error();
    Plan& p = *compiled;
    MediaCache localMedia;
    p.projectDir = opt.projectDir;
    p.media = opt.media ? opt.media : &localMedia;
    SUPPLE_TRY(prepare(p, s));
    const std::int64_t length = time::samplesAt(opt.endSeconds, p.rate) -
                                time::samplesAt(opt.startSeconds, p.rate) +
                                time::samplesAt(opt.tailSeconds, p.rate);
    const std::int64_t total = length + p.outputLatency;
    if (total > (std::int64_t(1) << 28)) return Error("engine.too_long", "render too long");
    std::vector<float> outL(static_cast<std::size_t>(total)), outR(static_cast<std::size_t>(total));
    bool failed = false, startFailed = false;

    std::vector<host::Instance*> instances;
    for (Node& n : p.nodes)
        for (Stage& st : n.chain)
            if (st.instance) instances.push_back(st.instance);
    std::thread audio([&] {
        for (host::Instance* in : instances) {
            in->reset();
            in->flushParams();
            if (!in->startProcessing()) startFailed = true;
        }
        if (!startFailed) {
            for (std::int64_t pos = 0; pos < total; pos += p.block) {
                const auto n =
                    static_cast<std::uint32_t>(std::min<std::int64_t>(p.block, total - pos));
                if (!processBlock(p, pos, n)) {
                    failed = true;
                    break;
                }
                const Node& master = p.nodes.back();
                std::copy_n(master.post.l.begin(), n, outL.begin() + pos);
                std::copy_n(master.post.r.begin(), n, outR.begin() + pos);
            }
        }
        for (host::Instance* in : instances) in->stopProcessing();
    });
    audio.join();

    OfflineResult r;
    for (host::Instance* in : instances) {
        in->serviceMainThread();
        for (auto& line : in->takeLog()) r.log.push_back(in->info().name + ": " + line);
    }
    if (startFailed) return Error("engine.start_failed", "a plugin refused to start processing");
    if (failed) return Error("engine.process_error", "a plugin reported a processing error");
    for (const Node& n : p.nodes) {
        std::string line = n.id.str() + " in=[";
        for (std::size_t k = 0; k < n.in.size(); ++k) {
            const Edge& e = p.edges[n.in[k]];
            static constexpr const char* kTaps[] = {"post_fader", "pre_fader", "pre_fx"};
            char g[64];
            std::snprintf(g, sizeof g, " d%lld g%.3f/%.3f", static_cast<long long>(e.delay.length),
                          e.gainL, e.gainR);
            line += (k ? ", " : "") + p.nodes[e.from].id.str() + " " +
                    kTaps[static_cast<int>(e.tap)] + g;
        }
        line += "] lat_in=" + std::to_string(n.latIn) + " lat_chain=" + std::to_string(n.latChain);
        r.graph.push_back(std::move(line));
    }
    r.outputLatency = static_cast<std::uint32_t>(p.outputLatency);
    r.warnings = std::move(p.warnings);
    r.audio.sampleRate = p.rate;
    const auto skip = static_cast<std::ptrdiff_t>(p.outputLatency);
    r.audio.channels = {std::vector<float>(outL.begin() + skip, outL.end()),
                        std::vector<float>(outR.begin() + skip, outR.end())};
    return r;
}

}  // namespace supple::engine
