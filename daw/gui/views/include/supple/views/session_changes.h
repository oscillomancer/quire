// Adjacent versions supply their transaction change list. Branch jumps and
// callers without history compare shared object identities instead.
#pragma once

#include <set>
#include "supple/model/session.h"

namespace supple::views {

template <class T>
std::set<model::Id> changedObjects(const model::IdMap<T>& before, const model::IdMap<T>& after,
                                   const std::vector<model::Change>* changes = nullptr) {
    std::set<model::Id> result;
    if (before.sameStorage(after)) return result;
    if (changes) {
        for (const auto& change : *changes)
            if (change.id.kind == model::Traits<T>::kind &&
                before.find(change.id) != after.find(change.id))
                result.insert(change.id);
        return result;
    }
    auto a = before.begin(), b = after.begin();
    while (a != before.end() || b != after.end()) {
        if (b == after.end() || (a != before.end() && a->id < b->id)) {
            result.insert(a++->id);
        } else if (a == before.end() || b->id < a->id) {
            result.insert(b++->id);
        } else {
            if (a->value != b->value) result.insert(a->id);
            ++a;
            ++b;
        }
    }
    return result;
}

}  // namespace supple::views
