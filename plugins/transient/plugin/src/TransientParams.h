// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include "transient/params.h"
namespace supple::transient {
using ParamDef = params::ParamSpec;
inline const auto kParams = table();
inline constexpr int kParamCount = kNumParams;
inline const ParamDef* findParam(uint32_t id) { return params::findById(kParams, id); }
inline double toNormalised(const ParamDef& p, double v) { return params::toNormalized(p, v); }
inline double fromNormalised(const ParamDef& p, double v) { return params::fromNormalized(p, v); }
inline void formatCompact(uint32_t id, double v, char* out, size_t n) {
    if (const auto* p = findParam(id); p && n)
        std::snprintf(out, n, "%s", formatValue(id, v, true).c_str());
}
}  // namespace supple::transient
