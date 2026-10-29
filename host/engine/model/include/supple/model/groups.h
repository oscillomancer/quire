// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (c) 2026 Supple Audio
#pragma once
#include "supple/model/session.h"

namespace supple::model {
// Model-thread helpers. Results are unique, sorted IDs; linked results include source.
std::vector<Id> linkedTracks(const Session&, Id track, std::string_view attribute);
std::vector<Id> linkedItems(const Session&, Id item);
std::vector<Id> trackGroups(const Session&, Id track);
// All unique controlling VCAs, excluding target itself for a group target.
std::vector<Id> vcaControllers(const Session&, Id target);
Status validateGroups(const Session&);
// Model curve value at project seconds, including automation items.
double groupParameterAt(const Session&, Id object, std::string_view param, double seconds,
                        double fallback);
struct EffectiveLevel {
    double requestedDb = 0, appliedDb = 0;
    bool mute = false, soloSilenced = false;
};
EffectiveLevel effectiveTrackLevel(const Session&, Id track, double seconds);
}  // namespace supple::model
