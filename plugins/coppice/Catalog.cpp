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
#include "script/coppice.cpp"
}  // namespace supple::script::user

namespace {

using Script = supple::script::user::Coppice;
using Code = supple::script::detail::NativeCode<Script>;

class OneScript final : public supple::Catalog {
  public:
    std::vector<supple::DefinitionPtr> list() override {
        if (!made_) {
            made_ = true;
            auto code = std::make_shared<const Code>(std::vector<Code::Key>{
                {&Script::mode, "sample.mode"},
                {&Script::start, "sample.start"},
                {&Script::end, "sample.end"},
                {&Script::reverse, "sample.reverse"},
                {&Script::normalize, "sample.normalize"},
                {&Script::gain, "sample.gain"},
                {&Script::root, "sample.root"},
                {&Script::transpose, "sample.transpose"},
                {&Script::keytrack, "sample.keytrack"},
                {&Script::loopMode, "loop.mode"},
                {&Script::loopStart, "loop.start"},
                {&Script::loopEnd, "loop.end"},
                {&Script::crossfade, "loop.crossfade"},
                {&Script::snap, "loop.snap"},
                {&Script::playOn, "loop.release"},
                {&Script::sliceBy, "slice.by"},
                {&Script::sensitivity, "slice.sensitivity"},
                {&Script::sliceCount, "slice.count"},
                {&Script::firstKey, "slice.key"},
                {&Script::slicePlay, "slice.play"},
                {&Script::voiceMode, "voice.mode"},
                {&Script::polyphony, "voice.polyphony"},
                {&Script::glide, "voice.glide"},
                {&Script::bendRange, "voice.bend_range"},
                {&Script::tune, "voice.tune"},
                {&Script::quality, "voice.quality"},
                {&Script::fType, "filter.type"},
                {&Script::cutoff, "filter.cutoff"},
                {&Script::resonance, "filter.resonance"},
                {&Script::slope, "filter.slope"},
                {&Script::fMode, "filter.mode"},
                {&Script::fKeytrack, "filter.keytrack"},
                {&Script::fEnv, "filter.env"},
                {&Script::fVelocity, "filter.velocity"},
                {&Script::fDrive, "filter.drive"},
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
                {&Script::m1, "macro1"},
                {&Script::m2, "macro2"},
                {&Script::m3, "macro3"},
                {&Script::m4, "macro4"},
                {&Script::output, "output"},
            }, std::vector<Code::FileKey>{
                {&Script::sample, "sample"},
                {&Script::ampSource, "amp"},
                {&Script::env2Source, "env2"},
                {&Script::lfo1, "lfo1"},
                {&Script::lfo2, "lfo2"},
                {&Script::macro1, "macro1"},
                {&Script::macro2, "macro2"},
                {&Script::macro3, "macro3"},
                {&Script::macro4, "macro4"},
            });
            std::string problems;
            definition_ = supple::suite::makeScriptDefinition(std::move(code), {}, {}, &problems);
            if (!definition_) std::fprintf(stderr, "Coppice: %s\n", problems.c_str());
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
