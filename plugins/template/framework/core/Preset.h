// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Factory presets: a table in product/Presets.h, for a product that switched
// presets on (PRODUCT_PRESETS ON in product.cmake):
//
//   inline constexpr supple::Preset kPresets[] = {
//       {"Slow Wobble", "Gentle", {{kRate, 0.5}, {kDepth, 0.4}}},
//       {"Seventh", "", {{kShape, 6}}, {{kIntervals, "0 4 7 10"}}},
//   };
//
// Name, category ("" for none), then parameter values in plain units, as
// Processor::update reads them (Hz, dB, a percent as 0..1, a choice or note
// value by index: noteValueOf("1/8")), and property values. Everything a
// preset leaves out takes its default, except perProject() properties, which
// no preset touches. The name is the preset's identity for hosts that list
// it (CLAP preset discovery), so it is checked unique, and renaming a shipped
// preset breaks a host's reference to it.
//
// The table is checked when it compiles: an unknown ID, an output
// parameter, a value outside its range, or a perProject() property is a build
// error. User presets (saved from the editor) need no table.
#pragma once

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iterator>

#include "ProductInfo.h"
#include "core/ParamTable.h"
#include "core/PropertyTable.h"

namespace supple {

struct PresetValue {
    uint32_t id;
    double plain;
};

struct PresetText {
    uint32_t id;
    const char* value;
};

// A preset's values or texts, copied in from the braces that list them. Not
// a std::initializer_list kept in the table: MSVC compiles such a member of
// a constexpr array to point at storage that is gone when the program runs.
template <class T, size_t N>
class PresetList {
  public:
    constexpr PresetList() = default;
    constexpr PresetList(std::initializer_list<T> items) {
        for (const T& item : items) {
            if (size_ == N) throw "a factory preset lists more values or texts than the product has";
            items_[size_++] = item;
        }
    }
    constexpr const T* begin() const { return items_; }
    constexpr const T* end() const { return items_ + size_; }
    constexpr size_t size() const { return size_; }

  private:
    T items_[N > 0 ? N : 1]{};
    size_t size_ = 0;
};

struct Preset {
    const char* name;
    const char* category;
    PresetList<PresetValue, params::kCount> values;
    PresetList<PresetText, properties::kCount> texts = PresetList<PresetText, properties::kCount>();
};

}  // namespace supple

#if __has_include("Presets.h")
#include "Presets.h"
#define SUPPLE_HAS_FACTORY_PRESETS 1
#else
#define SUPPLE_HAS_FACTORY_PRESETS 0
#endif

namespace supple::presets {

inline constexpr bool kEnabled = product::info::kPresets;

#if SUPPLE_HAS_FACTORY_PRESETS
static_assert(kEnabled, "product/Presets.h needs PRODUCT_PRESETS ON in product.cmake");
inline constexpr size_t kFactoryCount = std::size(product::kPresets);
inline constexpr const Preset& factory(size_t index) { return product::kPresets[index]; }
#else
inline constexpr size_t kFactoryCount = 0;
inline constexpr Preset kNone{"", "", {}};
inline constexpr const Preset& factory(size_t) { return kNone; }
#endif

namespace detail {

constexpr bool sameString(const char* a, const char* b) {
    while (*a && *a == *b) {
        ++a;
        ++b;
    }
    return *a == *b;
}

constexpr size_t length(const char* s) {
    size_t n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

constexpr bool namesValid() {
    for (size_t i = 0; i < kFactoryCount; ++i) {
        const char* name = factory(i).name;
        if (!name || length(name) == 0 || length(name) > 64) return false;
        for (size_t j = i + 1; j < kFactoryCount; ++j)
            if (sameString(name, factory(j).name)) return false;
    }
    return true;
}

constexpr bool valuesValid() {
    for (size_t i = 0; i < kFactoryCount; ++i) {
        for (const PresetValue& v : factory(i).values) {
            // By index: comparing a pointer with null is not a constant
            // expression under some sanitizers.
            const int k = params::indexOf(v.id);
            if (k < 0) return false;
            const ParamDef& p = params::at(static_cast<size_t>(k));
            if (p.isOutput() || !(v.plain >= p.min && v.plain <= p.max)) return false;
            if (p.stepped() && v.plain != static_cast<double>(static_cast<long long>(v.plain))) return false;
        }
    }
    return true;
}

constexpr bool textsValid() {
    for (size_t i = 0; i < kFactoryCount; ++i) {
        for (const PresetText& t : factory(i).texts) {
            const int k = properties::indexOf(t.id);
            if (k < 0 || properties::at(static_cast<size_t>(k)).perProjectOnly() ||
                length(t.value) > properties::at(static_cast<size_t>(k)).limit)
                return false;
        }
    }
    return true;
}

}  // namespace detail

static_assert(detail::namesValid(), "a factory preset's name is empty, longer than 64 bytes, or not unique");
static_assert(detail::valuesValid(),
              "a factory preset sets an unknown or output parameter, a value outside its range, or a "
              "fraction for a stepped one");
static_assert(detail::textsValid(),
              "a factory preset sets an unknown or perProject() property, or a value longer than its limit");

// Index of the factory preset with this name, or -1.
inline int findFactory(const char* name) {
    if (!name) return -1;
    for (size_t i = 0; i < kFactoryCount; ++i)
        if (detail::sameString(factory(i).name, name)) return static_cast<int>(i);
    return -1;
}

}  // namespace supple::presets
