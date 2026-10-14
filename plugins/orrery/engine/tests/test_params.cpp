// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <set>
#include <string>

#include "orrery/params.h"

using namespace supple;
using namespace supple::orrery;

TEST_CASE("the Orrery table passes core-params validation") {
    auto st = params::validateTable(table());
    INFO((st ? std::string() : st.error().message + " " + st.error().path));
    CHECK(st);
    CHECK(paramCount() == table().size());
    CHECK(paramCount() > 200);
}

TEST_CASE("ids and keys follow the spec's blocks (synth.md §8)") {
    auto id = [](std::string_view key) {
        const params::ParamSpec* p = params::findByKey(table(), key);
        REQUIRE_MESSAGE(p != nullptr, std::string(key));
        return p->id;
    };
    CHECK(id("voice.mode") == 0);
    CHECK(id("osc1.level") == 100);
    CHECK(id("osc2.level") == 200);
    CHECK(id("osc4.octave") == 418);
    CHECK(id("filters.balance") == 500);
    CHECK(id("filter1.cutoff") == 510);
    CHECK(id("filter2.cutoff") == 610);
    CHECK(id("drive.amount") == 700);
    CHECK(id("amp.level") == 720);
    CHECK(id("amp.attack") == 801);  // the amp envelope
    CHECK(id("env2.attack") == 901);
    CHECK(id("lfo1.rate") == 1201);
    CHECK(id("step2.glide") == 1702);
    CHECK(id("macro1") == 1800);
    CHECK(id("macro8") == 1807);
    CHECK(id("fx.chorus.rate") == 2000);
    CHECK(id("fx.reverb.mix") == 2305);
    CHECK(id("fx.eq.high_gain") == 2404);
    CHECK(id("output.level") == 2500);
}

TEST_CASE("index and locate agree for every parameter") {
    std::set<std::uint32_t> ids;
    for (std::size_t i = 0; i < paramCount(); ++i) {
        const ParamLocation loc = locate(i);
        CHECK(index(loc.slot, loc.field) == i);
        CHECK(table()[i].id == slotInfo(loc.slot).baseId + loc.field);
        CHECK(ids.insert(table()[i].id).second);
        if (slotInfo(loc.slot).block != Block::Macro)
            CHECK(table()[i].key ==
                  std::string(slotInfo(loc.slot).name) + "." +
                      std::string(fieldName(slotInfo(loc.slot).block, loc.field)));
    }
    CHECK(index(osc(2), OscF::fine) + 1 == index(osc(2), OscF::phase_mode));
    CHECK(findField(Block::Filter, "keytrack") == static_cast<std::uint16_t>(FilterF::keytrack));
    CHECK(!findField(Block::Filter, "wave"));
}

TEST_CASE("type applicability") {
    CHECK(oscFieldApplies(OscType::Va, OscF::width));
    CHECK(!oscFieldApplies(OscType::Fm, OscF::width));
    CHECK(oscFieldApplies(OscType::Fm, OscF::ratio));
    CHECK(!oscFieldApplies(OscType::Sub, OscF::coarse));
    CHECK(oscFieldApplies(OscType::Noise, OscF::tilt));
    CHECK(filterFieldApplies(FilterType::Comb, FilterF::feedback));
    CHECK(!filterFieldApplies(FilterType::Formant, FilterF::cutoff));
    CHECK(waveApplies(OscType::Va, Wave::Pulse));
    CHECK(!waveApplies(OscType::Fm, Wave::Saw));
    CHECK(waveApplies(OscType::Sub, Wave::Square));
    CHECK(!waveApplies(OscType::Sub, Wave::Saw));
    CHECK(defaultWave(OscType::Fm) == Wave::Sine);
    for (int f = 0; f < static_cast<int>(OscF::count); ++f) {
        bool any = false;
        for (int t = 0; t < kOscTypeCount; ++t)
            any = any || oscFieldApplies(static_cast<OscType>(t), static_cast<OscF>(f));
        CHECK(any);
    }
}

TEST_CASE("note values") {
    CHECK(noteQuarters(NoteValue::N1_4) == 1.0);
    CHECK(noteQuarters(NoteValue::N1_8d) == 0.75);
    CHECK(noteQuarters(NoteValue::N1_16t) == doctest::Approx(1.0 / 6.0));
    const auto& sync = table()[index(kDelay, DelayF::sync)];
    CHECK(static_cast<NoteValue>(sync.def) == NoteValue::N1_8d);
    CHECK(writesLabel(index(kDelay, DelayF::sync)));
    CHECK(!writesLabel(index(kDelay, DelayF::time)));
    // Steps have no "off": index 0 is 8/1.
    const auto& stepSync = table()[index(kStep1, StepF::sync)];
    CHECK(stepSync.choices[0].label == "8/1");
    CHECK(stepSync.choices[static_cast<std::size_t>(stepSync.def)].label == "1/16");
}
