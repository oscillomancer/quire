#pragma once
#include "eq/params.h"
namespace eq {
using ParamDef = supple::params::ParamSpec;
inline const auto kParams = supple::eq::table();
inline constexpr int kNumParams = static_cast<int>(supple::eq::kParamCount),
                     kParamCount = kNumParams;
inline const ParamDef* findParam(uint32_t id) { return supple::params::findById(kParams, id); }
inline double toNormalised(const ParamDef& p, double v) {
    return supple::params::toNormalized(p, v);
}
inline double fromNormalised(const ParamDef& p, double v) {
    return supple::params::fromNormalized(p, v);
}
}  // namespace eq
