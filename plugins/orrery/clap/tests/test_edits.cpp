// Structural edit helpers (orrery/edits.h): every edit they offer must pass
// the codec, and cascades (routes, oscillator targets) must keep patches valid.
#include <doctest/doctest.h>

#include <string>

#include "orrery/edits.h"

using namespace supple;
using namespace supple::orrery;

namespace {

const PatchType& type() {
    static const PatchType t;
    return t;
}

Patch parse(std::string_view body) {
    auto p = type().parse(std::string(R"({"schema": "supple.orrery-patch/1.0", )") +
                          std::string(body) + "}");
    REQUIRE_MESSAGE(p, (p ? "" : p.error().toString()));
    return *p;
}

Patch apply(const Patch& p, const json::Value& edit) {
    auto r = type().merge(p, edit);
    REQUIRE_MESSAGE(r, (r ? "" : r.error().toString() + "\nedit: " + json::writeCompact(edit)));
    return *r;
}

constexpr std::string_view kFull = R"(
    "oscillators": {"osc1": {"type": "va", "wave": "pulse", "width": 30, "to": "filter1"},
                    "osc2": {"type": "fm", "ratio": 2, "to": "none"}},
    "filters": {"routing": "serial", "filter1": {"type": "ladder", "slope": "24 dB"},
                "filter2": {"type": "comb", "feedback": 40}},
    "drive": {"type": "tape", "amount": 6},
    "envelopes": {"env2": {"decay": 300}},
    "lfos": {"lfo1": {"rate": 2}},
    "steps": {"step1": {"values": [100, 0, -50]}},
    "routes": {"osc2>osc1.fm": {"amount": 2}, "lfo1>osc2.pitch": {"amount": 1},
               "env2>filter2.cutoff": {"amount": 1}, "step1>osc2.level": {"amount": -6},
               "env2>osc1.width": {"amount": 20, "via": "lfo1"}},
    "fx": {"chain": ["chorus", "reverb"]})";

}  // namespace

TEST_CASE("adding and removing every removable slot keeps the patch valid") {
    for (int s = 0; s < kSlotCount; ++s) {
        const auto slot = static_cast<Slot>(s);
        if (!edits::removable(slot) || slotInfo(slot).block >= Block::Chorus) continue;
        INFO(slotInfo(slot).name);
        Patch p = parse(kFull);
        if (p.body.structure.has(slot)) {
            p = apply(p, edits::removeSlot(p.body.structure, slot));
            CHECK_FALSE(p.body.structure.has(slot));
            for (const Route& r : p.body.structure.routes) CHECK(edits::destSlot(r.dest) != slot);
        }
        p = apply(p, edits::addSlot(p.body.structure, slot));
        CHECK(p.body.structure.has(slot));
    }
}

TEST_CASE("removal cascades: routes through the slot go, oscillators move off a removed filter") {
    Patch p = parse(kFull);
    Patch q = apply(p, edits::removeSlot(p.body.structure, kOsc2));
    CHECK(q.body.structure.routes.size() == 2);  // env2>filter2.cutoff, env2>osc1.width via lfo1
    q = apply(p, edits::removeSlot(p.body.structure, kLfo1));
    CHECK(q.body.structure.routes.size() == 3);  // the lfo1 route and the lfo1 "via" go
    q = apply(p, edits::removeSlot(p.body.structure, kFilter1));
    CHECK(q.body.structure.to[0] == OscTarget::Filter2);
    q = apply(q, edits::removeSlot(q.body.structure, kFilter2));
    CHECK(q.body.structure.to[0] == OscTarget::Direct);
    CHECK_FALSE(q.body.structure.filter[0].has_value());
    CHECK_FALSE(q.body.structure.filter[1].has_value());
}

TEST_CASE("every type change applies") {
    const Patch p = parse(kFull);
    for (int t = 0; t < kOscTypeCount; ++t) {
        const Patch q = apply(p, edits::setType(p.body.structure, kOsc1, t));
        CHECK(q.body.structure.osc[0] == static_cast<OscType>(t));
    }
    for (int t = 0; t < kFilterTypeCount; ++t) {
        const Patch q = apply(p, edits::setType(p.body.structure, kFilter1, t));
        CHECK(q.body.structure.filter[0] == static_cast<FilterType>(t));
    }
    for (int t = 0; t < 5; ++t)
        CHECK(apply(p, edits::setType(p.body.structure, kDrive, t)).body.structure.drive ==
              static_cast<DriveType>(t));
    CHECK(apply(p, edits::setQuality(Quality::High)).body.structure.quality == Quality::High);
    CHECK(apply(p, edits::setRouting(FilterRouting::Parallel)).body.structure.routing ==
          FilterRouting::Parallel);
    CHECK(apply(p, edits::setOscTarget(1, OscTarget::Both)).body.structure.to[1] ==
          OscTarget::Both);
}

TEST_CASE("fx chain edits") {
    Patch p = parse(kFull);
    p = apply(p, edits::addFx(p.body.structure, FxKind::Delay));
    REQUIRE(p.body.structure.chain.size() == 3);
    p = apply(p, edits::moveFx(p.body.structure, 2, 0));
    CHECK(p.body.structure.chain.front() == FxKind::Delay);
    p = apply(p, edits::removeFx(p.body.structure, 1));
    CHECK(p.body.structure.chain == std::vector<FxKind>{FxKind::Delay, FxKind::Reverb});
    p = apply(p, edits::removeSlot(p.body.structure, kReverb));
    CHECK(p.body.structure.chain == std::vector<FxKind>{FxKind::Delay});
}

TEST_CASE("every route the helpers offer is accepted, changed and removed") {
    const Patch p = parse(kFull);
    const Structure& st = p.body.structure;
    int routes = 0;
    for (Source s : edits::availableSources(st))
        for (const Dest& d : edits::availableDests(st, s)) {
            Route r;
            r.source = s;
            r.dest = d;
            r.amount = edits::defaultAmount(d);
            INFO(edits::routeKey(r));
            const Patch q = apply(p, edits::setRoute(r));
            CHECK(std::find(q.body.structure.routes.begin(), q.body.structure.routes.end(), r) !=
                  q.body.structure.routes.end());
            CHECK(apply(q, edits::removeRoute(r)).body.structure.routes.size() <= st.routes.size());
            ++routes;
        }
    CHECK(routes > 200);
    // Changing an existing route's amount and clearing its via.
    Route r = st.routes.back();
    for (const Route& x : st.routes)
        if (x.via) r = x;
    r.via.reset();
    r.amount = 10;
    const Patch q = apply(p, edits::setRoute(r));
    CHECK(std::find(q.body.structure.routes.begin(), q.body.structure.routes.end(), r) !=
          q.body.structure.routes.end());
}
