#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

#include "supple/base/result.h"

namespace supple::engine::detail {
// All MIDI edit tick additions/subtractions pass here before changing a note.
inline Result<std::int64_t> addMidiTicks(std::int64_t a, std::int64_t b, std::string path) {
    constexpr auto max = std::numeric_limits<std::int64_t>::max();
    constexpr auto min = std::numeric_limits<std::int64_t>::min();
    if ((b > 0 && a > max - b) || (b < 0 && a < min - b))
        return Error("midi.range", "MIDI tick arithmetic would overflow", std::move(path),
                     "reduce the beat position, length or delta so the resulting MIDI ticks fit");
    return a + b;
}
}  // namespace supple::engine::detail
