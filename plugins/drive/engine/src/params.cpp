// SPDX-License-Identifier: MIT
#include "drive/params.h"
namespace supple::drive {
const patch::FlatPatchType& patchType() {
    static const patch::FlatPatchType type("drive", 1, 0, kParams, false);
    return type;
}
Values defaults() {
    Values v{};
    for (const auto& p : kParams) v[p.id] = p.def;
    return v;
}
std::string describe(const Values& v, bool changedOnly) {
    std::string s;
    constexpr int order[] = {kTone, kDrive, kBias, kCurve, kOversampling, kAutoGain, kMix, kOutput};
    for (int i : order) {
        const auto& p = kParams[i];
        if (changedOnly && v[i] == p.def) continue;
        if (!s.empty()) s += "; ";
        s += p.name;
        s += " ";
        s += pa::format(p, v[i]);
        if (i == kTone) s += "/oct";
    }
    return s.empty() ? "Default saturation" : s;
}
}  // namespace supple::drive
