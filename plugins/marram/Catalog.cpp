// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Supple Audio
//
// The plugin this product offers (core/Catalog.h): the script in
// product/script/, compiled as C++ (supple/script_native.h) and run by the
// suite's glue as the script player runs it.
//
// Below, each Param member's patch key. C++ cannot see a member's name,
// which a key is made from, so they are listed by member; a Param missing
// from the list takes the key withKey() gives, or else its name's in
// snake_case. List a Param you add, under the key its member's name gives
// (lowCut: low_cut), and keep a key once sessions use it.
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/Catalog.h"
#include "supple/script_native.h"
#include "supple/suite/script_definition.h"

namespace supple::script::user {
#include "script/marram.cpp"
}  // namespace supple::script::user

namespace {

using Script = supple::script::user::Marram;
using Code = supple::script::detail::NativeCode<Script>;

class OneScript final : public supple::Catalog {
  public:
    std::vector<supple::DefinitionPtr> list() override {
        if (!made_) {
            made_ = true;
            auto code = std::make_shared<const Code>(std::vector<Code::Key>{
                {&Script::mode, "voice.mode"},
                {&Script::polyphony, "voice.polyphony"},
                {&Script::glide, "voice.glide"},
                {&Script::bendRange, "voice.bend_range"},
                {&Script::tune, "voice.tune"},
                {&Script::unison, "voice.unison"},
                {&Script::detune, "voice.unison_detune"},
                {&Script::spread, "voice.unison_spread"},
                {&Script::drift, "voice.drift"},
                {&Script::oversampling, "voice.oversampling"},
                {&Script::o1Type, "osc1.type"},
                {&Script::o1Level, "osc1.level"},
                {&Script::o1Pan, "osc1.pan"},
                {&Script::o1Coarse, "osc1.coarse"},
                {&Script::o1Fine, "osc1.fine"},
                {&Script::o1Wave, "osc1.wave"},
                {&Script::o1Width, "osc1.width"},
                {&Script::o1Table, "osc1.table"},
                {&Script::o1Position, "osc1.position"},
                {&Script::o1Ratio, "osc1.ratio"},
                {&Script::o1Feedback, "osc1.feedback"},
                {&Script::o1Color, "osc1.color"},
                {&Script::o1Octave, "osc1.octave"},
                {&Script::o1Fm, "osc1.fm"},
                {&Script::o1FmFrom, "osc1.fm_from"},
                {&Script::o1To, "osc1.to"},
                {&Script::o1Retrigger, "osc1.retrigger"},
                {&Script::o2Type, "osc2.type"},
                {&Script::o2Level, "osc2.level"},
                {&Script::o2Pan, "osc2.pan"},
                {&Script::o2Coarse, "osc2.coarse"},
                {&Script::o2Fine, "osc2.fine"},
                {&Script::o2Wave, "osc2.wave"},
                {&Script::o2Width, "osc2.width"},
                {&Script::o2Sync, "osc2.sync"},
                {&Script::o2Table, "osc2.table"},
                {&Script::o2Position, "osc2.position"},
                {&Script::o2Ratio, "osc2.ratio"},
                {&Script::o2Feedback, "osc2.feedback"},
                {&Script::o2Color, "osc2.color"},
                {&Script::o2Octave, "osc2.octave"},
                {&Script::o2Fm, "osc2.fm"},
                {&Script::o2FmFrom, "osc2.fm_from"},
                {&Script::o2To, "osc2.to"},
                {&Script::o2Retrigger, "osc2.retrigger"},
                {&Script::o3Type, "osc3.type"},
                {&Script::o3Level, "osc3.level"},
                {&Script::o3Pan, "osc3.pan"},
                {&Script::o3Coarse, "osc3.coarse"},
                {&Script::o3Fine, "osc3.fine"},
                {&Script::o3Wave, "osc3.wave"},
                {&Script::o3Width, "osc3.width"},
                {&Script::o3Sync, "osc3.sync"},
                {&Script::o3Table, "osc3.table"},
                {&Script::o3Position, "osc3.position"},
                {&Script::o3Ratio, "osc3.ratio"},
                {&Script::o3Feedback, "osc3.feedback"},
                {&Script::o3Color, "osc3.color"},
                {&Script::o3Octave, "osc3.octave"},
                {&Script::o3Fm, "osc3.fm"},
                {&Script::o3FmFrom, "osc3.fm_from"},
                {&Script::o3To, "osc3.to"},
                {&Script::o3Retrigger, "osc3.retrigger"},
                {&Script::o4Type, "osc4.type"},
                {&Script::o4Level, "osc4.level"},
                {&Script::o4Pan, "osc4.pan"},
                {&Script::o4Coarse, "osc4.coarse"},
                {&Script::o4Fine, "osc4.fine"},
                {&Script::o4Wave, "osc4.wave"},
                {&Script::o4Width, "osc4.width"},
                {&Script::o4Sync, "osc4.sync"},
                {&Script::o4Table, "osc4.table"},
                {&Script::o4Position, "osc4.position"},
                {&Script::o4Ratio, "osc4.ratio"},
                {&Script::o4Feedback, "osc4.feedback"},
                {&Script::o4Color, "osc4.color"},
                {&Script::o4Octave, "osc4.octave"},
                {&Script::o4Fm, "osc4.fm"},
                {&Script::o4FmFrom, "osc4.fm_from"},
                {&Script::o4To, "osc4.to"},
                {&Script::o4Retrigger, "osc4.retrigger"},
                {&Script::f1Type, "filter1.type"},
                {&Script::f1Cutoff, "filter1.cutoff"},
                {&Script::f1Resonance, "filter1.resonance"},
                {&Script::f1Slope, "filter1.slope"},
                {&Script::f1Mode, "filter1.mode"},
                {&Script::f1Feedback, "filter1.feedback"},
                {&Script::f1Damping, "filter1.damping"},
                {&Script::f1Vowel, "filter1.vowel"},
                {&Script::f1Shift, "filter1.shift"},
                {&Script::f1Keytrack, "filter1.keytrack"},
                {&Script::f1Env, "filter1.env"},
                {&Script::f1Velocity, "filter1.velocity"},
                {&Script::f1Drive, "filter1.drive"},
                {&Script::f2Type, "filter2.type"},
                {&Script::f2Cutoff, "filter2.cutoff"},
                {&Script::f2Resonance, "filter2.resonance"},
                {&Script::f2Slope, "filter2.slope"},
                {&Script::f2Mode, "filter2.mode"},
                {&Script::f2Feedback, "filter2.feedback"},
                {&Script::f2Damping, "filter2.damping"},
                {&Script::f2Vowel, "filter2.vowel"},
                {&Script::f2Shift, "filter2.shift"},
                {&Script::f2Keytrack, "filter2.keytrack"},
                {&Script::f2Env, "filter2.env"},
                {&Script::f2Velocity, "filter2.velocity"},
                {&Script::f2Drive, "filter2.drive"},
                {&Script::routing, "filters.routing"},
                {&Script::ampAttack, "amp.attack"},
                {&Script::ampDecay, "amp.decay"},
                {&Script::ampSustain, "amp.sustain"},
                {&Script::ampRelease, "amp.release"},
                {&Script::ampVelocity, "amp.velocity"},
                {&Script::ampLevel, "amp.level"},
                {&Script::ampPan, "amp.pan"},
                {&Script::envAttack, "env2.attack"},
                {&Script::envDecay, "env2.decay"},
                {&Script::envSustain, "env2.sustain"},
                {&Script::envRelease, "env2.release"},
                {&Script::envPitch, "env2.pitch"},
                {&Script::envFm, "env2.fm"},
                {&Script::env3Attack, "env3.attack"},
                {&Script::env3Decay, "env3.decay"},
                {&Script::env3Sustain, "env3.sustain"},
                {&Script::env3Release, "env3.release"},
                {&Script::l1Shape, "lfo1.shape"},
                {&Script::l1Sync, "lfo1.sync"},
                {&Script::l1Rate, "lfo1.rate"},
                {&Script::l1Division, "lfo1.division"},
                {&Script::l1Mode, "lfo1.mode"},
                {&Script::l1Phase, "lfo1.phase"},
                {&Script::l1Delay, "lfo1.delay"},
                {&Script::l2Shape, "lfo2.shape"},
                {&Script::l2Sync, "lfo2.sync"},
                {&Script::l2Rate, "lfo2.rate"},
                {&Script::l2Division, "lfo2.division"},
                {&Script::l2Mode, "lfo2.mode"},
                {&Script::l2Phase, "lfo2.phase"},
                {&Script::l2Delay, "lfo2.delay"},
                {&Script::l3Shape, "lfo3.shape"},
                {&Script::l3Sync, "lfo3.sync"},
                {&Script::l3Rate, "lfo3.rate"},
                {&Script::l3Division, "lfo3.division"},
                {&Script::l3Mode, "lfo3.mode"},
                {&Script::l3Phase, "lfo3.phase"},
                {&Script::l3Delay, "lfo3.delay"},
                {&Script::m1, "macro1"},
                {&Script::m2, "macro2"},
                {&Script::m3, "macro3"},
                {&Script::m4, "macro4"},
                {&Script::output, "output"},
            }, std::vector<Code::FileKey>{
                {&Script::ampSource, "amp"},
                {&Script::env2Source, "env2"},
                {&Script::env3Source, "env3"},
                {&Script::lfo1, "lfo1"},
                {&Script::lfo2, "lfo2"},
                {&Script::lfo3, "lfo3"},
                {&Script::macro1, "macro1"},
                {&Script::macro2, "macro2"},
                {&Script::macro3, "macro3"},
                {&Script::macro4, "macro4"},
            });
            std::string problems;
            definition_ = supple::suite::makeScriptDefinition(std::move(code), {}, {}, &problems);
            if (!definition_) std::fprintf(stderr, "Marram: %s\n", problems.c_str());
        }
        if (!definition_) return {};
        return {definition_};
    }

  private:
    bool made_ = false;
    supple::DefinitionPtr definition_;
};

}  // namespace

std::unique_ptr<supple::Catalog> product::makeCatalog() { return std::make_unique<OneScript>(); }
