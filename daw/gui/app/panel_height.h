// How tall a resizable dock (the mixer, the MIDI editor) may be: what its
// parent column has left once the siblings with a height of their own (the
// other docks) and a strip of the shell (transport, a little arrange) are kept.
// Docks re-apply it every sync so a stored preference survives a smaller window
// or a taller neighbour.
#pragma once

#include <algorithm>

#include "supple/ui/node.h"

namespace supple::gui {

inline constexpr float kMinShellHeight = 120;

inline float maxPanelHeight(const ui::Node* panel, float minimum,
                            float minShellHeight = kMinShellHeight) {
    const ui::Node* parent = panel ? panel->parent() : nullptr;
    if (!parent || parent->bounds().h <= 0) return 4000;
    float others = 0;
    for (const auto& sibling : parent->children())
        if (sibling.get() != panel && sibling->visible &&
            sibling->layout.height.kind != ui::SizeRule::Kind::Fraction)
            others += sibling->bounds().h;
    return std::max(minimum, parent->bounds().h - others - minShellHeight);
}

}  // namespace supple::gui
