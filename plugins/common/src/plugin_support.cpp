// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/clap/plugin_support.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "supple/json/json.h"

namespace supple::clapx {

namespace {

json::Value errorJson(const Error& e) {
    json::Value v = json::Value::object();
    v["code"] = e.code;
    v["message"] = e.message;
    v["path"] = e.path;
    v["hint"] = e.hint;
    if (!e.details.empty()) {
        json::Value d = json::Value::array();
        for (const auto& x : e.details) d.push_back(errorJson(x));
        v["details"] = std::move(d);
    }
    return v;
}

void copyName(char* dst, std::size_t size, std::string_view src) {
    const std::size_t n = std::min(size - 1, src.size());
    if (n) std::memcpy(dst, src.data(), n);  // an empty string_view may have a null data()
    dst[n] = 0;
}

}  // namespace

std::string errorsToJson(const Error& error) {
    json::Value arr = json::Value::array();
    if (error.code == "schema.invalid" && !error.details.empty()) {
        for (const auto& d : error.details) arr.push_back(errorJson(d));
    } else {
        arr.push_back(errorJson(error));
    }
    return json::writeCompact(arr);
}

// --- ParamStore --------------------------------------------------------------------

ParamStore::ParamStore(params::Table table)
    : table_(table), values_(std::make_unique<std::atomic<double>[]>(table.size())) {
    resetToDefaults();
}

double ParamStore::get(std::size_t index) const SUPPLE_RT {
    return values_[index].load(std::memory_order_relaxed);
}

void ParamStore::set(std::size_t index, double physical) SUPPLE_RT {
    values_[index].store(params::clampPhysical(table_[index], physical), std::memory_order_relaxed);
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

bool ParamStore::getClap(clap_id id, double& clapValue) const SUPPLE_RT {
    for (std::size_t i = 0; i < table_.size(); ++i)
        if (table_[i].id == id) {
            clapValue = params::toClap(table_[i], get(i));
            return true;
        }
    return false;
}

bool ParamStore::setClap(clap_id id, double clapValue) SUPPLE_RT {
    for (std::size_t i = 0; i < table_.size(); ++i)
        if (table_[i].id == id) {
            if (table_[i].flags & params::kReadonly) return false;
            set(i, params::fromClap(table_[i], clapValue));
            return true;
        }
    return false;
}

void ParamStore::resetToDefaults() {
    patch::FlatBody b;
    b.values.resize(table_.size());
    for (std::size_t i = 0; i < table_.size(); ++i) b.values[i] = table_[i].def;
    const auto s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i < table_.size(); ++i)
        values_[i].store(b.values[i], std::memory_order_relaxed);  // readonly too: defaults
    seq_.store(s + 2, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

bool ParamStore::readAll(std::span<double> out) const SUPPLE_RT {
    // Seqlock reader (Boehm, "Can seqlocks get along with programming
    // language memory models?", 2012): relaxed data loads bracketed by an
    // acquire load and an acquire fence.
    const auto s1 = seq_.load(std::memory_order_acquire);
    if (s1 & 1) return false;
    const std::size_t n = std::min(out.size(), table_.size());
    for (std::size_t i = 0; i < n; ++i) out[i] = values_[i].load(std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_acquire);
    return seq_.load(std::memory_order_relaxed) == s1;
}

patch::FlatBody ParamStore::snapshot() const {
    patch::FlatBody b;
    b.values.resize(table_.size());
    for (std::size_t i = 0; i < table_.size(); ++i) b.values[i] = get(i);
    return b;
}

void ParamStore::assign(const patch::FlatBody& body) {
    // Seqlock writer (single writer: the main thread).
    const auto s = seq_.load(std::memory_order_relaxed);
    seq_.store(s + 1, std::memory_order_relaxed);
    std::atomic_thread_fence(std::memory_order_release);
    for (std::size_t i = 0; i < table_.size() && i < body.values.size(); ++i) {
        if (table_[i].flags & params::kReadonly) continue;
        values_[i].store(params::clampPhysical(table_[i], body.values[i]),
                         std::memory_order_relaxed);
    }
    seq_.store(s + 2, std::memory_order_release);
    generation_.fetch_add(1, std::memory_order_acq_rel);
}

// --- ClapParams --------------------------------------------------------------------

bool ClapParams::info(params::Table t, std::uint32_t index, clap_param_info_t* out) {
    if (index >= t.size()) return false;
    const auto& p = t[index];
    std::memset(out, 0, sizeof(*out));
    out->id = p.id;
    out->flags = params::clapFlags(p);
    out->cookie = nullptr;
    copyName(out->name, sizeof(out->name), p.name);
    copyName(out->module, sizeof(out->module), p.group);
    const auto r = params::clapRange(p);
    out->min_value = r.min;
    out->max_value = r.max;
    out->default_value = r.def;
    return true;
}

bool ClapParams::valueToText(params::Table t, clap_id id, double clapValue, char* out,
                             std::uint32_t size) {
    const auto* p = params::findById(t, id);
    if (!p || size == 0) return false;
    std::string display = params::format(*p, params::fromClap(*p, clapValue));
    // Unit formatting can round across a prefix boundary (for example,
    // 999.6 Hz -> "1.00 kHz"). Canonicalize the displayed value so a host's
    // text -> value -> text round trip keeps the same string.
    if (auto parsed = params::parse(*p, display)) display = params::format(*p, *parsed);
    copyName(out, size, display);
    return true;
}

bool ClapParams::textToValue(params::Table t, clap_id id, const char* text, double* clapValue) {
    const auto* p = params::findById(t, id);
    if (!p || !text) return false;
    auto v = params::parse(*p, text);
    if (!v) return false;
    *clapValue = params::toClap(*p, *v);
    return true;
}

bool ClapParams::applyEvents(ParamStore& store, const clap_input_events_t* in) SUPPLE_RT {
    bool changed = false;
    const uint32_t n = in ? in->size(in) : 0;
    for (uint32_t i = 0; i < n; ++i) {
        const clap_event_header_t* h = in->get(in, i);
        if (!h || h->size < sizeof(clap_event_param_value_t) ||
            h->space_id != CLAP_CORE_EVENT_SPACE_ID || h->type != CLAP_EVENT_PARAM_VALUE)
            continue;
        const auto* e = reinterpret_cast<const clap_event_param_value_t*>(h);
        changed = store.setClap(e->param_id, e->value) || changed;
    }
    return changed;
}

}  // namespace supple::clapx
