// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
#include <doctest/doctest.h>

#include <string>

#include "orrery/patch.h"

using namespace supple;
using namespace supple::orrery;

namespace {

const PatchType& type() {
    static const PatchType t;
    return t;
}

// The example from synth.md §3.
constexpr std::string_view kPad = R"({
  "schema": "supple.orrery-patch/1.0",
  "name": "Warm Evolving Pad",
  "tags": ["pad", "warm"],
  "notes": "Slow filter bloom; macro1 opens it up.",
  "seed": 1234,
  "voice": {
    "mode": "poly", "polyphony": 8,
    "unison": 3, "unison_detune": "12 ct", "unison_spread": "70 %",
    "glide": "0 ms", "glide_mode": "legato",
    "bend_range": "2 st", "drift": "10 %", "quality": "normal"
  },
  "oscillators": {
    "osc1": { "type": "va", "wave": "saw", "level": "-6 dB", "fine": "-7 ct", "to": "filter1" },
    "osc2": { "type": "va", "wave": "saw", "level": "-6 dB", "fine": "+7 ct", "to": "filter1" },
    "osc3": { "type": "sub", "wave": "sine", "octave": -1, "level": "-12 dB", "to": "filter1" }
  },
  "filters": {
    "routing": "serial",
    "filter1": { "type": "ladder", "slope": "24 dB", "cutoff": "1.2 kHz", "resonance": "25 %", "keytrack": "50 %" }
  },
  "drive": { "type": "tape", "amount": "3 dB", "mix": "100 %" },
  "amp": { "level": "-3 dB", "velocity": "40 %", "pan": "0 %" },
  "envelopes": {
    "amp":  { "attack": "600 ms", "decay": "1.2 s", "sustain": "80 %", "release": "1.5 s" },
    "env2": { "attack": "1.5 s", "decay": "3 s", "sustain": "30 %", "release": "2 s" }
  },
  "lfos": {
    "lfo1": { "wave": "sine", "rate": "0.2 Hz", "retrigger": "free" }
  },
  "steps": {},
  "routes": {
    "env2>filter1.cutoff":   { "amount": "+2 oct" },
    "lfo1>osc1.fine":        { "amount": "8 ct" },
    "macro1>filter1.cutoff": { "amount": "+3 oct" },
    "velocity>filter1.cutoff": { "amount": "+1 oct" }
  },
  "macros": { "macro1": { "name": "Brightness", "value": "20 %" } },
  "fx": {
    "chain": ["chorus", "reverb"],
    "chorus": { "rate": "0.4 Hz", "depth": "30 %", "mix": "35 %" },
    "reverb": { "size": "70 %", "decay": "3.5 s", "damping": "40 %", "mix": "25 %" }
  },
  "output": { "level": "0 dB" }
})";

double value(const Patch& p, Slot s, auto field) { return p.body.values[index(s, field)]; }

Error parseError(std::string_view body) {
    auto r = type().parse(std::string(R"({"schema": "supple.orrery-patch/1.0", )") +
                          std::string(body) + "}");
    REQUIRE_FALSE(r);
    return r.error();
}

bool mentions(const Error& e, std::string_view code, std::string_view path = {}) {
    if (e.code == code && (path.empty() || e.path == path)) return true;
    for (const auto& d : e.details)
        if (mentions(d, code, path)) return true;
    return false;
}

std::string all(const Error& e) {
    std::string s = e.code + " " + e.message + " @" + e.path + " hint: " + e.hint + "\n";
    for (const auto& d : e.details) s += all(d);
    return s;
}

}  // namespace

TEST_CASE("the spec's example pad parses with physical values") {
    auto p = type().parse(kPad);
    REQUIRE_MESSAGE(p, (p ? "" : all(p.error())));
    const Structure& st = p->body.structure;
    CHECK(p->meta.name == "Warm Evolving Pad");
    CHECK(p->meta.seed == 1234u);
    CHECK(st.osc[0] == OscType::Va);
    CHECK(st.osc[2] == OscType::Sub);
    CHECK(!st.osc[3]);
    CHECK(st.filter[0] == FilterType::Ladder);
    CHECK(st.drive == DriveType::Tape);
    CHECK(st.env[1]);
    CHECK(st.lfo[0]);
    CHECK(st.lfoRetrigger[0] == Retrigger::Free);
    CHECK(st.macroName[0] == "Brightness");
    REQUIRE(st.chain.size() == 2);
    CHECK(st.chain[1] == FxKind::Reverb);
    CHECK(value(*p, kFilter1, FilterF::cutoff) == 1200.0);
    CHECK(value(*p, kEnvAmp, EnvF::decay) == 1200.0);
    CHECK(value(*p, kOsc1, OscF::fine) == -7.0);
    CHECK(value(*p, kVoice, VoiceF::unison) == 3.0);
    CHECK(value(*p, kReverb, ReverbF::decay) == 3500.0);
    CHECK(value(*p, kMacro1, MacroF::value) == 20.0);
    REQUIRE(st.routes.size() == 4);
    // Canonical route order: by source, then destination.
    CHECK(st.routes[0].source == Source::Env2);
    CHECK(st.routes[0].amount == 2.0);
    CHECK(st.routes[1].source == Source::Lfo1);
    CHECK(st.routes[1].amount == doctest::Approx(0.08));  // 8 ct in semitones
    CHECK(st.routes[2].source == Source::Velocity);
    CHECK(st.routes[3].source == Source::Macro1);
    CHECK(modUnit(st.routes[0].dest) == units::Unit::oct);
    CHECK(modUnit(st.routes[1].dest) == units::Unit::st);
}

TEST_CASE("round trips: full and minimal forms parse back to the same patch") {
    auto p = type().parse(kPad);
    REQUIRE(p);
    for (auto detail : {patch::Detail::Full, patch::Detail::Changed}) {
        const std::string text = type().serialize(*p, detail);
        auto q = type().parse(text);
        REQUIRE_MESSAGE(q, (q ? "" : all(q.error())));
        CHECK(q->body == p->body);
        CHECK(q->meta == p->meta);
        CHECK(type().serialize(*q, detail) == text);
    }
    const std::string minimal = type().serialize(*p, patch::Detail::Changed);
    CHECK(minimal.find("\"slope\"") == std::string::npos);  // default 24 dB
    CHECK(minimal.find("\"to\"") == std::string::npos);     // default filter1
    CHECK(minimal.find("\"env2>filter1.cutoff\"") != std::string::npos);
    const std::string full = type().serialize(*p, patch::Detail::Full);
    CHECK(full.find("\"slope\": \"24 dB\"") != std::string::npos);  // written as its label
    CHECK(full.find("\"sync\": \"1/8.\"") == std::string::npos);    // delay not in the chain
    CHECK(full.find("\"ratio\"") == std::string::npos);  // fm-only field; no fm oscillator
}

TEST_CASE("defaults: one saw, and an empty document is valid") {
    const Patch d = type().defaults();
    CHECK(d.body.structure.osc[0] == OscType::Va);
    auto empty = type().parse(R"({"schema": "supple.orrery-patch/1.0"})");
    REQUIRE(empty);
    CHECK(!empty->body.structure.osc[0]);
    auto fm = type().parse(
        R"({"schema": "supple.orrery-patch/1.0", "oscillators": {"osc1": {"type": "fm"}}})");
    REQUIRE(fm);
    CHECK(static_cast<Wave>(value(*fm, kOsc1, OscF::wave)) == Wave::Sine);  // per-type default
    CHECK(type().serialize(*fm, patch::Detail::Changed).find("\"wave\"") == std::string::npos);
}

TEST_CASE("validation errors carry paths and hints") {
    SUBCASE("a field the oscillator type does not have") {
        auto e = parseError(R"("oscillators": {"osc1": {"type": "fm", "width": 30}})");
        CHECK(e.code == "orrery.field_not_for_type");
        CHECK(e.path == "/oscillators/osc1/width");
        CHECK(e.hint.find("ratio") != std::string::npos);
    }
    SUBCASE("unknown field with a closest match") {
        auto e = parseError(R"("filters": {"filter1": {"type": "svf", "cutof": 300}})");
        CHECK(e.code == "orrery.unknown_field");
        CHECK(e.hint.find("did you mean \"cutoff\"") != std::string::npos);
    }
    SUBCASE("missing type") {
        auto e = parseError(R"("oscillators": {"osc2": {"wave": "saw"}})");
        CHECK(e.code == "orrery.missing_type");
        CHECK(e.path == "/oscillators/osc2/type");
    }
    SUBCASE("wave not available for the type") {
        auto e = parseError(R"("oscillators": {"osc1": {"type": "sub", "wave": "saw"}})");
        CHECK(e.code == "orrery.wave_not_for_type");
        CHECK(e.hint.find("triangle") != std::string::npos);
    }
    SUBCASE("route to a slot that does not exist") {
        auto e = parseError(R"("oscillators": {"osc1": {"type": "va"}},
                               "envelopes": {"env2": {}},
                               "routes": {"env2>osc3.level": {"amount": "-6 dB"}})");
        CHECK(e.code == "orrery.route.dest_missing");
        CHECK(e.path == "/routes/env2>osc3.level");
        CHECK(e.hint.find("oscillators") != std::string::npos);
    }
    SUBCASE("route from a missing envelope") {
        auto e = parseError(R"("oscillators": {"osc1": {"type": "va"}},
                               "routes": {"env3>osc1.level": {"amount": "-6 dB"}})");
        CHECK(e.code == "orrery.route.source_missing");
    }
    SUBCASE("wrong unit names the right one") {
        auto e = parseError(R"("filters": {"filter1": {"type": "ladder"}},
                               "envelopes": {"env2": {}},
                               "routes": {"env2>filter1.cutoff": {"amount": "500 Hz"}})");
        CHECK(e.code == "orrery.route.unit");
        CHECK(e.message.find("oct") != std::string::npos);
        CHECK(e.hint.find("+1.5 oct") != std::string::npos);
    }
    SUBCASE("voice source to a global destination") {
        auto e = parseError(R"("fx": {"chain": ["reverb"]},
                               "routes": {"velocity>fx.reverb.mix": {"amount": "20 %"}})");
        CHECK(e.code == "orrery.route.voice_to_global");
    }
    SUBCASE("stepped destinations are not modulatable") {
        auto e = parseError(R"("oscillators": {"osc1": {"type": "va"}},
                               "routes": {"modwheel>osc1.coarse": {"amount": "12 st"}})");
        CHECK(e.code == "orrery.route.not_modulatable");
        CHECK(e.hint.find("osc1.pitch") != std::string::npos);
    }
    SUBCASE("oscillators only modulate fm/am inputs, which only take oscillators") {
        auto e1 = parseError(R"("oscillators": {"osc1": {"type": "va"}, "osc2": {"type": "fm"}},
                                "routes": {"osc2>osc1.level": {"amount": "3 dB"}})");
        CHECK(e1.code == "orrery.route.audio_source");
        auto e2 = parseError(R"("oscillators": {"osc1": {"type": "fm"}}, "envelopes": {"env2": {}},
                                "routes": {"env2>osc1.fm": {"amount": 2}})");
        CHECK(e2.code == "orrery.route.audio_dest");
        CHECK(e2.hint.find("level") != std::string::npos);
    }
    SUBCASE("FX blocks must be in the chain") {
        auto e = parseError(R"("fx": {"chain": ["chorus"], "reverb": {"mix": 30}})");
        CHECK(e.code == "orrery.fx.not_in_chain");
    }
    SUBCASE("explicit target filter must exist") {
        auto e = parseError(R"("oscillators": {"osc1": {"type": "va", "to": "filter2"}},
                               "filters": {"filter1": {"type": "svf"}})");
        CHECK(e.code == "orrery.target_missing");
    }
    SUBCASE("several problems are reported together") {
        auto e = parseError(R"("oscilators": {}, "amp": {"level": 99}, "fx": {"chain": ["verb"]})");
        CHECK(e.code == "orrery.patch.invalid");
        CHECK(e.details.size() == 3);
        CHECK(mentions(e, "orrery.unknown_block", "/oscilators"));
        CHECK(mentions(e, "orrery.unknown_choice", "/fx/chain/0"));
    }
}

TEST_CASE("FM routes and modulator-only oscillators") {
    auto p = type().parse(R"({"schema": "supple.orrery-patch/1.0",
        "oscillators": {"osc1": {"type": "fm"}, "osc2": {"type": "fm", "ratio": 2, "to": "none"}},
        "envelopes": {"env2": {"decay": "400 ms", "sustain": 0}},
        "routes": {"osc2>osc1.fm": {"amount": 3}, "env2>osc2.level": {"amount": "12 dB"}}})");
    REQUIRE_MESSAGE(p, (p ? "" : all(p.error())));
    CHECK(p->body.structure.to[1] == OscTarget::None);
    REQUIRE(p->body.structure.routes.size() == 2);
    CHECK(p->body.structure.routes[0].source == Source::Env2);  // canonical order: by source
    CHECK(p->body.structure.routes[1].dest.kind == Dest::Kind::Fm);
    CHECK(describe(*p).find("(modulator)") != std::string::npos);
}

TEST_CASE("merge: type changes drop the old type's fields; FX leaving the chain drop blocks") {
    auto p = type().parse(kPad);
    REQUIRE(p);
    auto m = json::parse(R"({"oscillators": {"osc1": {"type": "fm", "ratio": 3}},
                             "fx": {"chain": ["reverb"]}})");
    REQUIRE(m);
    auto q = type().merge(*p, *m);
    REQUIRE_MESSAGE(q, (q ? "" : all(q.error())));
    CHECK(q->body.structure.osc[0] == OscType::Fm);
    CHECK(value(*q, kOsc1, OscF::ratio) == 3.0);
    CHECK(static_cast<Wave>(value(*q, kOsc1, OscF::wave)) == Wave::Sine);
    CHECK(value(*q, kOsc1, OscF::fine) == -7.0);  // common fields survive
    REQUIRE(q->body.structure.chain.size() == 1);
    CHECK(value(*q, kReverb, ReverbF::decay) == 3500.0);
    CHECK(value(*q, kChorus, ChorusF::mix) == 50.0);  // back to its default
    // A bad merge changes nothing and reports the merged document's path.
    auto bad = json::parse(R"({"filters": {"filter1": {"cutoff": "2 kHz", "slope": "36 dB"}}})");
    auto r = type().merge(*p, *bad);
    REQUIRE_FALSE(r);
    CHECK(r.error().path == "/filters/filter1/slope");
    // Removing a slot that a route uses is an error until the route goes too.
    auto drop = json::parse(R"({"envelopes": {"env2": null}})");
    CHECK_FALSE(type().merge(*p, *drop));
    auto drop2 =
        json::parse(R"({"envelopes": {"env2": null}, "routes": {"env2>filter1.cutoff": null}})");
    CHECK(type().merge(*p, *drop2));
}

TEST_CASE("diff is a merge patch between minimal forms") {
    auto a = type().parse(kPad);
    REQUIRE(a);
    auto m = json::parse(
        R"({"filters": {"filter1": {"cutoff": 800}}, "routes": {"lfo1>osc1.fine": null}})");
    auto b = type().merge(*a, *m);
    REQUIRE(b);
    const json::Value d = type().diff(*a, *b);
    auto c = type().merge(*a, d);
    REQUIRE(c);
    CHECK(c->body == b->body);
}

TEST_CASE("describe stays short and names the structure") {
    auto p = type().parse(kPad);
    REQUIRE(p);
    const std::string d = describe(*p);
    INFO(d);
    CHECK(d.size() <= 600);
    CHECK(d.find("Poly, 3-voice unison") != std::string::npos);
    CHECK(d.find("osc3 sub sine -1 oct") != std::string::npos);
    CHECK(d.find("ladder 24 dB LP") != std::string::npos);
    CHECK(d.find("macro1 \"Brightness\" → filter1.cutoff +3") != std::string::npos);
    CHECK(d.find("FX: chorus") != std::string::npos);
}

TEST_CASE("the JSON Schema covers every block") {
    const json::Value s = type().jsonSchema();
    const json::Value* props = s.find("properties");
    REQUIRE(props);
    for (auto block : {"schema", "voice", "oscillators", "filters", "drive", "amp", "envelopes",
                       "lfos", "steps", "routes", "macros", "fx", "output"})
        CHECK_MESSAGE(props->find(block) != nullptr, block);
}

TEST_CASE("the default patch round-trips in both forms") {
    const Patch d = type().defaults();
    for (auto detail : {patch::Detail::Full, patch::Detail::Changed}) {
        const std::string text = type().serialize(d, detail);
        auto q = type().parse(text);
        REQUIRE_MESSAGE(q, (q ? "" : all(q.error()) + text));
        CHECK(q->body == d.body);
    }
}

TEST_CASE("flat params in merges set values by key") {
    auto p = type().parse(kPad);
    REQUIRE(p);
    auto m =
        json::parse(R"({"params": {"filter1.cutoff": "2 kHz", "amp.attack": 20, "macro1": 60}})");
    auto q = type().merge(*p, *m);
    REQUIRE_MESSAGE(q, (q ? "" : all(q.error())));
    CHECK(value(*q, kFilter1, FilterF::cutoff) == 2000.0);
    CHECK(value(*q, kEnvAmp, EnvF::attack) == 20.0);
    CHECK(value(*q, kMacro1, MacroF::value) == 60.0);
    auto unknown = type().merge(*p, *json::parse(R"({"params": {"filter1.cutof": 1}})"));
    REQUIRE_FALSE(unknown);
    CHECK(unknown.error().code == "orrery.unknown_param");
    CHECK(unknown.error().hint.find("filter1.cutoff") != std::string::npos);
    auto inactive = type().merge(*p, *json::parse(R"({"params": {"osc4.level": -3}})"));
    REQUIRE_FALSE(inactive);
    CHECK(inactive.error().code == "orrery.param_inactive");
    CHECK(inactive.error().path == "/params/osc4.level");
    CHECK(paramActive(p->body.structure, index(kFilter1, FilterF::cutoff)));
    CHECK_FALSE(paramActive(p->body.structure, index(kFilter1, FilterF::vowel)));  // ladder
    CHECK(patchPath(index(kReverb, ReverbF::mix)) ==
          std::vector<std::string>{"fx", "reverb", "mix"});
    CHECK(patchPath(index(kEnv2, EnvF::decay)) ==
          std::vector<std::string>{"envelopes", "env2", "decay"});
}

TEST_CASE(
    "review round 3: drive off takes no fields; steps need values; no curves on audio routes") {
    const Error e1 = parseError(R"("drive": {"type": "off", "amount": 99, "bogus": 1})");
    CHECK_MESSAGE(mentions(e1, "orrery.field_not_for_type", "/drive/amount"), all(e1));
    CHECK_MESSAGE(mentions(e1, "orrery.field_not_for_type", "/drive/bogus"), all(e1));
    for (const auto& detail : e1.details) {
        if (detail.path == "/drive/amount")
            CHECK(detail.message == "\"amount\" does nothing while drive is off");
        if (detail.path == "/drive/bogus")
            CHECK(detail.message == "\"bogus\" does nothing while drive is off");
    }
    const Error e2 = parseError(R"("steps": {"step1": {"values": []}})");
    CHECK_MESSAGE(mentions(e2, "orrery.steps.empty", "/steps/step1/values"), all(e2));
    const Error e3 =
        parseError(R"("oscillators": {"osc1": {"type": "va"}, "osc2": {"type": "va", "to": "none"}},
                            "routes": {"osc2>osc1.am": {"amount": 100, "curve": 50}})");
    CHECK_MESSAGE(mentions(e3, "orrery.route.curve"), all(e3));
}
