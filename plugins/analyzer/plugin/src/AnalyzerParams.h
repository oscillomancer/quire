// SPDX-License-Identifier: MIT
#pragma once
#include "analyzer/params.h"
namespace analyzer {
using namespace supple::analyzer;
using ParamDef = supple::params::ParamSpec;
namespace pa = supple::params;
namespace un = supple::units;
inline auto kParams = table();
inline const ParamDef* findParam(uint32_t id) { return pa::findById(table(), id); }
inline double toNormalised(const ParamDef& p, double v) { return pa::toNormalized(p, v); }
inline double fromNormalised(const ParamDef& p, double v) { return pa::fromNormalized(p, v); }
}  // namespace analyzer
