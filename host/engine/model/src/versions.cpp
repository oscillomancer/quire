#include "supple/model/versions.h"

#include "supple/model/schemas.h"

#include <algorithm>
#include <set>

namespace supple::model {

namespace {

// Coalesces two change lists (a then b) per object, like EditTxn does.
std::vector<Change> coalesce(const std::vector<Change>& a, const std::vector<Change>& b) {
    std::map<Id, Change::Op> m;
    for (const auto& list : {std::cref(a), std::cref(b)})
        for (const Change& c : list.get()) {
            auto it = m.find(c.id);
            if (it == m.end()) {
                m.emplace(c.id, c.op);
            } else if (it->second == Change::Op::Add && c.op == Change::Op::Remove) {
                m.erase(it);
            } else if (it->second != Change::Op::Add) {
                it->second = c.op;
            }
        }
    std::vector<Change> out;
    for (const auto& [id, op] : m) out.push_back({op, id});
    return out;
}

// The entry for `id` in whichever collection holds its kind.
struct EntryInfo {
    bool exists = false;
    std::uint64_t rev = 0;
};
template <class T>
EntryInfo info(const IdMap<T>& m, Id id) {
    const auto* e = m.entry(id);
    return e ? EntryInfo{true, e->rev} : EntryInfo{};
}
EntryInfo entryInfo(const Session& s, Id id) {
    switch (id.kind) {
        case Kind::Track:
            return info(s.tracks, id);
        case Kind::Item:
            return info(s.items, id);
        case Kind::Take:
            return info(s.takes, id);
        case Kind::Media:
            return info(s.media, id);
        case Kind::Midi:
            return info(s.midi, id);
        case Kind::Fx:
            return info(s.fx, id);
        case Kind::Route:
            return info(s.routes, id);
        case Kind::Envelope:
            return info(s.envelopes, id);
        case Kind::AutoItem:
            return info(s.autoItems, id);
        case Kind::Marker:
            return info(s.markers, id);
        case Kind::Region:
            return info(s.regions, id);
        case Kind::TempoPoint:
            return info(s.tempo, id);
        case Kind::Meter:
            return info(s.meters, id);
        default:
            return {};
    }
}

// The object's JSON form (null when absent), for field-level comparisons.
json::Value objectJson(const Session& s, Id id) {
    auto enc = [&](const auto& map) -> json::Value {
        const auto* v = map.find(id);
        return v ? toJson(*v) : json::Value();
    };
    switch (id.kind) {
        case Kind::Track:
            return enc(s.tracks);
        case Kind::Item:
            return enc(s.items);
        case Kind::Take:
            return enc(s.takes);
        case Kind::Media:
            return enc(s.media);
        case Kind::Midi:
            return enc(s.midi);
        case Kind::Fx:
            return enc(s.fx);
        case Kind::Route:
            return enc(s.routes);
        case Kind::Envelope:
            return enc(s.envelopes);
        case Kind::AutoItem:
            return enc(s.autoItems);
        case Kind::Marker:
            return enc(s.markers);
        case Kind::Region:
            return enc(s.regions);
        case Kind::TempoPoint:
            return enc(s.tempo);
        case Kind::Meter:
            return enc(s.meters);
        default:
            return {};
    }
}

// Structural lists that a replayed command recomputes on the target (a track
// added on each side appends to the parent's children on both): never a
// conflict by themselves.
bool structural(const std::string& field) {
    return field == "children" || field == "chain" || field == "input_chain" || field == "takes" ||
           field == "items";
}

std::set<std::string> changedFields(const Session& before, const Session& after, Id id) {
    const json::Value a = objectJson(before, id), b = objectJson(after, id);
    std::set<std::string> out;
    std::set<std::string> keys;
    if (a.isObject())
        for (const auto& m : a.asObject()) keys.insert(m.key);
    if (b.isObject())
        for (const auto& m : b.asObject()) keys.insert(m.key);
    for (const auto& k : keys) {
        const json::Value* x = a.isObject() ? a.find(k) : nullptr;
        const json::Value* y = b.isObject() ? b.find(k) : nullptr;
        if ((!x) != (!y) || (x && y && !(*x == *y))) out.insert(k);
    }
    return out;
}

}  // namespace

namespace {

// "^tr_[0-9]+$" (one kind) or "^[a-z]{2}_[0-9]+$" (any kind).
bool idPattern(const json::Value* schema, const char* key) {
    const json::Value* p = schema && schema->isObject() ? schema->find(key) : nullptr;
    if (!p || !p->isString()) return false;
    const std::string& s = p->asString();
    constexpr std::string_view kTail = "_[0-9]+$";
    return s.size() > kTail.size() + 1 && s.front() == '^' &&
           std::string_view(s).substr(s.size() - kTail.size()) == kTail;
}

std::string remapped(const std::string& text, const std::map<Id, Id>& remap) {
    if (auto id = Id::parse(text))
        if (auto it = remap.find(*id); it != remap.end()) return it->second.str();
    return text;
}

bool typeMatches(const json::Value& v, const json::Value& schema) {
    const json::Value* t = schema.find("type");
    if (!t || !t->isString()) return true;
    const std::string& s = t->asString();
    return (s == "string" && v.isString()) || (s == "object" && v.isObject()) ||
           (s == "array" && v.isArray()) || (s == "number" && v.isNumber()) ||
           (s == "integer" && v.isNumber()) || (s == "boolean" && v.isBool()) ||
           (s == "null" && v.isNull());
}

// A oneOf/anyOf alternative applies when its tag constants match (tagged
// variants) and its type matches.
bool alternativeMatches(const json::Value& v, const json::Value& alt) {
    if (!alt.isObject() || !typeMatches(v, alt)) return false;
    if (const json::Value* props = alt.find("properties");
        props && props->isObject() && v.isObject())
        for (const auto& p : props->asObject())
            if (const json::Value* c = p.value.isObject() ? p.value.find("const") : nullptr) {
                const json::Value* x = v.find(p.key);
                if (!x || !(*x == *c)) return false;
            }
    return true;
}

using Defs = std::map<std::string, const json::Value*>;

json::Value remapWalk(const json::Value& v, const json::Value* schema, Defs defs,
                      const std::map<Id, Id>& remap, int depth) {
    if (!schema || !schema->isObject() || depth > 64) return v;
    if (const json::Value* d = schema->find("$defs"); d && d->isObject())
        for (const auto& m : d->asObject()) defs[m.key] = &m.value;
    if (const json::Value* ref = schema->find("$ref"); ref && ref->isString()) {
        constexpr std::string_view kPrefix = "#/$defs/";
        const std::string& r = ref->asString();
        if (r.rfind(kPrefix, 0) == 0)
            if (auto it = defs.find(r.substr(kPrefix.size())); it != defs.end())
                return remapWalk(v, it->second, defs, remap, depth + 1);
        return v;
    }
    for (const char* k : {"oneOf", "anyOf"})
        if (const json::Value* alts = schema->find(k); alts && alts->isArray()) {
            for (const auto& alt : alts->asArray())
                if (alternativeMatches(v, alt)) return remapWalk(v, &alt, defs, remap, depth + 1);
            return v;
        }
    if (v.isString())
        return idPattern(schema, "pattern") ? json::Value(remapped(v.asString(), remap)) : v;
    if (v.isArray()) {
        const json::Value* items = schema->find("items");
        json::Value out = json::Value::array();
        for (const auto& x : v.asArray())
            out.push_back(remapWalk(x, items, defs, remap, depth + 1));
        return out;
    }
    if (v.isObject()) {
        const json::Value* props = schema->find("properties");
        const json::Value* extra = schema->find("additionalProperties");
        const bool idKeys = idPattern(schema->find("propertyNames"), "pattern");
        json::Value out = json::Value::object();
        for (const auto& m : v.asObject()) {
            const json::Value* sub = props && props->isObject() ? props->find(m.key) : nullptr;
            if (!sub) sub = extra;
            out[idKeys ? remapped(m.key, remap) : m.key] =
                remapWalk(m.value, sub, defs, remap, depth + 1);
        }
        return out;
    }
    return v;
}

}  // namespace

json::Value remapIds(const json::Value& args, const json::Value& schema,
                     const std::map<Id, Id>& remap) {
    if (remap.empty()) return args;
    return remapWalk(args, &schema, {}, remap, 0);
}

VersionStore::VersionStore(Session initial) {
    const std::uint64_t id = initial.version;
    Version v;
    v.id = id;
    v.session = std::make_shared<const Session>(std::move(initial));
    v.txn.label = "initial";
    versions_.emplace(id, std::move(v));
    branches_.emplace("main", Branch{id, id, {}});
    next_ = id + 1;
}

const VersionStore::Branch* VersionStore::lookup(std::string_view name) const {
    auto it = branches_.find(name);
    return it == branches_.end() ? nullptr : &it->second;
}

Result<VersionStore::Branch*> VersionStore::find(std::string_view name) {
    auto it = branches_.find(name);
    if (it == branches_.end()) {
        std::string names;
        for (const auto& [n, b] : branches_) names += (names.empty() ? "" : ", ") + n;
        return Error("branch.unknown", "no branch named \"" + std::string(name) + "\"", {},
                     "branches: " + names);
    }
    return &it->second;
}

std::shared_ptr<const Session> VersionStore::head(std::string_view name) const {
    const Branch* b = lookup(name);
    return b ? versions_.at(b->head).session : nullptr;
}

std::uint64_t VersionStore::headVersion(std::string_view name) const {
    const Branch* b = lookup(name);
    return b ? b->head : 0;
}

const Version* VersionStore::version(std::uint64_t id) const {
    auto it = versions_.find(id);
    return it == versions_.end() ? nullptr : &it->second;
}

void VersionStore::forEachVersion(const std::function<void(const Version&)>& visit) const {
    for (const auto& [id, v] : versions_) visit(v);
}

Result<std::uint64_t> VersionStore::commit(Session session, TxnRecord txn,
                                           std::vector<Change> changes, std::vector<Id> created,
                                           std::uint64_t timeMs) {
    if (session.version != next_)
        return Error("history.version", "the session was built as version " +
                                            std::to_string(session.version) + ", expected " +
                                            std::to_string(next_));
    Branch& b = branches_.at(current_);
    const Version& head = versions_.at(b.head);
    Version v;
    v.id = next_++;
    v.txn = std::move(txn);
    v.timeMs = timeMs;
    v.session = std::make_shared<const Session>(std::move(session));
    // The head may only be replaced when nothing else points at it: another
    // branch's head, base or redo stack, or a version built on it.
    auto referencedElsewhere = [&](std::uint64_t id) {
        for (const auto& [name, other] : branches_) {
            if (name == current_) continue;
            if (other.head == id || other.base == id ||
                std::find(other.redo.begin(), other.redo.end(), id) != other.redo.end())
                return true;
        }
        for (const auto& [vid, ver] : versions_)
            if (std::find(ver.parents.begin(), ver.parents.end(), id) != ver.parents.end())
                return true;
        return false;
    };
    const bool coalesceWithHead = !v.txn.coalesceKey.empty() && b.head != b.base &&
                                  b.redo.empty() && head.txn.coalesceKey == v.txn.coalesceKey &&
                                  !referencedElsewhere(b.head);
    if (coalesceWithHead) {
        // Replace the head: same parent, combined changes and invocations.
        v.parents = head.parents;
        v.changes = coalesce(head.changes, changes);
        v.created = head.created;
        v.created.insert(v.created.end(), created.begin(), created.end());
        std::vector<Invocation> inv = head.txn.invocations;
        inv.insert(inv.end(), v.txn.invocations.begin(), v.txn.invocations.end());
        v.txn.invocations = std::move(inv);
        const std::uint64_t replaced = b.head;
        b.head = v.id;
        versions_.emplace(v.id, std::move(v));
        versions_.erase(replaced);
    } else {
        v.parents = {b.head};
        v.changes = std::move(changes);
        v.created = std::move(created);
        b.head = v.id;
        b.redo.clear();
        versions_.emplace(v.id, std::move(v));
    }
    return b.head;
}

bool VersionStore::canUndo() const {
    const Branch& b = branches_.at(current_);
    return b.head != b.base;
}

bool VersionStore::canRedo() const { return !branches_.at(current_).redo.empty(); }

Status VersionStore::undo() {
    Branch& b = branches_.at(current_);
    if (b.head == b.base)
        return Error("history.nothing_to_undo", "nothing to undo on branch " + current_);
    b.redo.push_back(b.head);
    b.head = versions_.at(b.head).parents.front();
    return okStatus();
}

Status VersionStore::redo() {
    Branch& b = branches_.at(current_);
    if (b.redo.empty())
        return Error("history.nothing_to_redo", "nothing to redo on branch " + current_);
    b.head = b.redo.back();
    b.redo.pop_back();
    return okStatus();
}

std::vector<const Version*> VersionStore::history(std::size_t limit) const {
    std::vector<const Version*> out;
    const Branch& b = branches_.at(current_);
    std::uint64_t id = b.head;
    while (out.size() < limit) {
        const Version& v = versions_.at(id);
        out.push_back(&v);
        if (id == b.base || v.parents.empty()) break;
        id = v.parents.front();
    }
    return out;
}

Status VersionStore::createBranch(const std::string& name, std::string_view from) {
    if (name.empty() || name.size() > 100)
        return Error("branch.name", "branch names are 1-100 characters", "/name");
    if (branches_.count(name))
        return Error("branch.exists", "branch \"" + name + "\" already exists", "/name",
                     "pick another name or switch to it");
    auto src = find(from);
    if (!src) return std::move(src).error();
    const std::uint64_t at = (*src)->head;
    branches_.emplace(name, Branch{at, at, {}});
    return okStatus();
}

Status VersionStore::switchBranch(std::string_view name) {
    auto b = find(name);
    if (!b) return std::move(b).error();
    current_ = std::string(name);
    return okStatus();
}

Status VersionStore::deleteBranch(std::string_view name) {
    if (name == "main") return Error("branch.protected", "main cannot be deleted");
    if (name == current_)
        return Error("branch.current", "cannot delete the current branch", {}, "switch first");
    auto b = find(name);
    if (!b) return std::move(b).error();
    branches_.erase(branches_.find(name));
    return okStatus();
}

std::vector<std::string> VersionStore::branches() const {
    std::vector<std::string> out;
    for (const auto& [n, b] : branches_) out.push_back(n);
    return out;
}

std::uint64_t VersionStore::forkPoint(std::uint64_t a, std::uint64_t b) const {
    auto ancestors = [&](std::uint64_t start) {
        std::set<std::uint64_t> seen;
        std::vector<std::uint64_t> stack{start};
        while (!stack.empty()) {
            const std::uint64_t id = stack.back();
            stack.pop_back();
            if (!seen.insert(id).second) continue;
            auto it = versions_.find(id);
            if (it == versions_.end()) continue;
            for (std::uint64_t p : it->second.parents) stack.push_back(p);
        }
        return seen;
    };
    const auto ofA = ancestors(a), ofB = ancestors(b);
    std::uint64_t best = 0;
    bool found = false;
    for (std::uint64_t id : ofA)
        if (ofB.count(id) && (!found || id > best)) {
            best = id;
            found = true;
        }
    return best;
}

MergeOutcome VersionStore::merge(std::string_view from, std::string_view into,
                                 const Replayer& replay, const ArgSchemas& schemas,
                                 std::uint64_t timeMs) {
    MergeOutcome out;
    const Branch* src = lookup(from);
    const Branch* dstc = lookup(into);
    if (!src || !dstc) {
        out.conflicts.push_back({Conflict::Kind::Invalid, 0, 0, std::nullopt,
                                 "unknown branch " + std::string(src ? into : from)});
        return out;
    }
    Branch& dst = branches_.find(into)->second;
    out.head = dst.head;
    const std::uint64_t fork = forkPoint(src->head, dst.head);
    if (fork == src->head) {
        out.upToDate = true;
        return out;
    }
    if (fork == dst.head) {
        dst.head = src->head;
        dst.redo.clear();
        out.head = dst.head;
        out.fastForward = true;
        return out;
    }
    // The source's transactions since the fork, oldest first (first-parent chain).
    std::vector<const Version*> path;
    for (std::uint64_t id = src->head; id != fork;) {
        const Version& v = versions_.at(id);
        if (v.parents.empty()) {
            out.conflicts.push_back({Conflict::Kind::Invalid, 0, id, std::nullopt,
                                     "the fork point is not on the branch's first-parent history"});
            return out;
        }
        path.push_back(&v);
        id = v.parents.front();
    }
    std::reverse(path.begin(), path.end());

    const Session& forkState = *versions_.at(fork).session;
    const Session& targetHead = *versions_.at(dst.head).session;
    std::shared_ptr<const Session> state = versions_.at(dst.head).session;
    std::vector<Version> staged;
    std::uint64_t nextId = next_;
    std::uint64_t parent = dst.head;
    for (std::size_t step = 0; step < path.size(); ++step) {
        const Version& v = *path[step];
        // Concurrent edits: objects this transaction changed that the target
        // also changed since the fork — in the same (non-structural) field — or
        // removed; or objects the transaction removes that the target changed.
        const Session& before = *versions_.at(v.parents.front()).session;
        for (const Change& c : v.changes) {
            if (c.op == Change::Op::Add) continue;
            const auto atFork = entryInfo(forkState, c.id);
            if (!atFork.exists) continue;  // created on the branch itself
            const auto onTarget = entryInfo(targetHead, c.id);
            if (onTarget.exists && onTarget.rev == atFork.rev) continue;  // untouched on target
            std::string why;
            if (!onTarget.exists) {
                why = " was removed on " + std::string(into) + " since the fork";
            } else if (c.op == Change::Op::Remove) {
                why = " was changed on " + std::string(into) + " since the fork";
            } else {
                const auto mine = changedFields(before, *v.session, c.id);
                const auto theirs = changedFields(forkState, targetHead, c.id);
                std::string both;
                for (const auto& f : mine)
                    if (!structural(f) && theirs.count(f)) both += (both.empty() ? "" : ", ") + f;
                if (both.empty()) continue;
                why = ": both branches changed " + both;
            }
            out.conflicts.push_back(
                {Conflict::Kind::ConcurrentEdit, step, v.id, c.id, c.id.str() + why});
        }
        if (!out.conflicts.empty()) {
            out.head = dst.head;
            return out;
        }
        std::vector<Invocation> inv;
        for (const auto& i : v.txn.invocations) {
            const json::Value* schema = schemas ? schemas(i.command) : nullptr;
            inv.push_back({i.command, schema ? remapIds(i.args, *schema, out.remap) : i.args});
        }
        auto result = replay(*state, inv, nextId);
        if (!result) {
            out.conflicts.push_back(
                {Conflict::Kind::Invalid, step, v.id, std::nullopt, result.error().toString()});
            out.head = dst.head;
            return out;
        }
        if (result->created.size() != v.created.size()) {
            out.conflicts.push_back(
                {Conflict::Kind::Invalid, step, v.id, std::nullopt,
                 "the replay created " + std::to_string(result->created.size()) +
                     " objects, the original " + std::to_string(v.created.size())});
            out.head = dst.head;
            return out;
        }
        for (std::size_t i = 0; i < v.created.size(); ++i)
            if (v.created[i] != result->created[i]) out.remap[v.created[i]] = result->created[i];
        Version nv;
        nv.id = nextId++;
        nv.parents = {parent};
        nv.txn = v.txn;
        nv.txn.invocations = std::move(inv);
        nv.changes = std::move(result->changes);
        nv.created = std::move(result->created);
        nv.timeMs = timeMs;
        nv.session = std::make_shared<const Session>(std::move(result->session));
        state = nv.session;
        parent = nv.id;
        staged.push_back(std::move(nv));
    }
    // Success: the last step also records the source head as a parent, so a
    // later merge of the same branch finds nothing new.
    staged.back().parents.push_back(src->head);
    for (auto& nv : staged) {
        const std::uint64_t id = nv.id;
        versions_.emplace(id, std::move(nv));
    }
    next_ = nextId;
    dst.head = parent;
    dst.redo.clear();
    out.head = dst.head;
    return out;
}

}  // namespace supple::model
