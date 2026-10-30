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
#include "script/drumlin.cpp"
}  // namespace supple::script::user

namespace {

using Script = supple::script::user::Drumlin;
using Code = supple::script::detail::NativeCode<Script>;

class OneScript final : public supple::Catalog {
  public:
    std::vector<supple::DefinitionPtr> list() override {
        if (!made_) {
            made_ = true;
            auto code = std::make_shared<const Code>(std::vector<Code::Key>{
                {&Script::p1Key, "pad1.key"},
                {&Script::p1Sound, "pad1.sound"},
                {&Script::p1Tune, "pad1.tune"},
                {&Script::p1Decay, "pad1.decay"},
                {&Script::p1Level, "pad1.level"},
                {&Script::p1Pan, "pad1.pan"},
                {&Script::p1Velocity, "pad1.velocity"},
                {&Script::p1Vary, "pad1.vary"},
                {&Script::p1Choke, "pad1.choke"},
                {&Script::p1Output, "pad1.output"},
                {&Script::p2Key, "pad2.key"},
                {&Script::p2Sound, "pad2.sound"},
                {&Script::p2Tune, "pad2.tune"},
                {&Script::p2Decay, "pad2.decay"},
                {&Script::p2Level, "pad2.level"},
                {&Script::p2Pan, "pad2.pan"},
                {&Script::p2Velocity, "pad2.velocity"},
                {&Script::p2Vary, "pad2.vary"},
                {&Script::p2Choke, "pad2.choke"},
                {&Script::p2Output, "pad2.output"},
                {&Script::p3Key, "pad3.key"},
                {&Script::p3Sound, "pad3.sound"},
                {&Script::p3Tune, "pad3.tune"},
                {&Script::p3Decay, "pad3.decay"},
                {&Script::p3Level, "pad3.level"},
                {&Script::p3Pan, "pad3.pan"},
                {&Script::p3Velocity, "pad3.velocity"},
                {&Script::p3Vary, "pad3.vary"},
                {&Script::p3Choke, "pad3.choke"},
                {&Script::p3Output, "pad3.output"},
                {&Script::p4Key, "pad4.key"},
                {&Script::p4Sound, "pad4.sound"},
                {&Script::p4Tune, "pad4.tune"},
                {&Script::p4Decay, "pad4.decay"},
                {&Script::p4Level, "pad4.level"},
                {&Script::p4Pan, "pad4.pan"},
                {&Script::p4Velocity, "pad4.velocity"},
                {&Script::p4Vary, "pad4.vary"},
                {&Script::p4Choke, "pad4.choke"},
                {&Script::p4Output, "pad4.output"},
                {&Script::p5Key, "pad5.key"},
                {&Script::p5Sound, "pad5.sound"},
                {&Script::p5Tune, "pad5.tune"},
                {&Script::p5Decay, "pad5.decay"},
                {&Script::p5Level, "pad5.level"},
                {&Script::p5Pan, "pad5.pan"},
                {&Script::p5Velocity, "pad5.velocity"},
                {&Script::p5Vary, "pad5.vary"},
                {&Script::p5Choke, "pad5.choke"},
                {&Script::p5Output, "pad5.output"},
                {&Script::p6Key, "pad6.key"},
                {&Script::p6Sound, "pad6.sound"},
                {&Script::p6Tune, "pad6.tune"},
                {&Script::p6Decay, "pad6.decay"},
                {&Script::p6Level, "pad6.level"},
                {&Script::p6Pan, "pad6.pan"},
                {&Script::p6Velocity, "pad6.velocity"},
                {&Script::p6Vary, "pad6.vary"},
                {&Script::p6Choke, "pad6.choke"},
                {&Script::p6Output, "pad6.output"},
                {&Script::p7Key, "pad7.key"},
                {&Script::p7Sound, "pad7.sound"},
                {&Script::p7Tune, "pad7.tune"},
                {&Script::p7Decay, "pad7.decay"},
                {&Script::p7Level, "pad7.level"},
                {&Script::p7Pan, "pad7.pan"},
                {&Script::p7Velocity, "pad7.velocity"},
                {&Script::p7Vary, "pad7.vary"},
                {&Script::p7Choke, "pad7.choke"},
                {&Script::p7Output, "pad7.output"},
                {&Script::p8Key, "pad8.key"},
                {&Script::p8Sound, "pad8.sound"},
                {&Script::p8Tune, "pad8.tune"},
                {&Script::p8Decay, "pad8.decay"},
                {&Script::p8Level, "pad8.level"},
                {&Script::p8Pan, "pad8.pan"},
                {&Script::p8Velocity, "pad8.velocity"},
                {&Script::p8Vary, "pad8.vary"},
                {&Script::p8Choke, "pad8.choke"},
                {&Script::p8Output, "pad8.output"},
                {&Script::p9Key, "pad9.key"},
                {&Script::p9Sound, "pad9.sound"},
                {&Script::p9Tune, "pad9.tune"},
                {&Script::p9Decay, "pad9.decay"},
                {&Script::p9Level, "pad9.level"},
                {&Script::p9Pan, "pad9.pan"},
                {&Script::p9Velocity, "pad9.velocity"},
                {&Script::p9Vary, "pad9.vary"},
                {&Script::p9Choke, "pad9.choke"},
                {&Script::p9Output, "pad9.output"},
                {&Script::p10Key, "pad10.key"},
                {&Script::p10Sound, "pad10.sound"},
                {&Script::p10Tune, "pad10.tune"},
                {&Script::p10Decay, "pad10.decay"},
                {&Script::p10Level, "pad10.level"},
                {&Script::p10Pan, "pad10.pan"},
                {&Script::p10Velocity, "pad10.velocity"},
                {&Script::p10Vary, "pad10.vary"},
                {&Script::p10Choke, "pad10.choke"},
                {&Script::p10Output, "pad10.output"},
                {&Script::p11Key, "pad11.key"},
                {&Script::p11Sound, "pad11.sound"},
                {&Script::p11Tune, "pad11.tune"},
                {&Script::p11Decay, "pad11.decay"},
                {&Script::p11Level, "pad11.level"},
                {&Script::p11Pan, "pad11.pan"},
                {&Script::p11Velocity, "pad11.velocity"},
                {&Script::p11Vary, "pad11.vary"},
                {&Script::p11Choke, "pad11.choke"},
                {&Script::p11Output, "pad11.output"},
                {&Script::p12Key, "pad12.key"},
                {&Script::p12Sound, "pad12.sound"},
                {&Script::p12Tune, "pad12.tune"},
                {&Script::p12Decay, "pad12.decay"},
                {&Script::p12Level, "pad12.level"},
                {&Script::p12Pan, "pad12.pan"},
                {&Script::p12Velocity, "pad12.velocity"},
                {&Script::p12Vary, "pad12.vary"},
                {&Script::p12Choke, "pad12.choke"},
                {&Script::p12Output, "pad12.output"},
                {&Script::p13Key, "pad13.key"},
                {&Script::p13Sound, "pad13.sound"},
                {&Script::p13Tune, "pad13.tune"},
                {&Script::p13Decay, "pad13.decay"},
                {&Script::p13Level, "pad13.level"},
                {&Script::p13Pan, "pad13.pan"},
                {&Script::p13Velocity, "pad13.velocity"},
                {&Script::p13Vary, "pad13.vary"},
                {&Script::p13Choke, "pad13.choke"},
                {&Script::p13Output, "pad13.output"},
                {&Script::p14Key, "pad14.key"},
                {&Script::p14Sound, "pad14.sound"},
                {&Script::p14Tune, "pad14.tune"},
                {&Script::p14Decay, "pad14.decay"},
                {&Script::p14Level, "pad14.level"},
                {&Script::p14Pan, "pad14.pan"},
                {&Script::p14Velocity, "pad14.velocity"},
                {&Script::p14Vary, "pad14.vary"},
                {&Script::p14Choke, "pad14.choke"},
                {&Script::p14Output, "pad14.output"},
                {&Script::p15Key, "pad15.key"},
                {&Script::p15Sound, "pad15.sound"},
                {&Script::p15Tune, "pad15.tune"},
                {&Script::p15Decay, "pad15.decay"},
                {&Script::p15Level, "pad15.level"},
                {&Script::p15Pan, "pad15.pan"},
                {&Script::p15Velocity, "pad15.velocity"},
                {&Script::p15Vary, "pad15.vary"},
                {&Script::p15Choke, "pad15.choke"},
                {&Script::p15Output, "pad15.output"},
                {&Script::p16Key, "pad16.key"},
                {&Script::p16Sound, "pad16.sound"},
                {&Script::p16Tune, "pad16.tune"},
                {&Script::p16Decay, "pad16.decay"},
                {&Script::p16Level, "pad16.level"},
                {&Script::p16Pan, "pad16.pan"},
                {&Script::p16Velocity, "pad16.velocity"},
                {&Script::p16Vary, "pad16.vary"},
                {&Script::p16Choke, "pad16.choke"},
                {&Script::p16Output, "pad16.output"},
                {&Script::output, "output"},
            }, std::vector<Code::FileKey>{

            });
            std::string problems;
            definition_ = supple::suite::makeScriptDefinition(std::move(code), {}, {}, &problems);
            if (!definition_) std::fprintf(stderr, "Drumlin: %s\n", problems.c_str());
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
