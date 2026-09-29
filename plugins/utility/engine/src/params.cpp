// SPDX-License-Identifier: MIT
#include "utility/params.h"
namespace supple::utility {
const patch::FlatPatchType& patchType() {
    static const patch::FlatPatchType type("utility", 1, 0, kParams, false);
    return type;
}
Values defaults() {
    Values v{};
    for (const auto& p : kParams) v[p.id] = p.def;
    return v;
}
std::string describe(const Values& v, bool changedOnly) {
    std::string s;
    constexpr int order[] = {kChannels, kSwap,      kInvertLeft, kInvertRight, kDcBlock,
                             kWidth,    kMonoBelow, kPan,        kGain,        kMute};
    for (int i : order) {
        const auto& p = kParams[i];
        if (changedOnly && v[i] == p.def) continue;
        if (!s.empty()) s += "; ";
        s += p.name;
        s += " ";
        s += params::formatCompact(p, v[i]);
    }
    return s.empty() ? "Unity stereo" : s;
}
}  // namespace supple::utility
