// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include "screensets.h"

namespace supple::gui {
namespace {

std::string trimmed(const std::string& s) {
    const auto a = s.find_first_not_of(" \t");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t") - a + 1);
}

Error unknown(const std::string& name, const std::vector<std::string>& names) {
    std::string have;
    for (const auto& n : names) have += (have.empty() ? "" : ", ") + n;
    return Error("screenset.unknown", "no screenset \"" + name + "\"", "/name",
                 have.empty() ? "save one first (ui.screenset.save)" : "saved: " + have);
}

}  // namespace

Status Screensets::save(const std::string& name) {
    const std::string n = trimmed(name);
    if (n.empty())
        return Error("screenset.name", "a screenset needs a name", "/name", "e.g. mixing");
    sets_[n] = capture_();
    return {};
}

Status Screensets::load(const std::string& name) const {
    const auto at = sets_.find(trimmed(name));
    if (at == sets_.end()) return unknown(trimmed(name), names());
    apply_(at->second);
    return {};
}

Status Screensets::remove(const std::string& name) {
    if (sets_.erase(trimmed(name)) == 0) return unknown(trimmed(name), names());
    return {};
}

std::vector<std::string> Screensets::names() const {
    std::vector<std::string> out;
    for (const auto& [n, layout] : sets_) out.push_back(n);
    return out;
}

json::Value Screensets::toJson() const {
    json::Value v = json::Value::object();
    for (const auto& [n, layout] : sets_) v[n] = layout;
    return v;
}

void Screensets::fromJson(const json::Value& v) {
    sets_.clear();
    if (!v.isObject()) return;
    for (const auto& m : v.asObject())
        if (m.value.isObject()) {
            auto name = trimmed(m.key);
            if (!name.empty()) sets_.try_emplace(std::move(name), m.value);
        }
}

}  // namespace supple::gui
