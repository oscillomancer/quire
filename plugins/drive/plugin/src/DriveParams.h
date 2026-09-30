// SPDX-License-Identifier: MIT
#pragma once
#include <cstdio>
#include "drive/params.h"
namespace supple::drive {
inline void formatCompact(uint32_t id, double v, char* out, size_t size) {
    if (const auto* p = findParam(id)) {
        auto text = pa::formatCompact(*p, v);
        std::snprintf(out, size, "%s", text.c_str());
    }
}
}  // namespace supple::drive
