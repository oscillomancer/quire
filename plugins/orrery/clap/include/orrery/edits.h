// Structural edits as semantic merge patches, for editors: each function
// returns an RFC 7396 merge patch over the current patch (EditorBridge::merge)
// that keeps the patch valid — removing a slot also removes the routes that
// touch it, removing filter2 retargets the oscillators that fed it. The codec
// still validates every result.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "orrery/patch.h"
#include "supple/json/json.h"

namespace supple::orrery::edits {

// The slot a modulation source belongs to (nullopt for performance sources:
// velocity, key, modwheel, ...).
std::optional<Slot> sourceSlot(Source s);
// The slot a route destination lives in.
Slot destSlot(const Dest& d);
std::string routeKey(const Route& r);  // "env2>filter1.cutoff"

// Slots an editor can add or remove (voice, amp, macros and output always exist).
bool removable(Slot s);
json::Value addSlot(const Structure& st, Slot s);  // oscillators: va, filters: svf, drive: soft
json::Value removeSlot(const Structure& st, Slot s);
// Type index: OscType for oscillators, FilterType for filters, DriveType for drive.
// Routes to parameters the new type lacks are removed with it.
json::Value setType(const Structure& st, Slot s, int type);
json::Value setOscTarget(int osc, OscTarget t);
json::Value setRouting(FilterRouting r);
json::Value setQuality(Quality q);

json::Value addFx(const Structure& st, FxKind k);
json::Value removeFx(const Structure& st, std::size_t position);
json::Value moveFx(const Structure& st, std::size_t from, std::size_t to);

// Adds the route or replaces the one with the same source and destination.
json::Value setRoute(const Route& r);
json::Value removeRoute(const Route& r);

// Sources the patch can route from now, and destinations `source` may reach
// (existing slots, active modulatable parameters, scope rules applied).
std::vector<Source> availableSources(const Structure& st);
std::vector<Dest> availableDests(const Structure& st, Source source);
// A sensible starting amount for a new route, in modUnit(dest).
double defaultAmount(const Dest& d);

}  // namespace supple::orrery::edits
