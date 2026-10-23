// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include "supple/base/result.h"

#include <ostream>

namespace supple {

std::string Error::toString() const {
    std::string s = code;
    if (!path.empty()) s += " at " + path;
    if (loc) s += " (" + std::to_string(loc->line) + ":" + std::to_string(loc->column) + ")";
    s += ": " + message;
    if (!hint.empty()) s += " Hint: " + hint;
    for (const Error& d : details) s += "\n  - " + d.toString();
    return s;
}

std::ostream& operator<<(std::ostream& os, const Error& e) { return os << e.toString(); }

std::ostream& operator<<(std::ostream& os, const Status& s) {
    if (s.ok()) return os << "ok";
    return os << s.error();
}

}  // namespace supple
