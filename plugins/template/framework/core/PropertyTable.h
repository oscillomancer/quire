// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Binds the framework to the product's properties (product/Properties.h, when
// it has one; see core/Property.h) and checks them at compile time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string_view>

#include "core/Property.h"

#if __has_include("Properties.h")
#include "Properties.h"
#define SUPPLE_HAS_PROPERTIES 1
#else
#define SUPPLE_HAS_PROPERTIES 0
#endif

namespace supple::properties {

// The longest value any property may declare: state is read whole into
// memory, and a property is not a place for audio.
inline constexpr uint32_t kMaxBytes = 1u << 20;

#if SUPPLE_HAS_PROPERTIES
inline constexpr size_t kCount = std::size(product::kProperties);
inline constexpr const PropertyDef& at(size_t index) { return product::kProperties[index]; }
#else
inline constexpr size_t kCount = 0;
inline constexpr PropertyDef kNone{};
inline constexpr const PropertyDef& at(size_t) { return kNone; }
#endif
// For arrays sized by the table.
inline constexpr size_t kSlots = kCount > 0 ? kCount : 1;

inline constexpr size_t countFiles() {
    size_t n = 0;
    for (size_t i = 0; i < kCount; ++i)
        if (at(i).isFile()) ++n;
    return n;
}
// Properties marked file(): the framework loads what they name.
inline constexpr size_t kFileCount = countFiles();

namespace detail {

constexpr size_t length(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

constexpr bool sameString(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

constexpr bool idsUnique() {
    for (size_t i = 0; i < kCount; ++i)
        for (size_t j = i + 1; j < kCount; ++j)
            if (at(i).id == at(j).id) return false;
    return true;
}

constexpr bool namesUnique() {
    for (size_t i = 0; i < kCount; ++i) {
        if (!at(i).name || !at(i).name[0]) return false;
        for (size_t j = i + 1; j < kCount; ++j)
            if (sameString(at(i).name, at(j).name)) return false;
    }
    return true;
}

constexpr bool limitsValid() {
    for (size_t i = 0; i < kCount; ++i)
        if (at(i).limit == 0 || at(i).limit > kMaxBytes || length(at(i).def) > at(i).limit) return false;
    return true;
}

}  // namespace detail

#if SUPPLE_HAS_PROPERTIES
static_assert(kCount > 0, "product::kProperties is empty: delete product/Properties.h instead");
static_assert(detail::idsUnique(), "two properties share an ID");
static_assert(detail::namesUnique(), "property names must be present and unique");
static_assert(detail::limitsValid(), "a property's limit is 0 or above 1 MiB, or its default is longer than it");
#endif

// Table index of the property with this ID, or -1.
inline constexpr int indexOf(uint32_t id) {
    for (size_t i = 0; i < kCount; ++i)
        if (at(i).id == id) return static_cast<int>(i);
    return -1;
}

inline bool accepts(size_t index, std::string_view value) { return value.size() <= at(index).limit; }

}  // namespace supple::properties

namespace supple {
// The product table, also usable by stores instantiated with a synthetic table.
struct PropertyTable {
    static constexpr size_t kCount = properties::kCount;
    static constexpr size_t kSlots = properties::kSlots;
    static constexpr const PropertyDef& at(size_t i) { return properties::at(i); }
};
}  // namespace supple
