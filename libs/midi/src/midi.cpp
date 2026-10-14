// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#include <supple/midi/midi.h>

#include <algorithm>
#include <tuple>

namespace supple::midi {

void Sequence::canonicalize() {
    std::stable_sort(notes.begin(), notes.end(), [](const Note& a, const Note& b) {
        return std::tie(a.start, a.pitch, a.channel, a.length, a.velocity, a.offVelocity) <
               std::tie(b.start, b.pitch, b.channel, b.length, b.velocity, b.offVelocity);
    });
    notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
    std::stable_sort(controls.begin(), controls.end(), [](const Control& a, const Control& b) {
        return std::tie(a.tick, a.kind, a.channel, a.number) <
               std::tie(b.tick, b.kind, b.channel, b.number);
    });
    // Events at the same time and address retain their order: last wins.
    controls.erase(std::unique(controls.begin(), controls.end()), controls.end());
    std::stable_sort(sysex.begin(), sysex.end(),
                     [](const Sysex& a, const Sysex& b) { return a.tick < b.tick; });
    sysex.erase(std::unique(sysex.begin(), sysex.end()), sysex.end());
}

}  // namespace supple::midi
