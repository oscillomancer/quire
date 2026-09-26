#include "supple/base/result.h"

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

}  // namespace supple
