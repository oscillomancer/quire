// IdMap<T>: an immutable, ID-ordered collection of shared immutable objects
// (session-model §2). Copying a map is a pointer copy; an edit copies the
// entry vector once per transaction (Builder) and shares every untouched
// object. Each entry carries the revision (version number) that last changed
// it, used for merge conflict detection.
//
// Storage is a sorted vector today; the interface allows swapping in a
// persistent trie for very large collections without touching callers.
#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

#include "supple/model/ids.h"

namespace supple::model {

template <class T>
class IdMap {
  public:
    struct Entry {
        Id id;
        std::uint64_t rev = 0;
        std::shared_ptr<const T> value;
    };

    IdMap() : data_(emptyData()) {}

    std::size_t size() const { return data_->size(); }
    bool empty() const { return data_->empty(); }
    auto begin() const { return data_->begin(); }
    auto end() const { return data_->end(); }

    const Entry* entry(Id id) const {
        auto it = lower(*data_, id);
        return it != data_->end() && it->id == id ? &*it : nullptr;
    }
    const T* find(Id id) const {
        const Entry* e = entry(id);
        return e ? e->value.get() : nullptr;
    }
    bool contains(Id id) const { return entry(id) != nullptr; }

    // True when both maps share storage (then they are equal) — lets diffs
    // skip untouched collections in O(1).
    bool sameStorage(const IdMap& other) const { return data_ == other.data_; }

    // Batch edits within one transaction: copies the entries on first write.
    class Builder {
      public:
        explicit Builder(const IdMap& base) : base_(base) {}
        const Entry* entry(Id id) const {
            const auto& v = touched_ ? copy_ : *base_.data_;
            auto it = lower(v, id);
            return it != v.end() && it->id == id ? &*it : nullptr;
        }
        const T* find(Id id) const {
            const Entry* e = entry(id);
            return e ? e->value.get() : nullptr;
        }
        void put(Id id, std::uint64_t rev, std::shared_ptr<const T> value) {
            touch();
            auto it = lower(copy_, id);
            if (it != copy_.end() && it->id == id) {
                it->rev = rev;
                it->value = std::move(value);
            } else {
                copy_.insert(it, Entry{id, rev, std::move(value)});
            }
        }
        bool erase(Id id) {
            touch();
            auto it = lower(copy_, id);
            if (it == copy_.end() || it->id != id) return false;
            copy_.erase(it);
            return true;
        }
        const std::vector<Entry>& entries() const { return touched_ ? copy_ : *base_.data_; }
        bool touched() const { return touched_; }
        IdMap build() const {
            if (!touched_) return base_;
            IdMap m;
            m.data_ = std::make_shared<const std::vector<Entry>>(copy_);
            return m;
        }

      private:
        void touch() {
            if (!touched_) {
                copy_ = *base_.data_;
                touched_ = true;
            }
        }
        IdMap base_;
        std::vector<Entry> copy_;
        bool touched_ = false;
    };

    // Single edits (tests, loaders): each returns a new map.
    IdMap with(Id id, std::uint64_t rev, std::shared_ptr<const T> value) const {
        Builder b(*this);
        b.put(id, rev, std::move(value));
        return b.build();
    }

  private:
    using Vec = std::vector<Entry>;
    static typename Vec::const_iterator lower(const Vec& v, Id id) {
        return std::lower_bound(v.begin(), v.end(), id,
                                [](const Entry& e, Id x) { return e.id < x; });
    }
    static typename Vec::iterator lower(Vec& v, Id id) {
        return std::lower_bound(v.begin(), v.end(), id,
                                [](const Entry& e, Id x) { return e.id < x; });
    }
    static std::shared_ptr<const Vec> emptyData() {
        static const auto empty = std::make_shared<const Vec>();
        return empty;
    }
    std::shared_ptr<const Vec> data_;
};

}  // namespace supple::model
