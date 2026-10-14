// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Render identifiers use rd_<digits>; other strings name audio files.
#pragma once

#include <algorithm>
#include <string_view>

namespace supple::sandbox {

inline bool isRenderRef(std::string_view source) {
    if (!source.starts_with("rd_") || source.size() == 3) return false;
    return std::all_of(source.begin() + 3, source.end(),
                       [](char c) { return c >= '0' && c <= '9'; });
}

}  // namespace supple::sandbox
