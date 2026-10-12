// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "core/Definition.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

#include "core/StateFormat.h"

namespace supple {

Layout PluginInfo::layoutOf(int channels) const {
    for (const BusLayout& l : layouts)
        if (l.channels == channels) return l.layout;
    return channels == 1 ? Layout::Mono : channels == 2 ? Layout::Stereo : Layout::Generic;
}

bool PluginInfo::offers(int channels) const {
    for (const BusLayout& l : layouts)
        if (l.channels == channels) return true;
    return false;
}

// --- parameters ------------------------------------------------------------------

ParamTable::ParamTable(const ParamDef* defs, size_t count) : defs_(defs), count_(count) { index(); }

ParamTable::ParamTable(std::vector<ParamDef> defs, std::shared_ptr<const void> storage)
    : owned_(std::move(defs)), storage_(std::move(storage)) {
    defs_ = owned_.data();
    count_ = owned_.size();
    index();
}

void ParamTable::index() {
    dense_.clear();
    sparse_.clear();
    outputs_.clear();
    smoothed_.clear();
    smoothedSlot_.assign(count_, int16_t{-1});
    uint32_t limit = 0;
    for (size_t i = 0; i < count_; ++i)
        if (defs_[i].id < kDense) limit = std::max(limit, defs_[i].id + 1);
    dense_.assign(limit, -1);
    for (size_t i = 0; i < count_; ++i) {
        const ParamDef& p = defs_[i];
        if (p.id < kDense) {
            if (dense_[p.id] < 0) dense_[p.id] = static_cast<int32_t>(i);
        } else {
            sparse_.emplace_back(p.id, static_cast<int32_t>(i));
        }
        if (p.is(pflag::kBypass) && bypass_ < 0) bypass_ = static_cast<int>(i);
        anyRestart_ = anyRestart_ || p.is(pflag::kRestart);
        anyPolyMod_ = anyPolyMod_ || p.is(pflag::kPolyMod);
        if (p.isOutput()) outputs_.push_back(static_cast<uint16_t>(i));
        if (p.smoothing > 0.0) {
            smoothedSlot_[i] = static_cast<int16_t>(smoothed_.size());
            smoothed_.push_back(static_cast<uint16_t>(i));
        }
    }
    std::sort(sparse_.begin(), sparse_.end());
}

int ParamTable::indexOf(uint32_t id) const {
    if (id < dense_.size()) return dense_[id];
    if (sparse_.empty() || id < kDense) return -1;
    auto it = std::lower_bound(sparse_.begin(), sparse_.end(), std::make_pair(id, int32_t{-1}));
    return it != sparse_.end() && it->first == id ? it->second : -1;
}

std::string ParamTable::problems() const {
    std::string out;
    auto add = [&](const std::string& what) {
        if (!out.empty()) out += "; ";
        out += what;
    };
    if (count_ > 65535) add("more than 65535 parameters");
    std::set<uint32_t> ids;
    std::set<std::string> names;
    int bypasses = 0;
    for (size_t i = 0; i < count_; ++i) {
        const ParamDef& p = defs_[i];
        const std::string name = p.name ? p.name : "";
        const std::string which = name.empty() ? "parameter " + std::to_string(i) : "'" + name + "'";
        if (!ids.insert(p.id).second) add(which + " shares its ID " + std::to_string(p.id));
        if (p.id == 0xFFFFFFFFu) add(which + " has the ID CLAP reserves for none");
        if (name.empty()) add("parameter " + std::to_string(i) + " has no name");
        else if (!names.insert(name).second) add(which + " is named twice");
        const bool finite = std::isfinite(p.min) && std::isfinite(p.max) && std::isfinite(p.def) &&
                            std::isfinite(p.curve) && std::isfinite(p.smoothing);
        if (!finite) add(which + " has a NaN or infinite number");
        else if (!(p.max > p.min) || p.def < p.min || p.def > p.max)
            add(which + ": max must exceed min and the default lie between them");
        if (p.taper == Taper::Log && p.min <= 0.0) add(which + " has a log taper from 0 or below");
        if (p.taper == Taper::Power && p.curve <= 0.0) add(which + " has a power taper with a curve <= 0");
        if (p.kind == ParamKind::Choice && p.labelCount != static_cast<int>(p.max - p.min) + 1)
            add(which + ": a choice's label count does not match its range");
        if (p.kind == ParamKind::Toggle && (p.min != 0.0 || p.max != 1.0)) add(which + ": a toggle is 0..1");
        if (p.stepped() && p.def != std::floor(p.def)) add(which + ": a stepped default must be a whole number");
        if (p.kind == ParamKind::Choice || p.kind == ParamKind::Toggle) {
            for (int k = 0; k < p.labelCount; ++k) {
                const size_t n = p.labels && p.labels[k] ? std::strlen(p.labels[k]) : 0;
                if (n == 0 || n > 31) add(which + ": a label is empty or longer than 31 bytes");
            }
        }
        if (p.is(pflag::kBypass)) {
            ++bypasses;
            if (p.kind != ParamKind::Toggle) add(which + ": the bypass must be a toggle");
        }
        if (p.isOutput() && (p.is(pflag::kBypass) || p.is(pflag::kRestart) || p.smoothing != 0.0 || p.is(pflag::kPolyMod)))
            add(which + ": an output cannot be the bypass, restart, smoothed or modulated per note");
        if (p.is(pflag::kOffAtMin) && p.kind != ParamKind::Continuous && p.kind != ParamKind::Integer)
            add(which + ": offAtMinimum() is for continuous and integer parameters");
        if (p.smoothing < 0.0 || (p.smoothing > 0.0 && p.kind != ParamKind::Continuous))
            add(which + ": only a continuous parameter is smoothed, for a time of 0 or more");
        if (p.unit == Unit::Note && p.kind == ParamKind::Choice) add(which + ": a note is an integer, not a choice");
        if (p.is(pflag::kNormalHost) && (p.kind != ParamKind::Continuous || p.is(pflag::kPlainHost)))
            add(which + ": normalizedHostValue() is for a continuous parameter without plainHostValue()");
    }
    if (bypasses > 1) add("more than one bypass parameter");
    return out;
}

const char* ParamTable::Builder::text(std::string_view text) {
    storage_->strings.emplace_back(text);
    return storage_->strings.back().c_str();
}

const char* const* ParamTable::Builder::labels(const std::vector<std::string>& labels) {
    std::vector<const char*> list;
    for (const std::string& l : labels) list.push_back(text(l));
    storage_->lists.push_back(std::move(list));
    return storage_->lists.back().data();
}

ParamTable::Builder& ParamTable::Builder::add(const ParamDef& def) {
    defs_.push_back(def);
    return *this;
}

ParamTable ParamTable::Builder::build() { return ParamTable(std::move(defs_), std::move(storage_)); }

// --- properties -------------------------------------------------------------------

PropertyList::PropertyList(const PropertyDef* defs, size_t count) : defs_(defs), count_(count) {
    for (size_t i = 0; i < count_; ++i) files_ += defs_[i].isFile() ? 1 : 0;
}

PropertyList::PropertyList(std::vector<PropertyDef> defs, std::shared_ptr<const void> storage)
    : owned_(std::move(defs)), storage_(std::move(storage)) {
    defs_ = owned_.data();
    count_ = owned_.size();
    for (size_t i = 0; i < count_; ++i) files_ += defs_[i].isFile() ? 1 : 0;
}

int PropertyList::indexOf(uint32_t id) const {
    for (size_t i = 0; i < count_; ++i)
        if (defs_[i].id == id) return static_cast<int>(i);
    return -1;
}

std::string PropertyList::problems() const {
    std::string out;
    auto add = [&](const std::string& what) {
        if (!out.empty()) out += "; ";
        out += what;
    };
    std::set<uint32_t> ids;
    std::set<std::string> names;
    for (size_t i = 0; i < count_; ++i) {
        const PropertyDef& p = defs_[i];
        const std::string name = p.name ? p.name : "";
        if (!ids.insert(p.id).second) add("property '" + name + "' shares its ID");
        if (name.empty() || !names.insert(name).second) add("a property's name is missing or used twice");
        if (p.limit == 0 || p.limit > kMaxBytes || !p.def || std::strlen(p.def) > p.limit)
            add("property '" + name + "': its limit is 0 or above 1 MiB, or its default is longer");
    }
    return out;
}

const char* PropertyList::Builder::text(std::string_view text) {
    storage_->emplace_back(text);
    return storage_->back().c_str();
}

PropertyList::Builder& PropertyList::Builder::add(const PropertyDef& def) {
    defs_.push_back(def);
    return *this;
}

PropertyList PropertyList::Builder::build() { return PropertyList(std::move(defs_), std::move(storage_)); }

// --- definitions --------------------------------------------------------------------

Definition::Definition(PluginInfo info, ParamTable params, PropertyList properties,
                       std::vector<FactoryPreset> presets)
    : info_(std::move(info)),
      params_(std::move(params)),
      properties_(std::move(properties)),
      presets_(std::move(presets)) {}

int Definition::findPreset(std::string_view name) const {
    for (size_t i = 0; i < presets_.size(); ++i)
        if (presets_[i].name == name) return static_cast<int>(i);
    return -1;
}

std::string Definition::problems() const {
    std::string out = params_.problems();
    auto add = [&](const std::string& what) {
        if (!out.empty()) out += "; ";
        out += what;
    };
    if (const std::string p = properties_.problems(); !p.empty()) add(p);
    if (out.empty() && state::maxSavedBytes(*this) > state::kMaxBytes)
        add("the properties' limits add up to more state than will load back (64 MiB)");
    if (info_.id.empty()) add("the plugin has no ID");
    if (info_.name.empty()) add("the plugin has no name");
    if (info_.stateTag.size() != 4) add("the state tag is not four characters");
    if (!info_.noteEffect() && info_.layouts.empty()) add("an effect or instrument needs a bus layout");
    std::set<int> widths;
    for (const BusLayout& l : info_.layouts)
        if (l.channels < 1 || l.channels > 64 || !widths.insert(l.channels).second)
            add("a layout's width is outside 1..64 or declared twice");
    if (params_.anyPolyMod() && !info_.takesNotes()) add("a polyModulatable() parameter needs note input");
    if (info_.tuning && !info_.takesNotes()) add("tuning needs note input");
    if (!presets_.empty() && !info_.presets) add("factory presets need presets switched on");
    std::set<std::string> presetNames;
    for (const FactoryPreset& preset : presets_) {
        const std::string which = "factory preset '" + preset.name + "'";
        if (preset.name.empty() || preset.name.size() > 64 || !presetNames.insert(preset.name).second)
            add("a factory preset's name is empty, longer than 64 bytes, or not unique");
        for (const auto& [id, plain] : preset.values) {
            const ParamDef* p = params_.find(id);
            if (!p || p->isOutput() || !(plain >= p->min && plain <= p->max) ||
                (p->stepped() && plain != std::floor(plain)))
                add(which + " sets an unknown or output parameter, a value outside its range, or a fraction for a "
                            "stepped one");
        }
        for (const auto& [id, text] : preset.texts) {
            const int k = properties_.indexOf(id);
            if (k < 0 || properties_.at(static_cast<size_t>(k)).perProjectOnly() ||
                !properties_.accepts(static_cast<size_t>(k), text))
                add(which + " sets an unknown or perProject() property, or a value longer than its limit");
        }
    }
    return out;
}

}  // namespace supple
