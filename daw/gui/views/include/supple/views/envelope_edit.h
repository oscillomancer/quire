// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
// Edits of an envelope's selected points (docs/specs/gui.md §4.3): the
// selection is the points themselves (position and value), so it survives the
// envelope being rewritten and drops points another edit removed. Each
// function returns the envelope's new points and the new selection; the
// arrange view commits them as one env.points.set.
#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "supple/model/objects.h"
#include "supple/views/envelope_lane.h"

namespace supple::views {

struct EnvEdit {
    std::vector<model::EnvPoint> points;    // the envelope's points after the edit
    std::vector<model::EnvPoint> selected;  // the selection after it
};

// The same point (a selection entry still names it).
bool samePoint(const model::EnvPoint& a, const model::EnvPoint& b);
// Indices of `points` that `selected` names.
std::vector<std::size_t> selectedIndices(const std::vector<model::EnvPoint>& points,
                                         const std::vector<model::EnvPoint>& selected);

// Points copied from an envelope: positions from the first one, values in
// the source's units and on its lane (0..1), for pasting into another kind.
struct EnvClip {
    std::vector<model::EnvPoint> points;
    std::vector<double> norms;
    std::string unit;  // which envelopes take the values as they are (envUnit)
    bool empty() const { return points.empty(); }
};
// The kind of value an envelope holds: volume, pan, mute, playrate or a plug-in
// parameter's key. Points paste between envelopes of one unit unchanged.
std::string envUnit(const model::Envelope& env, const EnvScale& scale);
EnvClip copyPoints(const std::vector<model::EnvPoint>& points,
                   const std::vector<model::EnvPoint>& selected, const EnvScale& scale,
                   std::string unit);

// The selected points removed.
EnvEdit deletePoints(const std::vector<model::EnvPoint>& points,
                     const std::vector<model::EnvPoint>& selected);
// The selected points moved by `beats` (none before 0) and `norm` (on the
// lane, each clamped to it), kept in time order.
EnvEdit movePoints(const std::vector<model::EnvPoint>& points,
                   const std::vector<model::EnvPoint>& selected, double beats, double norm,
                   const EnvScale& scale);
// The selected points given `shape` (the segment each one starts).
EnvEdit shapePoints(const std::vector<model::EnvPoint>& points,
                    const std::vector<model::EnvPoint>& selected, model::PointShape shape);
// The clip's points at `atBeat`, replacing the points over its span; they
// become the selection. Values convert through the lane when the units differ.
EnvEdit pastePoints(const std::vector<model::EnvPoint>& points, const EnvClip& clip, double atBeat,
                    const EnvScale& scale, const std::string& unit);

}  // namespace supple::views
