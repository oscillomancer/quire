// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// Binds the framework to the product's parameter table (product/Params.h) and
// checks that table at compile time. A duplicated ID, a default outside its
// range, a NaN or infinite bound or a log taper that starts at zero is a
// build error, not a session that misbehaves in a host months later.
//
// Parameter IDs are session ABI and need not be dense: a product may reserve
// ranges (globals 0..63, band b at 64 + 32 * b, ...). Storage everywhere is by
// table index; indexOf() maps an ID to it in constant time.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>

#include "core/Param.h"
#include "Params.h"

namespace supple::params {

inline constexpr size_t kCount = std::size(product::kParams);
static_assert(kCount > 0, "product::kParams is empty");

inline constexpr const ParamDef& at(size_t index) { return product::kParams[index]; }

namespace detail {

constexpr uint32_t idLimit() {
    uint32_t limit = 0;
    for (const auto& p : product::kParams)
        if (p.id + 1 > limit) limit = p.id + 1;
    return limit;
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

// Not NaN and not infinite, by comparison alone (std::isfinite is not
// constexpr before C++23).
constexpr bool finite(double v) {
    return v >= -std::numeric_limits<double>::max() && v <= std::numeric_limits<double>::max();
}

template <class Table>
constexpr bool rangesValid(const Table& table) {
    for (const auto& p : table) {
        if (!finite(p.min) || !finite(p.max) || !finite(p.def) || !finite(p.curve) || !finite(p.smoothing))
            return false;
        if (!(p.max > p.min) || p.def < p.min || p.def > p.max) return false;
        if (p.taper == Taper::Log && p.min <= 0.0) return false;
        if (p.taper == Taper::Power && p.curve <= 0.0) return false;
    }
    return true;
}

constexpr bool labelsValid() {
    for (const auto& p : product::kParams) {
        // The factories set labels and labelCount together. (Testing the
        // pointer for null is not a constant expression under some
        // sanitizers, so the count stands in for it.)
        if (p.kind == ParamKind::Choice && p.labelCount != static_cast<int>(p.max - p.min) + 1) return false;
        if (p.kind == ParamKind::Toggle && (p.min != 0.0 || p.max != 1.0)) return false;
        if (p.stepped() && p.def != static_cast<double>(static_cast<long long>(p.def))) return false;
    }
    return true;
}

// Labels are text the host shows, parses back and stores nowhere: they must
// fit ValueText::number with its terminator, or the text would not round-trip.
constexpr bool labelLengthsValid() {
    for (const auto& p : product::kParams) {
        if (p.kind != ParamKind::Choice && p.kind != ParamKind::Toggle) continue;
        for (int i = 0; i < p.labelCount; ++i) {
            int n = 0;
            while (p.labels[i][n] != '\0') ++n;
            if (n == 0 || n > 31) return false;
        }
    }
    return true;
}

constexpr int bypassCount() {
    int n = 0;
    for (const auto& p : product::kParams)
        if (p.is(pflag::kBypass)) ++n;
    return n;
}

constexpr bool bypassValid() {
    for (const auto& p : product::kParams)
        if (p.is(pflag::kBypass) && p.kind != ParamKind::Toggle) return false;
    return bypassCount() <= 1;
}

// An output is reported, not set: nothing that acts on a host's value applies.
constexpr bool outputsValid() {
    for (const auto& p : product::kParams)
        if (p.isOutput() && (p.is(pflag::kBypass) || p.is(pflag::kRestart) || p.smoothing != 0.0 ||
                             p.is(pflag::kPolyMod)))
            return false;
    return true;
}

constexpr bool modifiersValid() {
    for (const auto& p : product::kParams) {
        if (p.is(pflag::kOffAtMin) && p.kind != ParamKind::Continuous && p.kind != ParamKind::Integer) return false;
        if (p.smoothing < 0.0 || (p.smoothing > 0.0 && p.kind != ParamKind::Continuous)) return false;
        if (p.unit == Unit::Note && p.kind == ParamKind::Choice) return false;
        if (p.is(pflag::kNormalHost) && (p.kind != ParamKind::Continuous || p.is(pflag::kPlainHost))) return false;
    }
    return true;
}

}  // namespace detail

static_assert(detail::idsUnique(), "two parameters share an ID");
static_assert(detail::namesUnique(), "parameter names must be present and unique");
static_assert(detail::rangesValid(product::kParams),
              "a parameter has a NaN or infinite number, max <= min, a default outside its range, a log "
              "taper with min <= 0, or a power taper with curve <= 0");
static_assert(detail::labelsValid(),
              "a choice's label count does not match its range, a toggle is not 0..1, or a "
              "stepped default is not a whole number");
static_assert(detail::labelLengthsValid(), "a choice or toggle label is empty or longer than 31 bytes");
static_assert(detail::bypassValid(), "at most one bypass parameter, and it must be a toggle");
static_assert(detail::outputsValid(),
              "an output parameter cannot be the bypass, restart, smoothed or modulated per note");
static_assert(detail::modifiersValid(),
              "offAtMinimum() and smoothed() are for continuous parameters (offAtMinimum also integers), "
              "a smoothing time cannot be negative, and normalizedHostValue() is for a continuous "
              "parameter without plainHostValue()");

inline constexpr uint32_t kIdLimit = detail::idLimit();
static_assert(kIdLimit <= 65536, "parameter IDs above 65535: use a denser ID scheme");

namespace detail {
constexpr std::array<int32_t, kIdLimit> buildIndex() {
    std::array<int32_t, kIdLimit> index{};
    for (auto& slot : index) slot = -1;
    for (size_t i = 0; i < kCount; ++i) index[at(i).id] = static_cast<int32_t>(i);
    return index;
}
constexpr int findBypass() {
    for (size_t i = 0; i < kCount; ++i)
        if (at(i).is(pflag::kBypass)) return static_cast<int>(i);
    return -1;
}
constexpr bool anyFlag(uint32_t flag) {
    for (const auto& p : product::kParams)
        if (p.is(flag)) return true;
    return false;
}
constexpr size_t countOutputs() {
    size_t n = 0;
    for (const auto& p : product::kParams) n += p.isOutput() ? 1 : 0;
    return n;
}
constexpr size_t countSmoothed() {
    size_t n = 0;
    for (const auto& p : product::kParams) n += p.smoothing > 0.0 ? 1 : 0;
    return n;
}
}  // namespace detail

inline constexpr std::array<int32_t, kIdLimit> kIndexOfId = detail::buildIndex();

// Table index of the parameter with this ID, or -1.
inline constexpr int indexOf(uint32_t id) { return id < kIdLimit ? kIndexOfId[id] : -1; }

inline constexpr const ParamDef* find(uint32_t id) {
    const int i = indexOf(id);
    return i >= 0 ? &product::kParams[i] : nullptr;
}

// Table index of the bypass parameter, or -1 when the product has none.
inline constexpr int kBypassIndex = detail::findBypass();
inline constexpr bool kAnyRestart = detail::anyFlag(pflag::kRestart);
inline constexpr bool kAnyPolyMod = detail::anyFlag(pflag::kPolyMod);

// Output parameters, by table index.
inline constexpr size_t kOutputCount = detail::countOutputs();
inline constexpr std::array<uint16_t, kOutputCount> kOutputIndices = [] {
    std::array<uint16_t, kOutputCount> out{};
    size_t n = 0;
    for (size_t i = 0; i < kCount; ++i)
        if (at(i).isOutput()) out[n++] = static_cast<uint16_t>(i);
    return out;
}();

// Parameters the framework smooths: their table indices, and for each table
// index its place among them (-1 when not smoothed).
inline constexpr size_t kSmoothedCount = detail::countSmoothed();
inline constexpr std::array<uint16_t, kSmoothedCount> kSmoothedIndices = [] {
    std::array<uint16_t, kSmoothedCount> out{};
    size_t n = 0;
    for (size_t i = 0; i < kCount; ++i)
        if (at(i).smoothing > 0.0) out[n++] = static_cast<uint16_t>(i);
    return out;
}();
inline constexpr std::array<int16_t, kCount> kSmoothedSlot = [] {
    std::array<int16_t, kCount> out{};
    int16_t n = 0;
    for (size_t i = 0; i < kCount; ++i) out[i] = at(i).smoothing > 0.0 ? n++ : int16_t{-1};
    return out;
}();

}  // namespace supple::params
