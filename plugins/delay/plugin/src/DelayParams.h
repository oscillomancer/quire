// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include "delay/params.h"
namespace supple::delay {
using ParamDef = params::ParamSpec;
inline const auto kParams = table();
inline constexpr int kParamCount = kNumParams;
inline const ParamDef* findParam(uint32_t id) { return params::findById(kParams, id); }
inline double toNormalised(const ParamDef& p, double v) { return params::toNormalized(p, v); }
inline double fromNormalised(const ParamDef& p, double v) { return params::fromNormalized(p, v); }
inline void formatDelayact(uint32_t id, double v, char* out, size_t n) {
    if (const auto* p = findParam(id); p && n)
        std::snprintf(out, n, "%s", params::formatCompact(*p, v).c_str());
}
}  // namespace supple::delay
