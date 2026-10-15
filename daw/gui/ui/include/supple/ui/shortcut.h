// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include <string>
namespace supple::ui {
// Display the portable primary modifier in shortcuts and instructional text.
// Stored modifier names and the internal kCtrl bit remain platform independent.
inline std::string shortcutLabel(std::string text) {
#if defined(__APPLE__)
    for (std::size_t at = 0; (at = text.find("Ctrl", at)) != std::string::npos; at += 3)
        text.replace(at, 4, "Cmd");
#endif
    return text;
}
}  // namespace supple::ui
