// The immutable session (session-model §1–4): every object in ID-keyed maps,
// edited only through EditTxn, validated by invariants, diffable.
#pragma once

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "supple/base/result.h"
#include "supple/model/idmap.h"
#include "supple/model/objects.h"
#include "supple/time/timeline.h"

namespace supple::model {

struct Blob {
    std::vector<std::uint8_t> data;
};

struct Session {
    std::shared_ptr<const Project> project = std::make_shared<const Project>();
    Counters next;  // next ID per kind (project.next_ids in the file)
    IdMap<TempoPointObj> tempo;
    IdMap<MeterObj> meters;
    std::shared_ptr<const time::TimeMap> timeMap = std::make_shared<const time::TimeMap>();
    IdMap<Track> tracks;
    IdMap<Item> items;
    IdMap<Take> takes;
    IdMap<MediaSource> media;
    IdMap<MidiSource> midi;
    IdMap<FxInstance> fx;
    IdMap<Route> routes;
    IdMap<Envelope> envelopes;
    IdMap<Marker> markers;
    IdMap<Region> regions;
    std::map<std::string, std::shared_ptr<const Blob>> blobs;  // "sha256:<hex>"
    std::uint64_t version = 0;  // the version that produced this state

    // A new project: master track tr_0, 120 BPM at beat 0 (tp_1), 4/4 at
    // bar 1 (ts_1), counters past them.
    static Session create(Project project = {});
};

// An EditTxn's per-collection builders (created on first touch).
struct TxnBuilders {
    std::optional<IdMap<TempoPointObj>::Builder> tempo;
    std::optional<IdMap<MeterObj>::Builder> meters;
    std::optional<IdMap<Track>::Builder> tracks;
    std::optional<IdMap<Item>::Builder> items;
    std::optional<IdMap<Take>::Builder> takes;
    std::optional<IdMap<MediaSource>::Builder> media;
    std::optional<IdMap<MidiSource>::Builder> midi;
    std::optional<IdMap<FxInstance>::Builder> fx;
    std::optional<IdMap<Route>::Builder> routes;
    std::optional<IdMap<Envelope>::Builder> envelopes;
    std::optional<IdMap<Marker>::Builder> markers;
    std::optional<IdMap<Region>::Builder> regions;
};

// Which map of the Session holds objects of type T, and their ID kind.
template <class T>
struct Traits;
#define SUPPLE_MODEL_TRAITS(T, K, member)                                                  \
    template <>                                                                            \
    struct Traits<T> {                                                                     \
        static constexpr Kind kind = Kind::K;                                              \
        static IdMap<T>& map(Session& s) { return s.member; }                              \
        static const IdMap<T>& map(const Session& s) { return s.member; }                  \
        static std::optional<IdMap<T>::Builder>& slot(TxnBuilders& b) { return b.member; } \
    };
SUPPLE_MODEL_TRAITS(TempoPointObj, TempoPoint, tempo)
SUPPLE_MODEL_TRAITS(MeterObj, Meter, meters)
SUPPLE_MODEL_TRAITS(Track, Track, tracks)
SUPPLE_MODEL_TRAITS(Item, Item, items)
SUPPLE_MODEL_TRAITS(Take, Take, takes)
SUPPLE_MODEL_TRAITS(MediaSource, Media, media)
SUPPLE_MODEL_TRAITS(MidiSource, Midi, midi)
SUPPLE_MODEL_TRAITS(FxInstance, Fx, fx)
SUPPLE_MODEL_TRAITS(Route, Route, routes)
SUPPLE_MODEL_TRAITS(Envelope, Envelope, envelopes)
SUPPLE_MODEL_TRAITS(Marker, Marker, markers)
SUPPLE_MODEL_TRAITS(Region, Region, regions)
#undef SUPPLE_MODEL_TRAITS

// One object-level change made by a transaction.
struct Change {
    enum class Op : std::uint8_t { Add, Remove, Update };
    Op op;
    Id id;
    friend bool operator==(const Change&, const Change&) = default;
};

// The only way to build a new session version. Reads see the transaction's
// own edits. IDs come from the session's counters; every touched object is
// stamped with the new version as its revision. commit() validates the
// invariants (a violation means a command let a bad edit through: a bug).
class EditTxn {
  public:
    EditTxn(const Session& base, std::uint64_t version);

    const Session& base() const { return base_; }
    std::uint64_t version() const { return version_; }

    template <class T>
    const T* get(Id id) const {
        return builder<T>().find(id);
    }
    template <class T>
    bool exists(Id id) const {
        return get<T>(id) != nullptr;
    }
    // Creates an object with the next ID of its kind.
    template <class T>
    Id create(T value) {
        const Id id{Traits<T>::kind, next_[Traits<T>::kind]++};
        builder<T>().put(id, version_, std::make_shared<const T>(std::move(value)));
        record(Change::Op::Add, id);
        created_.push_back(id);
        return id;
    }
    // Adds an object under an explicit ID (the master, loaders, merge replay);
    // false if the ID is taken. Counters move past it.
    template <class T>
    bool insert(Id id, T value) {
        auto& b = builder<T>();
        if (id.kind != Traits<T>::kind || b.find(id)) return false;
        b.put(id, version_, std::make_shared<const T>(std::move(value)));
        if (next_[id.kind] <= id.n) next_[id.kind] = id.n + 1;
        record(Change::Op::Add, id);
        created_.push_back(id);
        return true;
    }
    // Replaces an existing object (false if it does not exist).
    template <class T>
    bool put(Id id, T value) {
        auto& b = builder<T>();
        if (!b.find(id)) return false;
        b.put(id, version_, std::make_shared<const T>(std::move(value)));
        record(Change::Op::Update, id);
        return true;
    }
    // Copy, modify, store: txn.modify<Track>(id, [](Track& t) { t.mute = true; }).
    template <class T, class F>
    bool modify(Id id, F&& f) {
        const T* cur = get<T>(id);
        if (!cur) return false;
        T copy = *cur;
        f(copy);
        return put<T>(id, std::move(copy));
    }
    template <class T>
    bool remove(Id id) {
        if (!builder<T>().erase(id)) return false;
        record(Change::Op::Remove, id);
        return true;
    }

    const Project& project() const { return *project_; }
    void setProject(Project p);
    bool projectChanged() const { return projectChanged_; }
    void putBlob(const std::string& hash, std::shared_ptr<const Blob> blob);

    // Changes so far, coalesced per object (add+update = add, add+remove = none).
    std::vector<Change> changes() const;
    // IDs created so far, in creation order (removed ones included: replay
    // merges match creations by order).
    const std::vector<Id>& created() const { return created_; }
    // The session as it would be committed now, without validation (queries
    // inside a batch read their earlier steps' effects through this).
    Session preview() const;

    // Validates and returns the new session. The transaction is spent.
    Result<Session> commit();

  private:
    template <class T>
    typename IdMap<T>::Builder& builder() const {
        auto& slot = Traits<T>::slot(builders_);
        if (!slot) slot.emplace(Traits<T>::map(base_));
        return *slot;
    }
    void record(Change::Op op, Id id);

    Session base_;
    std::uint64_t version_;
    Counters next_;
    std::shared_ptr<const Project> project_;
    std::map<std::string, std::shared_ptr<const Blob>> blobs_;
    mutable TxnBuilders builders_;
    std::map<Id, Change::Op> changes_;
    std::vector<Id> created_;
    bool committed_ = false;
    bool projectChanged_ = false;
};

// --- invariants and diffs --------------------------------------------------------

// Checks every structural invariant (session-model §3): the track tree is
// rooted at tr_0, every reference resolves, active takes belong to their
// items, no route/parent-send cycles, each fx in exactly one chain, envelope
// points sorted, counters past every used ID, tempo point at beat 0 and meter
// at bar 1. Errors: code "model.invariant" with a path like "/tracks/tr_3".
Status checkInvariants(const Session& s);

// The track whose FX chain holds `fx` (not input chains or take FX), if any.
std::optional<Id> trackOfFx(const Session& s, Id fx);

// Object-level differences, deterministic order (kind, then ID). `fields`
// lists the changed top-level fields of updated objects (JSON member names).
struct ObjectDiff {
    Change::Op op;
    Id id;
    std::vector<std::string> fields;
};
std::vector<ObjectDiff> diff(const Session& a, const Session& b);

}  // namespace supple::model
