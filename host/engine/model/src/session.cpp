#include "supple/model/session.h"

#include <algorithm>
#include <charconv>
#include <functional>
#include <set>

#include "supple/model/schemas.h"

namespace supple::model {

// --- ids -------------------------------------------------------------------------------

namespace {
constexpr std::string_view kPrefixes[kKindCount] = {"tr", "it", "tk", "md", "mi", "fx", "sd", "en",
                                                    "ai", "mk", "rg", "tp", "ts", "gr", "ln", "rj"};
}

std::string_view prefix(Kind kind) { return kPrefixes[static_cast<std::size_t>(kind)]; }

std::optional<Kind> kindFromPrefix(std::string_view p) {
    for (std::size_t i = 0; i < kKindCount; ++i)
        if (kPrefixes[i] == p) return static_cast<Kind>(i);
    return std::nullopt;
}

std::string Id::str() const { return std::string(prefix(kind)) + "_" + std::to_string(n); }

std::optional<Id> Id::parse(std::string_view text) {
    const auto us = text.find('_');
    if (us == std::string_view::npos || us + 1 >= text.size()) return std::nullopt;
    auto k = kindFromPrefix(text.substr(0, us));
    if (!k) return std::nullopt;
    const std::string_view digits = text.substr(us + 1);
    if (digits.size() > 1 && digits.front() == '0') return std::nullopt;  // canonical only
    std::uint32_t n = 0;
    auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), n);
    if (ec != std::errc{} || end != digits.data() + digits.size()) return std::nullopt;
    return Id{*k, n};
}

// --- session ------------------------------------------------------------------------------

Session Session::create(Project project) {
    Session s;
    s.project = std::make_shared<const Project>(std::move(project));
    for (auto& n : s.next.next) n = 1;
    EditTxn txn(s, 0);
    Track master;
    master.name = "Master";
    txn.insert(kMaster, std::move(master));  // the master is tr_0
    txn.create(TempoPointObj{0.0, 120.0, false});
    txn.create(MeterObj{1, 4, 4});
    auto out = txn.commit();
    return std::move(out).value();
}

// --- transactions --------------------------------------------------------------------------

EditTxn::EditTxn(const Session& base, std::uint64_t version)
    : base_(base),
      version_(version),
      next_(base.next),
      project_(base.project),
      blobs_(base.blobs) {}

void EditTxn::setProject(Project p) {
    project_ = std::make_shared<const Project>(std::move(p));
    projectChanged_ = true;
}

void EditTxn::putBlob(const std::string& hash, std::shared_ptr<const Blob> blob) {
    blobs_[hash] = std::move(blob);
}

void EditTxn::record(Change::Op op, Id id) {
    auto it = changes_.find(id);
    if (it == changes_.end()) {
        changes_.emplace(id, op);
        return;
    }
    Change::Op& cur = it->second;
    if (cur == Change::Op::Add && op == Change::Op::Remove)
        changes_.erase(it);  // created and removed in one transaction: nothing happened
    else if (cur == Change::Op::Add)
        ;  // add + update = add
    else
        cur = op;  // update + update = update; update + remove = remove
}

std::vector<Change> EditTxn::changes() const {
    std::vector<Change> out;
    out.reserve(changes_.size());
    for (const auto& [id, op] : changes_) out.push_back({op, id});
    return out;
}

namespace {
template <class T>
void apply(Session& s, TxnBuilders& b) {
    auto& slot = Traits<T>::slot(b);
    if (slot && slot->touched()) Traits<T>::map(s) = slot->build();
}
}  // namespace

Result<Session> EditTxn::commit() {
    if (committed_) return Error("model.txn", "transaction already committed");
    committed_ = true;
    Session s = base_;
    apply<TempoPointObj>(s, builders_);
    apply<MeterObj>(s, builders_);
    apply<Track>(s, builders_);
    apply<Item>(s, builders_);
    apply<Take>(s, builders_);
    apply<MediaSource>(s, builders_);
    apply<MidiSource>(s, builders_);
    apply<FxInstance>(s, builders_);
    apply<Route>(s, builders_);
    apply<Envelope>(s, builders_);
    apply<Marker>(s, builders_);
    apply<Region>(s, builders_);
    s.project = project_;
    s.blobs = blobs_;
    s.next = next_;
    s.version = version_;
    // Derive the time map from the tempo points and meter changes.
    std::vector<time::TempoPoint> tempo;
    for (const auto& e : s.tempo)
        tempo.push_back({e.value->beat, e.value->bpm,
                         e.value->ramp ? time::TempoShape::Ramp : time::TempoShape::Step});
    std::sort(tempo.begin(), tempo.end(),
              [](const auto& a, const auto& b) { return a.beat < b.beat; });
    std::vector<time::MeterChange> meter;
    for (const auto& e : s.meters)
        meter.push_back({e.value->bar, e.value->numerator, e.value->denominator});
    std::sort(meter.begin(), meter.end(),
              [](const auto& a, const auto& b) { return a.bar < b.bar; });
    auto map = time::TimeMap::create(std::move(tempo), std::move(meter));
    if (!map) {
        Error e = std::move(map).error();
        e.code = "model.invariant";
        return e;
    }
    s.timeMap = std::make_shared<const time::TimeMap>(std::move(*map));
    SUPPLE_TRY(checkInvariants(s));
    return s;
}

// --- invariants ----------------------------------------------------------------------------

namespace {

struct Checker {
    const Session& s;
    std::vector<Error> errors;

    void fail(std::string path, std::string message) {
        if (errors.size() < 50)
            errors.emplace_back("model.invariant", std::move(message), std::move(path));
    }
    static std::string at(const char* coll, Id id, const char* field = nullptr) {
        std::string p = std::string("/") + coll + "/" + id.str();
        if (field) p += std::string("/") + field;
        return p;
    }

    void counters() {
        auto check = [&](auto& map, Kind k) {
            for (const auto& e : map)
                if (e.id.n >= s.next[k])
                    fail("/project/next_ids/" + std::string(prefix(k)),
                         "counter " + std::to_string(s.next[k]) + " is not past " + e.id.str());
        };
        check(s.tempo, Kind::TempoPoint);
        check(s.meters, Kind::Meter);
        check(s.tracks, Kind::Track);
        check(s.items, Kind::Item);
        check(s.takes, Kind::Take);
        check(s.media, Kind::Media);
        check(s.midi, Kind::Midi);
        check(s.fx, Kind::Fx);
        check(s.routes, Kind::Route);
        check(s.envelopes, Kind::Envelope);
        check(s.markers, Kind::Marker);
        check(s.regions, Kind::Region);
    }

    void tracks() {
        const Track* master = s.tracks.find(kMaster);
        if (!master) {
            fail("/tracks/tr_0", "the master track tr_0 is missing");
            return;
        }
        if (master->parent) fail(at("tracks", kMaster, "parent"), "the master has no parent");
        for (const auto& e : s.tracks) {
            const Track& t = *e.value;
            if (e.id != kMaster) {
                if (!t.parent) {
                    fail(at("tracks", e.id, "parent"), e.id.str() + " has no parent");
                } else if (const Track* p = s.tracks.find(*t.parent); !p) {
                    fail(at("tracks", e.id, "parent"),
                         "parent " + t.parent->str() + " does not exist");
                } else if (std::count(p->children.begin(), p->children.end(), e.id) != 1) {
                    fail(at("tracks", e.id, "parent"),
                         t.parent->str() + " does not list " + e.id.str() + " exactly once");
                }
            }
            std::set<Id> seen;
            for (Id c : t.children) {
                if (!seen.insert(c).second)
                    fail(at("tracks", e.id, "children"), "duplicate child " + c.str());
                const Track* child = s.tracks.find(c);
                if (!child)
                    fail(at("tracks", e.id, "children"), "child " + c.str() + " does not exist");
                else if (child->parent != e.id)
                    fail(at("tracks", e.id, "children"), c.str() + " names another parent");
            }
            // Walk to the root: a cycle never reaches tr_0.
            std::size_t steps = 0;
            std::optional<Id> cur = e.id;
            while (cur && *cur != kMaster && steps <= s.tracks.size()) {
                const Track* x = s.tracks.find(*cur);
                cur = x ? x->parent : std::nullopt;
                ++steps;
            }
            if (e.id != kMaster && (!cur || *cur != kMaster))
                fail(at("tracks", e.id, "parent"), e.id.str() + " is not under the master");
            if (t.record.input && !s.routes.contains(*t.record.input))
                fail(at("tracks", e.id, "record"),
                     "record input " + t.record.input->str() + " does not exist");
        }
    }

    void itemsAndTakes() {
        std::map<Id, Id> takeOwner;
        for (const auto& e : s.items) {
            const Item& it = *e.value;
            if (!s.tracks.contains(it.track))
                fail(at("items", e.id, "track"), "track " + it.track.str() + " does not exist");
            if (it.track == kMaster)
                fail(at("items", e.id, "track"), "items cannot live on the master track");
            for (Id t : it.takes) {
                if (!s.takes.contains(t))
                    fail(at("items", e.id, "takes"), "take " + t.str() + " does not exist");
                auto [pos, fresh] = takeOwner.emplace(t, e.id);
                if (!fresh)
                    fail(at("items", e.id, "takes"),
                         t.str() + " also belongs to " + pos->second.str());
            }
            if (it.activeTake &&
                std::find(it.takes.begin(), it.takes.end(), *it.activeTake) == it.takes.end())
                fail(at("items", e.id, "active_take"),
                     it.activeTake->str() + " is not one of the item's takes");
            if (!it.takes.empty() && !it.activeTake)
                fail(at("items", e.id, "active_take"), "an item with takes needs an active take");
            if (!(it.length >= 0)) fail(at("items", e.id, "length"), "negative length");
        }
        for (const auto& e : s.takes) {
            if (!takeOwner.count(e.id)) fail(at("takes", e.id), e.id.str() + " belongs to no item");
            const Take& t = *e.value;
            if (t.source) {
                const bool ok = (t.source->kind == Kind::Media && s.media.contains(*t.source)) ||
                                (t.source->kind == Kind::Midi && s.midi.contains(*t.source));
                if (!ok)
                    fail(at("takes", e.id, "source"),
                         "source " + t.source->str() + " does not exist");
            }
        }
    }

    void fx() {
        std::map<Id, std::string> owner;
        auto claim = [&](const std::vector<Id>& chain, const std::string& where) {
            for (Id f : chain) {
                if (!s.fx.contains(f)) {
                    fail(where, "fx " + f.str() + " does not exist");
                    continue;
                }
                auto [pos, fresh] = owner.emplace(f, where);
                if (!fresh) fail(where, f.str() + " is also in " + pos->second);
            }
        };
        for (const auto& e : s.tracks) {
            claim(e.value->chain, at("tracks", e.id, "chain"));
            claim(e.value->inputChain, at("tracks", e.id, "input_chain"));
        }
        for (const auto& e : s.takes) claim(e.value->chain, at("takes", e.id, "chain"));
        for (const auto& e : s.fx)
            if (!owner.count(e.id)) fail(at("fx", e.id), e.id.str() + " is in no chain");
    }

    void routes() {
        std::map<Id, std::vector<Id>> edges;  // track -> tracks it feeds
        for (const auto& e : s.tracks)
            if (e.value->parent && e.value->parentSend.enabled)
                edges[e.id].push_back(*e.value->parent);
        for (const auto& e : s.routes) {
            const Route& r = *e.value;
            auto endOk = [&](const RouteEnd& x, const char* field, bool from) {
                const int n = (x.track ? 1 : 0) + (x.hwChannels ? 1 : 0) + (x.midiDevice ? 1 : 0);
                if (n != 1)
                    fail(at("routes", e.id, field),
                         "exactly one of track, hw_channels, midi_device");
                if (!from && x.midiDevice)
                    fail(at("routes", e.id, field), "a route cannot send to a MIDI input");
                if (x.track && !s.tracks.contains(*x.track))
                    fail(at("routes", e.id, field), "track " + x.track->str() + " does not exist");
            };
            endOk(r.from, "from", true);
            endOk(r.to, "to", false);
            if (r.from.track && r.to.track) edges[*r.from.track].push_back(*r.to.track);
        }
        // Cycle detection (iterative DFS with colours).
        std::map<Id, int> colour;
        for (const auto& [start, unused] : edges) {
            (void)unused;
            if (colour[start]) continue;
            std::vector<std::pair<Id, std::size_t>> stack{{start, 0}};
            colour[start] = 1;
            while (!stack.empty()) {
                auto& [node, i] = stack.back();
                auto it = edges.find(node);
                if (it == edges.end() || i >= it->second.size()) {
                    colour[node] = 2;
                    stack.pop_back();
                    continue;
                }
                const Id next = it->second[i++];
                if (colour[next] == 1) {
                    fail("/routes", "routing cycle through " + next.str());
                    return;
                }
                if (!colour[next]) {
                    colour[next] = 1;
                    stack.push_back({next, 0});
                }
            }
        }
    }

    void envelopes() {
        for (const auto& e : s.envelopes) {
            const Envelope& env = *e.value;
            const Id o = env.target.object;
            const bool ok = (o.kind == Kind::Track && s.tracks.contains(o)) ||
                            (o.kind == Kind::Route && s.routes.contains(o)) ||
                            (o.kind == Kind::Fx && s.fx.contains(o)) ||
                            (o.kind == Kind::Take && s.takes.contains(o));
            if (!ok) fail(at("envelopes", e.id, "target"), "target " + o.str() + " does not exist");
            for (std::size_t i = 1; i < env.points.size(); ++i)
                if (env.points[i].pos < env.points[i - 1].pos) {
                    fail(at("envelopes", e.id, "points"), "points are not sorted by position");
                    break;
                }
        }
        for (const auto& e : s.regions)
            if (e.value->end < e.value->start)
                fail(at("rulers/regions", e.id, "end"), "region ends before it starts");
    }
};

}  // namespace

Status checkInvariants(const Session& s) {
    Checker c{s, {}};
    if (!(s.project->sampleRate >= 8000 && s.project->sampleRate <= 384000))
        c.fail("/project/sample_rate", "sample rate out of range");
    c.counters();
    c.tracks();
    c.itemsAndTakes();
    c.fx();
    c.routes();
    c.envelopes();
    if (c.errors.empty()) return okStatus();
    if (c.errors.size() == 1) return c.errors.front();
    Error e("model.invariant", std::to_string(c.errors.size()) + " invariant violations");
    e.details = std::move(c.errors);
    return e;
}

// --- diff ------------------------------------------------------------------------------------

namespace {

template <class T>
void diffMap(const IdMap<T>& a, const IdMap<T>& b, std::vector<ObjectDiff>& out) {
    if (a.sameStorage(b)) return;
    auto ia = a.begin(), ib = b.begin();
    while (ia != a.end() || ib != b.end()) {
        if (ib == b.end() || (ia != a.end() && ia->id < ib->id)) {
            out.push_back({Change::Op::Remove, ia->id, {}});
            ++ia;
        } else if (ia == a.end() || ib->id < ia->id) {
            out.push_back({Change::Op::Add, ib->id, {}});
            ++ib;
        } else {
            if (ia->value != ib->value) {
                const json::Value ja = toJson(*ia->value), jb = toJson(*ib->value);
                if (!(ja == jb)) {
                    std::vector<std::string> fields;
                    std::set<std::string> keys;
                    for (const auto& m : ja.asObject()) keys.insert(m.key);
                    for (const auto& m : jb.asObject()) keys.insert(m.key);
                    for (const auto& k : keys) {
                        const json::Value* x = ja.find(k);
                        const json::Value* y = jb.find(k);
                        if (!x || !y || !(*x == *y)) fields.push_back(k);
                    }
                    out.push_back({Change::Op::Update, ia->id, std::move(fields)});
                }
            }
            ++ia;
            ++ib;
        }
    }
}

}  // namespace

std::vector<ObjectDiff> diff(const Session& a, const Session& b) {
    std::vector<ObjectDiff> out;
    // Kind order as in session-model §1 (tempo, rulers, tracks, items, …).
    diffMap(a.tempo, b.tempo, out);
    diffMap(a.meters, b.meters, out);
    diffMap(a.markers, b.markers, out);
    diffMap(a.regions, b.regions, out);
    diffMap(a.tracks, b.tracks, out);
    diffMap(a.items, b.items, out);
    diffMap(a.takes, b.takes, out);
    diffMap(a.fx, b.fx, out);
    diffMap(a.media, b.media, out);
    diffMap(a.midi, b.midi, out);
    diffMap(a.routes, b.routes, out);
    diffMap(a.envelopes, b.envelopes, out);
    return out;
}

}  // namespace supple::model
