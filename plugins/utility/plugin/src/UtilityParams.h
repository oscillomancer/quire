// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include "utility/params.h"
namespace supple::utility {
inline void formatCompact(uint32_t id, double v, char* out, size_t size) {
    if (const auto* p = findParam(id)) {
        auto text = pa::formatCompact(*p, v);
        std::snprintf(out, size, "%s", text.c_str());
    }
}
}  // namespace supple::utility
